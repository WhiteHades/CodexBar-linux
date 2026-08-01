#define _POSIX_C_SOURCE 200809L

#include "serve.h"

#include "config.h"
#include "provider_registry.h"
#include "version.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <glib.h>
#include <json-c/json.h>
#include <math.h>
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
    MAX_ACTIVE_CLIENTS = 16,
};

typedef enum {
    ROUTE_HEALTH,
    ROUTE_USAGE,
    ROUTE_COST,
    ROUTE_DASHBOARD,
} RouteKind;

typedef struct {
    RouteKind kind;
    char *provider;
    char *cache_key;
    char *authorization;
} Route;

typedef struct {
    int status;
    char *body;
} HttpResponse;

typedef struct {
    gint64 expires_at_us;
    char *body;
} CacheEntry;

typedef struct {
    gint64 expires_at_us;
    char *body;
} LastGoodEntry;

typedef struct {
    char *fingerprint;
    guint64 generation;
    gboolean running;
    guint waiters;
    guint remaining_waiters;
    int status;
    char *body;
} Operation;

typedef struct {
    gboolean configured;
    guint8 digest[32];
} DashboardAuth;

typedef struct {
    GMutex mutex;
    GCond condition;
    GHashTable *cache;
    GHashTable *operations;
    GHashTable *last_good_usage;
    GHashTable *last_good_cost;
    GHashTable *last_good_dashboard;
    guint64 next_generation;
    guint active_clients;
} ServeState;

typedef struct {
    int client;
    const char *host;
    double refresh_interval;
    double request_timeout;
    const DashboardAuth *dashboard_auth;
    gboolean data_routes_require_auth;
    ServeState *state;
} ClientContext;

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
    g_free(route->authorization);
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

static void last_good_entry_free(gpointer data) {
    LastGoodEntry *entry = data;
    if (!entry) return;
    g_free(entry->body);
    g_free(entry);
}

static void operation_free(gpointer data) {
    Operation *operation = data;
    if (!operation) return;
    g_free(operation->fingerprint);
    g_free(operation->body);
    g_free(operation);
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

static void dashboard_auth_init(DashboardAuth *auth, const char *token) {
    *auth = (DashboardAuth){0};
    if (!token) return;
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, (const guchar *)token, strlen(token));
    gsize digest_length = sizeof(auth->digest);
    g_checksum_get_digest(checksum, auth->digest, &digest_length);
    g_checksum_free(checksum);
    auth->configured = digest_length == sizeof(auth->digest);
}

static char *bearer_token(const char *authorization) {
    if (!authorization) return NULL;
    char *trimmed = g_strstrip(g_strdup(authorization));
    const char scheme[] = "Bearer ";
    if (strlen(trimmed) <= sizeof(scheme) - 1 ||
        g_ascii_strncasecmp(trimmed, scheme, sizeof(scheme) - 1) != 0) {
        g_free(trimmed);
        return NULL;
    }
    char *token = g_strstrip(g_strdup(trimmed + sizeof(scheme) - 1));
    g_free(trimmed);
    if (token[0] == '\0') g_clear_pointer(&token, g_free);
    return token;
}

static gboolean dashboard_auth_authorizes(const DashboardAuth *auth, const char *authorization) {
    if (!auth->configured) return FALSE;
    char *token = bearer_token(authorization);
    if (!token) return FALSE;
    guint8 digest[32];
    GChecksum *checksum = g_checksum_new(G_CHECKSUM_SHA256);
    g_checksum_update(checksum, (const guchar *)token, strlen(token));
    gsize digest_length = sizeof(digest);
    g_checksum_get_digest(checksum, digest, &digest_length);
    g_checksum_free(checksum);
    g_free(token);
    guint8 difference = 0;
    for (guint index = 0; index < sizeof(digest); index++) difference |= digest[index] ^ auth->digest[index];
    return digest_length == sizeof(digest) && difference == 0;
}

static gboolean loopback_bind_host(const char *host) {
    return g_str_has_prefix(host, "127.");
}

static const char *status_reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
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
                                    "%s"
                                    "\r\n",
                                    response->status,
                                    status_reason(response->status),
                                    length,
                                    response->status == 401 ? "WWW-Authenticate: Bearer\r\n" : "");
    if (send_all(client, headers, strlen(headers))) send_all(client, response->body, length);
    g_free(headers);
}

static gboolean valid_port_text(const char *text) {
    if (!text || text[0] == '\0') return FALSE;
    char *end = NULL;
    gint64 port = g_ascii_strtoll(text, &end, 10);
    return *end == '\0' && port >= 1 && port <= 65535;
}

