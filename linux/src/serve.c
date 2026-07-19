#define _POSIX_C_SOURCE 200809L

#include "serve.h"

#include "cli_cost.h"
#include "cli_usage.h"
#include "provider_registry.h"
#include "version.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <json-c/json.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

enum {
    MAX_REQUEST_BYTES = 16 * 1024,
    MAX_RESPONSE_BYTES = 16 * 1024 * 1024,
    REQUEST_READ_TIMEOUT_MS = 5000,
};

typedef enum {
    ROUTE_HEALTH,
    ROUTE_USAGE,
    ROUTE_COST,
} RouteKind;

typedef struct {
    RouteKind kind;
    char *provider;
    char *cache_key;
} Route;

typedef struct {
    int status;
    char *body;
} HttpResponse;

typedef struct {
    gint64 expires_at_us;
    char *body;
} CacheEntry;

typedef enum {
    REQUEST_OK,
    REQUEST_INVALID,
    REQUEST_FORBIDDEN,
    REQUEST_METHOD,
    REQUEST_NOT_FOUND,
} RequestResult;

static volatile sig_atomic_t stop_requested;

static void handle_stop(int signal_number) {
    (void)signal_number;
    stop_requested = 1;
}

static void route_clear(Route *route) {
    g_free(route->provider);
    g_free(route->cache_key);
    *route = (Route){0};
}

static void response_clear(HttpResponse *response) {
    g_free(response->body);
    *response = (HttpResponse){0};
}

static void cache_entry_free(gpointer data) {
    CacheEntry *entry = data;
    if (!entry) return;
    g_free(entry->body);
    g_free(entry);
}

static HttpResponse json_error(int status, const char *message) {
    json_object *object = json_object_new_object();
    json_object_object_add(object, "error", json_object_new_string(message));
    HttpResponse response = {
        .status = status,
        .body = g_strdup(json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN)),
    };
    json_object_put(object);
    return response;
}

static const char *status_reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    case 504: return "Gateway Timeout";
    default: return "Error";
    }
}

static gboolean send_all(int descriptor, const void *data, gsize length) {
    const char *bytes = data;
    while (length > 0) {
        ssize_t count = send(descriptor, bytes, length, MSG_NOSIGNAL);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return FALSE;
        bytes += count;
        length -= (gsize)count;
    }
    return TRUE;
}

static void send_response(int client, const HttpResponse *response) {
    gsize length = strlen(response->body);
    char *headers = g_strdup_printf("HTTP/1.1 %d %s\r\n"
                                    "Content-Type: application/json; charset=utf-8\r\n"
                                    "Content-Length: %zu\r\n"
                                    "Connection: close\r\n"
                                    "X-Content-Type-Options: nosniff\r\n"
                                    "Cache-Control: no-store\r\n"
                                    "\r\n",
                                    response->status,
                                    status_reason(response->status),
                                    length);
    if (send_all(client, headers, strlen(headers))) send_all(client, response->body, length);
    g_free(headers);
}

static gboolean valid_port_text(const char *text) {
    if (!text || text[0] == '\0') return FALSE;
    char *end = NULL;
    gint64 port = g_ascii_strtoll(text, &end, 10);
    return *end == '\0' && port >= 1 && port <= 65535;
}

static gboolean allowed_host(const char *raw) {
    char *host = g_strstrip(g_strdup(raw));
    if (host[0] == '\0' || strchr(host, ',')) {
        g_free(host);
        return FALSE;
    }
    char *name = host;
    char *port = NULL;
    if (host[0] == '[') {
        char *closing = strchr(host, ']');
        if (!closing) {
            g_free(host);
            return FALSE;
        }
        if (closing[1] == ':') {
            port = closing + 2;
            if (!valid_port_text(port)) {
                g_free(host);
                return FALSE;
            }
        } else if (closing[1] != '\0') {
            g_free(host);
            return FALSE;
        }
        closing[1] = '\0';
    } else {
        char *colon = strchr(host, ':');
        if (colon) {
            if (strchr(colon + 1, ':')) {
                g_free(host);
                return FALSE;
            }
            *colon = '\0';
            port = colon + 1;
            if (!valid_port_text(port)) {
                g_free(host);
                return FALSE;
            }
        }
    }
    gboolean allowed = g_ascii_strcasecmp(name, "127.0.0.1") == 0 ||
        g_ascii_strcasecmp(name, "localhost") == 0 || g_ascii_strcasecmp(name, "localhost.") == 0 ||
        g_ascii_strcasecmp(name, "[::1]") == 0;
    g_free(host);
    return allowed;
}

static char *query_provider(const char *query) {
    if (!query) return NULL;
    char **items = g_strsplit(query, "&", -1);
    char *provider = NULL;
    for (guint index = 0; items[index]; index++) {
        char *equals = strchr(items[index], '=');
        if (!equals) continue;
        *equals = '\0';
        char *name = g_uri_unescape_string(items[index], NULL);
        char *value = g_uri_unescape_string(equals + 1, NULL);
        if (name && value && g_str_equal(name, "provider")) {
            g_free(provider);
            provider = g_strstrip(g_strdup(value));
            if (provider[0] == '\0') g_clear_pointer(&provider, g_free);
        }
        g_free(value);
        g_free(name);
    }
    g_strfreev(items);
    return provider;
}

static gboolean provider_allowed(RouteKind kind, const char *raw) {
    if (!raw) return TRUE;
    if (g_ascii_strcasecmp(raw, "all") == 0 || g_ascii_strcasecmp(raw, "both") == 0) return TRUE;
    char *lower = g_ascii_strdown(raw, -1);
    const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(lower);
    g_free(lower);
    if (!descriptor) return FALSE;
    return kind != ROUTE_COST || g_str_equal(descriptor->id, "codex") || g_str_equal(descriptor->id, "claude");
}

static RequestResult parse_request(char *request, Route *route) {
    char *line_end = strstr(request, "\r\n");
    if (!line_end) return REQUEST_INVALID;
    *line_end = '\0';
    char **parts = g_strsplit(request, " ", 3);
    if (g_strv_length(parts) != 3 || parts[1][0] != '/' || !g_str_has_prefix(parts[2], "HTTP/1.")) {
        g_strfreev(parts);
        return REQUEST_INVALID;
    }
    gboolean get = g_ascii_strcasecmp(parts[0], "GET") == 0;
    char *target = g_strdup(parts[1]);
    g_strfreev(parts);

    guint host_count = 0;
    gboolean host_ok = FALSE;
    char *cursor = line_end + 2;
    while (*cursor != '\0') {
        char *end = strstr(cursor, "\r\n");
        if (!end) {
            g_free(target);
            return REQUEST_INVALID;
        }
        if (end == cursor) break;
        *end = '\0';
        char *colon = strchr(cursor, ':');
        if (!colon || colon == cursor) {
            g_free(target);
            return REQUEST_INVALID;
        }
        *colon = '\0';
        if (g_ascii_strcasecmp(cursor, "Host") == 0) {
            host_count++;
            host_ok = allowed_host(colon + 1);
        }
        cursor = end + 2;
    }
    if (host_count != 1) {
        g_free(target);
        return REQUEST_INVALID;
    }
    if (!host_ok) {
        g_free(target);
        return REQUEST_FORBIDDEN;
    }
    if (!get) {
        g_free(target);
        return REQUEST_METHOD;
    }

    char *query = strchr(target, '?');
    if (query) *query++ = '\0';
    if (g_str_equal(target, "/health")) {
        route->kind = ROUTE_HEALTH;
    } else if (g_str_equal(target, "/usage")) {
        route->kind = ROUTE_USAGE;
    } else if (g_str_equal(target, "/cost")) {
        route->kind = ROUTE_COST;
    } else {
        g_free(target);
        return REQUEST_NOT_FOUND;
    }
    route->provider = query_provider(query);
    if (!provider_allowed(route->kind, route->provider)) {
        g_free(target);
        return REQUEST_INVALID;
    }
    route->cache_key = g_strdup_printf("%s:%s",
                                       route->kind == ROUTE_USAGE ? "usage" :
                                       route->kind == ROUTE_COST ? "cost" : "health",
                                       route->provider ? route->provider : "");
    g_free(target);
    return REQUEST_OK;
}

static RequestResult read_request(int client, Route *route) {
    char buffer[MAX_REQUEST_BYTES + 1];
    gsize used = 0;
    gint64 deadline = g_get_monotonic_time() + REQUEST_READ_TIMEOUT_MS * 1000;
    while (used < MAX_REQUEST_BYTES) {
        gint64 remaining_us = deadline - g_get_monotonic_time();
        if (remaining_us <= 0) return REQUEST_INVALID;
        struct pollfd descriptor = {.fd = client, .events = POLLIN};
        int ready;
        do {
            ready = poll(&descriptor, 1, (int)MAX(1, (remaining_us + 999) / 1000));
        } while (ready < 0 && errno == EINTR);
        if (ready <= 0 || !(descriptor.revents & POLLIN)) return REQUEST_INVALID;
        ssize_t count = recv(client, buffer + used, MAX_REQUEST_BYTES - used, 0);
        if (count <= 0) return REQUEST_INVALID;
        used += (gsize)count;
        buffer[used] = '\0';
        if (strstr(buffer, "\r\n\r\n")) return parse_request(buffer, route);
    }
    return REQUEST_INVALID;
}