static gboolean allowed_host(const char *raw, const char *bind_host) {
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
    gboolean loopback = g_ascii_strcasecmp(name, "127.0.0.1") == 0 ||
        g_ascii_strcasecmp(name, "localhost") == 0 || g_ascii_strcasecmp(name, "localhost.") == 0 ||
        g_ascii_strcasecmp(name, "[::1]") == 0;
    gboolean wildcard = g_str_equal(bind_host, "0.0.0.0");
    gboolean allowed = loopback || wildcard || g_ascii_strcasecmp(name, bind_host) == 0;
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

static RequestResult parse_request(char *request, Route *route, const char *bind_host) {
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
    guint authorization_count = 0;
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
        char *name = g_strstrip(cursor);
        char *value = g_strstrip(colon + 1);
        if (g_ascii_strcasecmp(name, "Host") == 0) {
            host_count++;
            host_ok = allowed_host(value, bind_host);
        } else if (g_ascii_strcasecmp(name, "Authorization") == 0) {
            authorization_count++;
            g_free(route->authorization);
            route->authorization = g_strdup(value);
        }
        cursor = end + 2;
    }
    if (host_count != 1 || authorization_count > 1) {
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
    } else if (g_str_equal(target, "/dashboard/v1/snapshot")) {
        route->kind = ROUTE_DASHBOARD;
    } else {
        g_free(target);
        return REQUEST_NOT_FOUND;
    }
    route->provider = route->kind == ROUTE_USAGE || route->kind == ROUTE_COST ? query_provider(query) : NULL;
    route->cache_key = g_strdup_printf("%s:%s",
                                       route->kind == ROUTE_USAGE ? "usage" :
                                       route->kind == ROUTE_COST ? "cost" :
                                       route->kind == ROUTE_DASHBOARD ? "dashboard" : "health",
                                       route->provider ? route->provider : "");
    g_free(target);
    return REQUEST_OK;
}

static RequestResult read_request(int client, Route *route, const char *bind_host) {
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
        if (strstr(buffer, "\r\n\r\n")) return parse_request(buffer, route, bind_host);
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
    char *executable = g_file_read_link("/proc/self/exe", NULL);
    if (!executable) return json_error(500, "could not locate request worker");
    int descriptors[2] = {-1, -1};
    if (pipe(descriptors) != 0 || !set_close_on_exec(descriptors[0]) || !set_close_on_exec(descriptors[1])) {
        if (descriptors[0] >= 0) close(descriptors[0]);
        if (descriptors[1] >= 0) close(descriptors[1]);
        g_free(executable);
        return json_error(500, "could not start request worker");
    }
    char *arguments[7];
    int count = 0;
    arguments[count++] = executable;
    arguments[count++] = route->kind == ROUTE_USAGE ? "usage" : "cost";
    arguments[count++] = "--format";
    arguments[count++] = "json";
    if (route->provider) {
        arguments[count++] = "--provider";
        arguments[count++] = route->provider;
    }
    arguments[count] = NULL;
    pid_t worker = fork();
    if (worker < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        g_free(executable);
        return json_error(500, "could not start request worker");
    }
    if (worker == 0) {
        close(descriptors[0]);
        setpgid(0, 0);
        signal(SIGTERM, SIG_DFL);
        signal(SIGINT, SIG_DFL);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(1);
        close(descriptors[1]);
        execv(executable, arguments);
        _exit(127);
    }
    g_free(executable);
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

static json_object *json_member(json_object *object, const char *key, json_type type) {
    json_object *value = NULL;
    return object && json_object_is_type(object, json_type_object) &&
                   json_object_object_get_ex(object, key, &value) && json_object_is_type(value, type)
               ? value
               : NULL;
}

static const char *json_string_member(json_object *object, const char *key) {
    json_object *value = json_member(object, key, json_type_string);
    return value ? json_object_get_string(value) : NULL;
}

static gboolean json_number_member(json_object *object, const char *key, double *number) {
    json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value) ||
        (!json_object_is_type(value, json_type_double) && !json_object_is_type(value, json_type_int))) {
        return FALSE;
    }
    *number = json_object_get_double(value);
    return isfinite(*number);
}

static double clamped_percent(double value) {
    return MIN(100, MAX(0, value));
}