static gint64 monotonic_milliseconds(void) {
    struct timespec value;
    if (clock_gettime(CLOCK_MONOTONIC, &value) != 0) return 0;
    return (gint64)value.tv_sec * 1000 + value.tv_nsec / 1000000;
}

static gboolean set_close_on_exec(int descriptor) {
    int flags = fcntl(descriptor, F_GETFD);
    return flags >= 0 && fcntl(descriptor, F_SETFD, flags | FD_CLOEXEC) == 0;
}

static gboolean capture_worker_output(int descriptor,
                                      pid_t worker,
                                      double timeout,
                                      char **body,
                                      gboolean *timed_out) {
    GByteArray *bytes = g_byte_array_new();
    gint64 deadline = timeout > 0 ? monotonic_milliseconds() + (gint64)(timeout * 1000.0) : 0;
    gboolean complete = FALSE;
    *timed_out = FALSE;
    while (!complete && bytes->len <= MAX_RESPONSE_BYTES) {
        int wait_ms = -1;
        if (deadline > 0) {
            gint64 remaining = deadline - monotonic_milliseconds();
            if (remaining <= 0) {
                *timed_out = TRUE;
                break;
            }
            wait_ms = (int)MIN(remaining, G_MAXINT);
        }
        struct pollfd poll_descriptor = {.fd = descriptor, .events = POLLIN | POLLHUP};
        int result;
        do {
            result = poll(&poll_descriptor, 1, wait_ms);
        } while (result < 0 && errno == EINTR);
        if (result == 0) {
            *timed_out = TRUE;
            break;
        }
        if (result < 0) break;
        if (poll_descriptor.revents & (POLLIN | POLLHUP)) {
            guint8 buffer[8192];
            ssize_t count;
            do {
                count = read(descriptor, buffer, sizeof(buffer));
            } while (count < 0 && errno == EINTR);
            if (count > 0) {
                g_byte_array_append(bytes, buffer, (guint)count);
            } else {
                complete = TRUE;
            }
        }
    }
    if (*timed_out || bytes->len > MAX_RESPONSE_BYTES) {
        kill(-worker, SIGTERM);
        kill(worker, SIGTERM);
        kill(-worker, SIGKILL);
        kill(worker, SIGKILL);
    }
    int status = 0;
    while (waitpid(worker, &status, 0) < 0 && errno == EINTR) {}
    if (*timed_out || bytes->len > MAX_RESPONSE_BYTES) {
        g_byte_array_unref(bytes);
        return FALSE;
    }
    g_byte_array_append(bytes, (const guint8 *)"", 1);
    *body = (char *)g_byte_array_free(bytes, FALSE);
    return WIFEXITED(status);
}

static HttpResponse execute_route(const Route *route, double timeout) {
    int descriptors[2] = {-1, -1};
    if (pipe(descriptors) != 0 || !set_close_on_exec(descriptors[0]) || !set_close_on_exec(descriptors[1])) {
        if (descriptors[0] >= 0) close(descriptors[0]);
        if (descriptors[1] >= 0) close(descriptors[1]);
        return json_error(500, "could not start request worker");
    }
    pid_t worker = fork();
    if (worker < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return json_error(500, "could not start request worker");
    }
    if (worker == 0) {
        close(descriptors[0]);
        setpgid(0, 0);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(1);
        close(descriptors[1]);
        char *arguments[4];
        int count = 0;
        arguments[count++] = "--format";
        arguments[count++] = "json";
        if (route->provider) {
            arguments[count++] = "--provider";
            arguments[count++] = route->provider;
        }
        int status = route->kind == ROUTE_USAGE ? codexbar_cli_usage_run(count, arguments)
                                                : codexbar_cli_cost_run(count, arguments);
        fflush(stdout);
        _exit(status);
    }
    setpgid(worker, worker);
    close(descriptors[1]);
    char *body = NULL;
    gboolean timed_out = FALSE;
    gboolean completed = capture_worker_output(descriptors[0], worker, timeout, &body, &timed_out);
    close(descriptors[0]);
    if (timed_out) return json_error(504, "request timed out");
    if (!completed || !body || body[0] == '\0') {
        g_free(body);
        return json_error(500, "request failed");
    }
    json_tokener *tokener = json_tokener_new();
    json_object *parsed = json_tokener_parse_ex(tokener, body, (int)strlen(body));
    gboolean valid = json_tokener_get_error(tokener) == json_tokener_success && parsed != NULL;
    if (parsed) json_object_put(parsed);
    json_tokener_free(tokener);
    if (!valid) {
        g_free(body);
        return json_error(500, "request returned invalid JSON");
    }
    return (HttpResponse){.status = 200, .body = body};
}