static json_object *dashboard_number(double value) {
    char text[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(text, sizeof(text), "%.12g", value);
    return json_object_new_double_s(value, text);
}

static json_object *dashboard_error(json_object *source) {
    if (!source || !json_object_is_type(source, json_type_object)) return NULL;
    json_object *error = json_object_new_object();
    const char *message = json_string_member(source, "message");
    const char *kind = json_string_member(source, "kind");
    json_object *code = NULL;
    if (message) json_object_object_add(error, "message", json_object_new_string(message));
    if (json_object_object_get_ex(source, "code", &code) && json_object_is_type(code, json_type_int)) {
        json_object_object_add(error, "code", json_object_new_int64(json_object_get_int64(code)));
    }
    if (kind) json_object_object_add(error, "kind", json_object_new_string(kind));
    return error;
}

static json_object *dashboard_status(json_object *source) {
    if (!source || !json_object_is_type(source, json_type_object)) return NULL;
    const char *indicator = json_string_member(source, "indicator");
    const char *level = "unknown";
    const char *label = "Status unknown";
    if (g_strcmp0(indicator, "none") == 0) {
        level = "ok";
        label = "Operational";
    } else if (g_strcmp0(indicator, "minor") == 0) {
        level = "warning";
        label = "Partial outage";
    } else if (g_strcmp0(indicator, "maintenance") == 0) {
        level = "warning";
        label = "Maintenance";
    } else if (g_strcmp0(indicator, "major") == 0) {
        level = "critical";
        label = "Major outage";
    } else if (g_strcmp0(indicator, "critical") == 0) {
        level = "critical";
        label = "Critical issue";
    }
    json_object *status = json_object_new_object();
    json_object_object_add(status, "level", json_object_new_string(level));
    json_object_object_add(status, "label", json_object_new_string(label));
    const char *updated_at = json_string_member(source, "updatedAt");
    json_object_object_add(status, "updatedAt", updated_at ? json_object_new_string(updated_at) : NULL);
    return status;
}

static char *redacted_email(const char *raw) {
    if (!raw) return NULL;
    char *email = g_strstrip(g_strdup(raw));
    if (email[0] == '\0') {
        g_free(email);
        return NULL;
    }
    char *domain = strrchr(email, '@');
    char *redacted = domain ? g_strdup_printf("redacted%s", domain) : g_strdup("redacted");
    g_free(email);
    return redacted;
}

static json_object *dashboard_identity(json_object *provider, json_object *usage) {
    json_object *identity_source = json_member(usage, "identity", json_type_object);
    const char *email = json_string_member(identity_source, "accountEmail");
    if (!email) email = json_string_member(usage, "accountEmail");
    if (!email) email = json_string_member(provider, "account");
    const char *plan = json_string_member(identity_source, "loginMethod");
    if (!plan) plan = json_string_member(usage, "loginMethod");
    if (!plan) plan = json_string_member(provider, "plan");
    char *redacted = redacted_email(email);
    char *clean_plan = plan ? g_strstrip(g_strdup(plan)) : NULL;
    if (clean_plan && clean_plan[0] == '\0') g_clear_pointer(&clean_plan, g_free);
    if (!redacted && !clean_plan) return NULL;
    json_object *identity = json_object_new_object();
    json_object_object_add(identity, "accountEmail", redacted ? json_object_new_string(redacted) : NULL);
    json_object_object_add(identity, "plan", clean_plan ? json_object_new_string(clean_plan) : NULL);
    g_free(clean_plan);
    g_free(redacted);
    return identity;
}

static json_object *dashboard_window(const char *kind, const char *label, json_object *source) {
    double used = 0;
    if (!source || !json_object_is_type(source, json_type_object) ||
        !json_number_member(source, "usedPercent", &used)) {
        return NULL;
    }
    used = clamped_percent(used);
    json_object *window = json_object_new_object();
    json_object_object_add(window, "kind", json_object_new_string(kind));
    json_object_object_add(window, "label", json_object_new_string(label));
    json_object_object_add(window, "usedPercent", dashboard_number(used));
    json_object_object_add(window, "remainingPercent", dashboard_number(clamped_percent(100 - used)));
    const char *reset_at = json_string_member(source, "resetsAt");
    json_object_object_add(window, "resetAt", reset_at ? json_object_new_string(reset_at) : NULL);
    return window;
}

static json_object *dashboard_windows(const char *provider_id, json_object *usage) {
    json_object *windows = json_object_new_array();
    gboolean amp_subscription = g_strcmp0(provider_id, "amp") == 0 &&
        json_member(json_member(usage, "ampUsage", json_type_object), "subscription", json_type_object);
    const char *keys[] = {"primary", "secondary", "tertiary"};
    const char *kinds[] = {amp_subscription ? "other" : "session", amp_subscription ? "orb" : "weekly", "tertiary"};
    const char *labels[] = {amp_subscription ? "Other usage" : "Session",
                            amp_subscription ? "Orb usage" : "Weekly",
                            "Tertiary"};
    for (guint index = 0; index < G_N_ELEMENTS(keys); index++) {
        json_object *window = dashboard_window(
            kinds[index], labels[index], json_member(usage, keys[index], json_type_object));
        if (window) json_object_array_add(windows, window);
    }
    json_object *extra = json_member(usage, "extraRateWindows", json_type_array);
    if (extra) {
        for (size_t index = 0; index < json_object_array_length(extra); index++) {
            json_object *item = json_object_array_get_idx(extra, index);
            const char *id = json_string_member(item, "id");
            const char *title = json_string_member(item, "title");
            json_object *window = dashboard_window(id ? id : "other",
                                                   title ? title : "Other",
                                                   json_member(item, "window", json_type_object));
            if (window) json_object_array_add(windows, window);
        }
    }
    return windows;
}

static json_object *dashboard_credits(json_object *source) {
    double remaining = 0;
    if (!source || !json_number_member(source, "remaining", &remaining)) return NULL;
    json_object *credits = json_object_new_object();
    json_object_object_add(credits, "remaining", dashboard_number(remaining));
    json_object_object_add(credits, "unit", json_object_new_string("credits"));
    return credits;
}

static json_object *cost_for_provider(json_object *costs, const char *provider_id) {
    if (!costs || !json_object_is_type(costs, json_type_array)) return NULL;
    for (size_t index = 0; index < json_object_array_length(costs); index++) {
        json_object *cost = json_object_array_get_idx(costs, index);
        if (g_strcmp0(json_string_member(cost, "provider"), provider_id) == 0) return cost;
    }
    return NULL;
}

static json_object *dashboard_cost(json_object *source) {
    double today = 0;
    double last_30_days = 0;
    gboolean has_today = source && json_number_member(source, "sessionCostUSD", &today);
    gboolean has_last_30_days = source && json_number_member(source, "last30DaysCostUSD", &last_30_days);
    if (!has_today && !has_last_30_days) return NULL;
    json_object *cost = json_object_new_object();
    json_object_object_add(cost, "todayUSD", has_today ? dashboard_number(today) : NULL);
    json_object_object_add(cost,
                           "last30DaysUSD",
                           has_last_30_days ? dashboard_number(last_30_days) : NULL);
    return cost;
}

static const char *newest_timestamp(const char *newest, const char *candidate) {
    return candidate && (!newest || strcmp(candidate, newest) > 0) ? candidate : newest;
}

static const char *dashboard_updated_at(json_object *provider,
                                        json_object *usage,
                                        json_object *cost,
                                        gboolean has_error,
                                        const char *generated_at) {
    const char *newest = NULL;
    json_object *status = json_member(provider, "status", json_type_object);
    json_object *credits = json_member(provider, "credits", json_type_object);
    newest = newest_timestamp(newest, json_string_member(status, "updatedAt"));
    newest = newest_timestamp(newest, json_string_member(usage, "updatedAt"));
    newest = newest_timestamp(newest, json_string_member(credits, "updatedAt"));
    newest = newest_timestamp(newest, json_string_member(cost, "updatedAt"));
    return newest ? newest : has_error ? generated_at : NULL;
}

static json_object *dashboard_provider(json_object *source,
                                       json_object *costs,
                                       guint index,
                                       CodexBarConfig *config,
                                       const char *generated_at) {
    const char *provider_id = json_string_member(source, "provider");
    if (!provider_id) provider_id = "unknown";
    const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(provider_id);
    json_object *usage = json_member(source, "usage", json_type_object);
    json_object *cost_source = cost_for_provider(costs, provider_id);
    json_object *error_source = json_member(source, "error", json_type_object);
    if (!error_source) error_source = json_member(cost_source, "error", json_type_object);
    const char *source_name = json_string_member(source, "source");
    if (!source_name || source_name[0] == '\0') source_name = "unknown";
    json_object *provider = json_object_new_object();
    json_object_object_add(provider, "id", json_object_new_string(provider_id));
    json_object_object_add(provider,
                           "name",
                           json_object_new_string(descriptor ? descriptor->display_name : provider_id));
    CodexBarProviderConfig *provider_config = codexbar_config_provider(config, provider_id);
    json_object_object_add(provider,
                           "enabled",
                           json_object_new_boolean(provider_config ? provider_config->enabled : TRUE));
    json_object_object_add(provider, "source", json_object_new_string(source_name));
    json_object_object_add(provider,
                           "status",
                           dashboard_status(json_member(source, "status", json_type_object)));
    json_object_object_add(provider, "identity", dashboard_identity(source, usage));
    json_object_object_add(provider, "windows", dashboard_windows(provider_id, usage));
    json_object_object_add(provider, "credits", dashboard_credits(json_member(source, "credits", json_type_object)));
    json_object_object_add(provider, "cost", dashboard_cost(cost_source));
    json_object *display = json_object_new_object();
    json_object_object_add(display, "accentColor", json_object_new_string("#6E6E6E"));
    gint64 sort_key = 10000 + index;
    for (guint config_index = 0; config_index < config->providers->len; config_index++) {
        CodexBarProviderConfig *configured = g_ptr_array_index(config->providers, config_index);
        if (g_strcmp0(configured->id, provider_id) == 0) {
            sort_key = (gint64)config_index * 10;
            break;
        }
    }
    json_object_object_add(display, "sortKey", json_object_new_int64(sort_key));
    json_object_object_add(display, "priority", json_object_new_string("normal"));
    json_object_object_add(provider, "display", display);
    json_object_object_add(provider, "error", dashboard_error(error_source));
    const char *updated_at = dashboard_updated_at(source, usage, cost_source, error_source != NULL, generated_at);
    json_object_object_add(provider, "updatedAt", updated_at ? json_object_new_string(updated_at) : NULL);
    return provider;
}

static char *iso8601_now(void) {
    GDateTime *now = g_date_time_new_now_utc();
    char *timestamp = g_date_time_format(now, "%Y-%m-%dT%H:%M:%SZ");
    g_date_time_unref(now);
    return timestamp;
}

static json_object *dashboard_snapshot(json_object *usage,
                                       json_object *costs,
                                       CodexBarConfig *config,
                                       double refresh_interval) {
    char *generated_at = iso8601_now();
    gint64 refresh_seconds = refresh_interval > 0 ? (gint64)ceil(refresh_interval) : 0;
    refresh_seconds = MIN(refresh_seconds, G_MAXINT64 / 3);
    json_object *snapshot = json_object_new_object();
    json_object_object_add(snapshot, "schemaVersion", json_object_new_int(1));
    json_object_object_add(snapshot, "generatedAt", json_object_new_string(generated_at));
    json_object_object_add(snapshot, "staleAfterSeconds", json_object_new_int64(MAX(180, refresh_seconds * 3)));
    json_object *host = json_object_new_object();
    json_object_object_add(host, "codexBarVersion", json_object_new_string(CODEXBAR_LINUX_VERSION));
    json_object_object_add(host, "refreshIntervalSeconds", json_object_new_int64(refresh_seconds));
    json_object_object_add(snapshot, "host", host);
    json_object *providers = json_object_new_array();
    if (usage && json_object_is_type(usage, json_type_array)) {
        for (size_t index = 0; index < json_object_array_length(usage); index++) {
            json_object *source = json_object_array_get_idx(usage, index);
            if (json_object_is_type(source, json_type_object)) {
                json_object_array_add(
                    providers, dashboard_provider(source, costs, (guint)index, config, generated_at));
            }
        }
    }
    json_object_object_add(snapshot, "providers", providers);
    g_free(generated_at);
    return snapshot;
}

static json_object *parse_json_body(const char *body) {
    json_tokener *tokener = json_tokener_new();
    json_object *object = json_tokener_parse_ex(tokener, body, (int)strlen(body));
    gboolean valid = json_tokener_get_error(tokener) == json_tokener_success && object != NULL;
    json_tokener_free(tokener);
    if (!valid && object) json_object_put(object);
    return valid ? object : NULL;
}

static HttpResponse execute_dashboard(CodexBarConfig *config,
                                      double refresh_interval,
                                      double request_timeout) {
    gint64 started_at = g_get_monotonic_time();
    Route usage_route = {.kind = ROUTE_USAGE};
    HttpResponse usage_response = execute_route(&usage_route, request_timeout);
    if (usage_response.status != 200) return usage_response;
    double cost_timeout = request_timeout;
    if (request_timeout > 0) {
        double elapsed = (g_get_monotonic_time() - started_at) / (double)G_USEC_PER_SEC;
        cost_timeout = request_timeout - elapsed;
        if (cost_timeout <= 0) {
            response_clear(&usage_response);
            return json_error(504, "request timed out");
        }
    }
    Route cost_route = {.kind = ROUTE_COST};
    HttpResponse cost_response = execute_route(&cost_route, cost_timeout);
    if (cost_response.status != 200) {
        response_clear(&usage_response);
        return cost_response;
    }
    json_object *usage = parse_json_body(usage_response.body);
    json_object *costs = parse_json_body(cost_response.body);
    response_clear(&usage_response);
    response_clear(&cost_response);
    if (!usage || !json_object_is_type(usage, json_type_array) || !costs ||
        !json_object_is_type(costs, json_type_array)) {
        if (usage) json_object_put(usage);
        if (costs) json_object_put(costs);
        return json_error(500, "request returned invalid JSON");
    }
    json_object *snapshot = dashboard_snapshot(usage, costs, config, refresh_interval);
    HttpResponse response = {
        .status = 200,
        .body = g_strdup(json_object_to_json_string_ext(snapshot, JSON_C_TO_STRING_PLAIN)),
    };
    json_object_put(snapshot);
    json_object_put(costs);
    json_object_put(usage);
    return response;
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

static char *config_fingerprint(const CodexBarConfig *config) {
    if (config->loaded_digest) return g_strdup(config->loaded_digest);
    char *rendered = codexbar_config_render_json(config, FALSE);
    char *fingerprint = g_compute_checksum_for_string(G_CHECKSUM_SHA256, rendered, -1);
    g_free(rendered);
    return fingerprint;
}

static CodexBarConfig *load_config(char **fingerprint) {
    GError *error = NULL;
    CodexBarConfig *config = codexbar_config_load(&error);
    if (!config) {
        g_clear_error(&error);
        return NULL;
    }
    *fingerprint = config_fingerprint(config);
    return config;
}

static gboolean object_has_error(json_object *object) {
    json_object *error = NULL;
    return object && json_object_is_type(object, json_type_object) &&
        json_object_object_get_ex(object, "error", &error) && error && !json_object_is_type(error, json_type_null);
}

static gboolean response_has_provider_errors(RouteKind kind, const char *body) {
    json_object *root = parse_json_body(body);
    if (!root) return TRUE;
    json_object *items = root;
    if (kind == ROUTE_DASHBOARD) items = json_member(root, "providers", json_type_array);
    gboolean has_errors = !items || !json_object_is_type(items, json_type_array);
    if (!has_errors) {
        for (size_t index = 0; index < json_object_array_length(items); index++) {
            if (object_has_error(json_object_array_get_idx(items, index))) {
                has_errors = TRUE;
                break;
            }
        }
    }
    json_object_put(root);
    return has_errors;
}

static gint64 stale_ttl_us(double refresh_interval) {
    if (refresh_interval <= 0) return 0;
    double seconds = MIN(3600.0, MAX(300.0, refresh_interval * 10.0));
    return (gint64)(seconds * G_USEC_PER_SEC);
}

static const char *usage_account_key(json_object *row) {
    const char *account = json_string_member(row, "account");
    json_object *usage = json_member(row, "usage", json_type_object);
    json_object *identity = json_member(usage, "identity", json_type_object);
    if (!account) account = json_string_member(identity, "accountID");
    if (!account) account = json_string_member(identity, "accountEmail");
    if (!account) account = json_string_member(usage, "accountEmail");
    return account;
}

static char *last_good_key(const char *cache_key, const char *provider, const char *account) {
    return g_strdup_printf("%s\x1f%s\x1f%s", cache_key, provider, account ? account : "");
}

static void store_last_good(GHashTable *table,
                            const char *key,
                            json_object *row,
                            gint64 expires_at_us) {
    LastGoodEntry *entry = g_new0(LastGoodEntry, 1);
    entry->expires_at_us = expires_at_us;
    entry->body = g_strdup(json_object_to_json_string_ext(row, JSON_C_TO_STRING_PLAIN));
    g_hash_table_replace(table, g_strdup(key), entry);
}

static void apply_row_last_good_locked(ServeState *state,
                                       RouteKind kind,
                                       const char *cache_key,
                                       HttpResponse *response,
                                       gint64 expires_at_us) {
    if (response->status != 200 || (kind != ROUTE_USAGE && kind != ROUTE_COST)) return;
    json_object *rows = parse_json_body(response->body);
    if (!rows || !json_object_is_type(rows, json_type_array)) {
        if (rows) json_object_put(rows);
        return;
    }
    GHashTable *table = kind == ROUTE_USAGE ? state->last_good_usage : state->last_good_cost;
    gboolean changed = FALSE;
    gint64 now = g_get_monotonic_time();
    for (size_t index = 0; index < json_object_array_length(rows); index++) {
        json_object *row = json_object_array_get_idx(rows, index);
        const char *provider = json_string_member(row, "provider");
        const char *account = kind == ROUTE_USAGE ? usage_account_key(row) : NULL;
        if (!provider || (kind == ROUTE_USAGE && !account)) continue;
        char *key = last_good_key(cache_key, provider, account);
        if (object_has_error(row)) {
            LastGoodEntry *entry = g_hash_table_lookup(table, key);
            if (entry && entry->expires_at_us > now) {
                json_object *replacement = parse_json_body(entry->body);
                if (replacement) {
                    json_object_array_put_idx(rows, index, replacement);
                    changed = TRUE;
                }
            } else if (entry) {
                g_hash_table_remove(table, key);
            }
        } else if (expires_at_us > now) {
            store_last_good(table, key, row, expires_at_us);
        }
        g_free(key);
    }
    if (changed) {
        g_free(response->body);
        response->body = g_strdup(json_object_to_json_string_ext(rows, JSON_C_TO_STRING_PLAIN));
    }
    json_object_put(rows);
}

static gboolean apply_dashboard_last_good_locked(ServeState *state,
                                                 const char *cache_key,
                                                 HttpResponse *response,
                                                 gboolean has_errors,
                                                 gint64 expires_at_us) {
    gint64 now = g_get_monotonic_time();
    LastGoodEntry *entry = g_hash_table_lookup(state->last_good_dashboard, cache_key);
    if ((response->status != 200 || has_errors) && entry && entry->expires_at_us > now) {
        response_clear(response);
        *response = (HttpResponse){.status = 200, .body = g_strdup(entry->body)};
        return TRUE;
    }
    if (entry && entry->expires_at_us <= now) g_hash_table_remove(state->last_good_dashboard, cache_key);
    if (response->status == 200 && !has_errors && expires_at_us > now) {
        LastGoodEntry *stored = g_new0(LastGoodEntry, 1);
        stored->expires_at_us = expires_at_us;
        stored->body = g_strdup(response->body);
        g_hash_table_replace(state->last_good_dashboard, g_strdup(cache_key), stored);
    }
    return FALSE;
}

static gboolean wait_for_operation(ServeState *state, gint64 deadline_us) {
    if (deadline_us <= 0) {
        g_cond_wait(&state->condition, &state->mutex);
        return TRUE;
    }
    return g_cond_wait_until(&state->condition, &state->mutex, deadline_us);
}

static double remaining_timeout(gint64 deadline_us) {
    if (deadline_us <= 0) return 0;
    return MAX(0, deadline_us - g_get_monotonic_time()) / (double)G_USEC_PER_SEC;
}

static HttpResponse route_response(const Route *route,
                                   ServeState *state,
                                   double refresh_interval,
                                   double request_timeout) {
    if (route->kind == ROUTE_HEALTH) return health_response();
    gint64 deadline_us = request_timeout > 0
        ? g_get_monotonic_time() + (gint64)(request_timeout * G_USEC_PER_SEC)
        : 0;
    char *fingerprint = NULL;
    CodexBarConfig *config = load_config(&fingerprint);
    if (!config) return json_error(500, "could not load config");
    char *cache_key = g_strdup_printf("%s:%s", route->cache_key, fingerprint);
    guint64 generation = 0;
    gboolean waited_for_operation = FALSE;

    g_mutex_lock(&state->mutex);
    CacheEntry *entry = g_hash_table_lookup(state->cache, cache_key);
    gint64 now = g_get_monotonic_time();
    if (entry && entry->expires_at_us > now) {
        HttpResponse cached = {.status = 200, .body = g_strdup(entry->body)};
        g_mutex_unlock(&state->mutex);
        g_free(cache_key);
        g_free(fingerprint);
        codexbar_config_free(config);
        return cached;
    }
    if (entry) g_hash_table_remove(state->cache, cache_key);

    for (;;) {
        Operation *operation = g_hash_table_lookup(state->operations, route->cache_key);
        if (!operation) {
            operation = g_new0(Operation, 1);
            operation->fingerprint = g_strdup(fingerprint);
            operation->generation = ++state->next_generation;
            operation->running = TRUE;
            generation = operation->generation;
            g_hash_table_insert(state->operations, g_strdup(route->cache_key), operation);
            break;
        }
        if (operation->running && g_strcmp0(operation->fingerprint, fingerprint) == 0) {
            generation = operation->generation;
            operation->waiters++;
            for (;;) {
                if (!wait_for_operation(state, deadline_us)) {
                    Operation *current = g_hash_table_lookup(state->operations, route->cache_key);
                    if (current && current->generation == generation && !current->running) {
                        HttpResponse response = {.status = current->status, .body = g_strdup(current->body)};
                        if (--current->remaining_waiters == 0) {
                            g_hash_table_remove(state->operations, route->cache_key);
                            g_cond_broadcast(&state->condition);
                        }
                        g_mutex_unlock(&state->mutex);
                        g_free(cache_key);
                        g_free(fingerprint);
                        codexbar_config_free(config);
                        return response;
                    }
                    if (current && current->generation == generation) current->waiters--;
                    g_mutex_unlock(&state->mutex);
                    g_free(cache_key);
                    g_free(fingerprint);
                    codexbar_config_free(config);
                    return json_error(504, "request timed out");
                }
                Operation *current = g_hash_table_lookup(state->operations, route->cache_key);
                if (current && current->generation == generation && !current->running) {
                    HttpResponse response = {.status = current->status, .body = g_strdup(current->body)};
                    if (--current->remaining_waiters == 0) {
                        g_hash_table_remove(state->operations, route->cache_key);
                        g_cond_broadcast(&state->condition);
                    }
                    g_mutex_unlock(&state->mutex);
                    g_free(cache_key);
                    g_free(fingerprint);
                    codexbar_config_free(config);
                    return response;
                }
            }
        }
        waited_for_operation = TRUE;
        if (!wait_for_operation(state, deadline_us)) {
            g_mutex_unlock(&state->mutex);
            g_free(cache_key);
            g_free(fingerprint);
            codexbar_config_free(config);
            return json_error(504, "request timed out");
        }
    }
    g_mutex_unlock(&state->mutex);

    double timeout = waited_for_operation ? remaining_timeout(deadline_us) : request_timeout;
    HttpResponse response = timeout == 0 && deadline_us > 0
        ? json_error(504, "request timed out")
        : route->kind == ROUTE_DASHBOARD ? execute_dashboard(config, refresh_interval, timeout)
                                         : execute_route(route, timeout);
    char *current_fingerprint = NULL;
    CodexBarConfig *current_config = load_config(&current_fingerprint);
    gboolean unchanged = current_config && g_strcmp0(fingerprint, current_fingerprint) == 0;
    if (current_config) codexbar_config_free(current_config);
    g_free(current_fingerprint);

    g_mutex_lock(&state->mutex);
    gboolean has_errors = response.status == 200 && response_has_provider_errors(route->kind, response.body);
    gboolean cacheable = response.status == 200 && !has_errors;
    if (unchanged) {
        gint64 stale_expiry = g_get_monotonic_time() + stale_ttl_us(refresh_interval);
        if (route->kind == ROUTE_DASHBOARD) {
            if (apply_dashboard_last_good_locked(state, cache_key, &response, has_errors, stale_expiry)) {
                cacheable = FALSE;
            }
        } else {
            apply_row_last_good_locked(state, route->kind, cache_key, &response, stale_expiry);
        }
        if (cacheable && refresh_interval > 0) {
            CacheEntry *stored = g_new0(CacheEntry, 1);
            stored->expires_at_us = g_get_monotonic_time() + (gint64)(refresh_interval * G_USEC_PER_SEC);
            stored->body = g_strdup(response.body);
            g_hash_table_replace(state->cache, g_strdup(cache_key), stored);
        }
    }
    Operation *operation = g_hash_table_lookup(state->operations, route->cache_key);
    if (operation && operation->generation == generation) {
        operation->running = FALSE;
        operation->status = response.status;
        operation->body = g_strdup(response.body);
        operation->remaining_waiters = operation->waiters;
        g_cond_broadcast(&state->condition);
        if (operation->remaining_waiters == 0) g_hash_table_remove(state->operations, route->cache_key);
    }
    g_mutex_unlock(&state->mutex);
    g_free(cache_key);
    g_free(fingerprint);
    codexbar_config_free(config);
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

static gpointer serve_client(gpointer data) {
    ClientContext *context = data;
    Route route = {0};
    RequestResult result = read_request(context->client, &route, context->host);
    gboolean route_requires_auth = result == REQUEST_OK &&
        (route.kind == ROUTE_DASHBOARD ||
         (context->data_routes_require_auth && (route.kind == ROUTE_USAGE || route.kind == ROUTE_COST)));
    HttpResponse response;
    if (route_requires_auth && !dashboard_auth_authorizes(context->dashboard_auth, route.authorization)) {
        response = json_error(401, "unauthorized");
    } else if (result == REQUEST_OK && !provider_allowed(route.kind, route.provider)) {
        response = request_error(REQUEST_INVALID);
    } else {
        response = result == REQUEST_OK
            ? route_response(&route, context->state, context->refresh_interval, context->request_timeout)
            : request_error(result);
    }
    send_response(context->client, &response);
    response_clear(&response);
    route_clear(&route);
    close(context->client);
    g_mutex_lock(&context->state->mutex);
    context->state->active_clients--;
    g_cond_broadcast(&context->state->condition);
    g_mutex_unlock(&context->state->mutex);
    g_free(context);
    return NULL;
}

static void serve_state_init(ServeState *state) {
    *state = (ServeState){0};
    g_mutex_init(&state->mutex);
    g_cond_init(&state->condition);
    state->cache = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, cache_entry_free);
    state->operations = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, operation_free);
    state->last_good_usage = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, last_good_entry_free);
    state->last_good_cost = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, last_good_entry_free);
    state->last_good_dashboard = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, last_good_entry_free);
}

static void serve_state_clear(ServeState *state) {
    g_hash_table_unref(state->last_good_dashboard);
    g_hash_table_unref(state->last_good_cost);
    g_hash_table_unref(state->last_good_usage);
    g_hash_table_unref(state->operations);
    g_hash_table_unref(state->cache);
    g_cond_clear(&state->condition);
    g_mutex_clear(&state->mutex);
}

int codexbar_serve_run(const char *host,
                       unsigned int port,
                       double refresh_interval,
                       double request_timeout,
                       const char *dashboard_token) {
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
    if (!host || inet_pton(AF_INET, host, &address.sin_addr) != 1) {
        fputs("Could not resolve server bind host.\n", stderr);
        close(server);
        return 1;
    }
    if (bind(server, (struct sockaddr *)&address, sizeof(address)) != 0 || listen(server, 16) != 0) {
        fprintf(stderr, "Could not listen on %s:%u: %s\n", host, port, g_strerror(errno));
        close(server);
        return 1;
    }
    fprintf(stderr, "CodexBar server listening on http://%s:%u\n", host, port);
    if (!loopback_bind_host(host)) {
        fputs("Warning: plain HTTP on a non-loopback host; the bearer token gating /usage, /cost, and "
              "/dashboard/v1/snapshot crosses the network in cleartext on every request.\n",
              stderr);
    }
    fflush(stderr);
    DashboardAuth dashboard_auth;
    dashboard_auth_init(&dashboard_auth, dashboard_token);
    gboolean data_routes_require_auth = !loopback_bind_host(host);
    ServeState state;
    serve_state_init(&state);
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
        g_mutex_lock(&state.mutex);
        if (state.active_clients >= MAX_ACTIVE_CLIENTS) {
            g_mutex_unlock(&state.mutex);
            HttpResponse response = json_error(503, "server busy");
            send_response(client, &response);
            response_clear(&response);
            close(client);
            continue;
        }
        state.active_clients++;
        g_mutex_unlock(&state.mutex);
        ClientContext *context = g_new0(ClientContext, 1);
        *context = (ClientContext){
            .client = client,
            .host = host,
            .refresh_interval = refresh_interval,
            .request_timeout = request_timeout,
            .dashboard_auth = &dashboard_auth,
            .data_routes_require_auth = data_routes_require_auth,
            .state = &state,
        };
        GThread *thread = g_thread_new("serve-client", serve_client, context);
        g_thread_unref(thread);
    }
    close(server);
    g_mutex_lock(&state.mutex);
    while (state.active_clients > 0) g_cond_wait(&state.condition, &state.mutex);
    g_mutex_unlock(&state.mutex);
    serve_state_clear(&state);
    return 0;
}