static HttpResponse health_response(void) {
    json_object *object = json_object_new_object();
    json_object_object_add(object, "status", json_object_new_string("ok"));
    json_object_object_add(object, "version", json_object_new_string(CODEXBAR_LINUX_VERSION));
    HttpResponse response = {
        .status = 200,
        .body = g_strdup(json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN)),
    };
    json_object_put(object);
    return response;
}

static HttpResponse route_response(const Route *route,
                                   GHashTable *cache,
                                   double refresh_interval,
                                   double request_timeout) {
    if (route->kind == ROUTE_HEALTH) return health_response();
    gint64 now = g_get_monotonic_time();
    CacheEntry *entry = g_hash_table_lookup(cache, route->cache_key);
    if (entry && entry->expires_at_us > now) {
        return (HttpResponse){.status = 200, .body = g_strdup(entry->body)};
    }
    if (entry) g_hash_table_remove(cache, route->cache_key);
    HttpResponse response = execute_route(route, request_timeout);
    if (response.status == 200 && refresh_interval > 0) {
        CacheEntry *stored = g_new0(CacheEntry, 1);
        stored->expires_at_us = now + (gint64)(refresh_interval * G_USEC_PER_SEC);
        stored->body = g_strdup(response.body);
        g_hash_table_replace(cache, g_strdup(route->cache_key), stored);
    }
    return response;
}

static HttpResponse request_error(RequestResult result) {
    switch (result) {
    case REQUEST_FORBIDDEN: return json_error(403, "forbidden host");
    case REQUEST_METHOD: return json_error(405, "method not allowed");
    case REQUEST_NOT_FOUND: return json_error(404, "not found");
    case REQUEST_INVALID: return json_error(400, "invalid request");
    case REQUEST_OK: break;
    }
    return json_error(500, "request failed");
}

int codexbar_serve_run(unsigned int port, double refresh_interval, double request_timeout) {
    signal(SIGPIPE, SIG_IGN);
    signal(SIGINT, handle_stop);
    signal(SIGTERM, handle_stop);
    stop_requested = 0;
    int server = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (server < 0) {
        fprintf(stderr, "Could not create server socket: %s\n", g_strerror(errno));
        return 1;
    }
    int reuse = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    struct sockaddr_in address = {
        .sin_family = AF_INET,
        .sin_port = htons((guint16)port),
    };
    inet_pton(AF_INET, "127.0.0.1", &address.sin_addr);
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(server, 16) != 0) {
        fprintf(stderr, "Could not listen on 127.0.0.1:%u: %s\n", port, g_strerror(errno));
        close(server);
        return 1;
    }
    fprintf(stderr, "CodexBar server listening on http://127.0.0.1:%u\n", port);
    fflush(stderr);
    GHashTable *cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, cache_entry_free);
    while (!stop_requested) {
        struct pollfd descriptor = {.fd = server, .events = POLLIN};
        int ready = poll(&descriptor, 1, 250);
        if (ready < 0 && errno == EINTR) continue;
        if (ready <= 0 || !(descriptor.revents & POLLIN)) continue;
        int client = accept(server, NULL, NULL);
        if (client < 0) {
            if (errno == EINTR || errno == ECONNABORTED) continue;
            fprintf(stderr, "Could not accept request: %s\n", g_strerror(errno));
            break;
        }
        Route route = {0};
        RequestResult result = read_request(client, &route);
        HttpResponse response = result == REQUEST_OK
            ? route_response(&route, cache, refresh_interval, request_timeout)
            : request_error(result);
        send_response(client, &response);
        response_clear(&response);
        route_clear(&route);
        close(client);
    }
    g_hash_table_unref(cache);
    close(server);
    return 0;
}
