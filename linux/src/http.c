#include "http.h"

#include <curl/curl.h>
#include <gio/gio.h>
#include <stdint.h>
#include <string.h>

#define DEFAULT_MAXIMUM_RESPONSE_BYTES (8U * 1024U * 1024U)
#define MAXIMUM_HEADER_BYTES (64U * 1024U)
#define MAXIMUM_REDIRECTS 5

typedef struct {
    GByteArray *data;
    size_t maximum;
    gboolean exceeded;
} BoundedBuffer;

typedef struct {
    GPtrArray *headers;
    size_t bytes;
    gboolean exceeded;
} HeaderBuffer;

static GQuark http_error_quark(void) {
    return g_quark_from_static_string("codexbar-http-error");
}

static void response_header_free(gpointer data) {
    CodexBarHttpResponseHeader *header = data;
    if (!header) return;
    g_free(header->name);
    g_free(header->value);
    g_free(header);
}

static CURLcode initialize_curl(void) {
    static gsize initialized = 0;
    static CURLcode result = CURLE_OK;
    if (g_once_init_enter(&initialized)) {
        result = curl_global_init(CURL_GLOBAL_DEFAULT);
        g_once_init_leave(&initialized, 1);
    }
    return result;
}

static gboolean is_loopback_host(const char *host) {
    if (!host) return FALSE;
    if (g_ascii_strcasecmp(host, "localhost") == 0) return TRUE;
    GInetAddress *address = g_inet_address_new_from_string(host);
    if (!address) return FALSE;
    gboolean loopback = g_inet_address_get_is_loopback(address);
    g_object_unref(address);
    return loopback;
}

static gboolean url_is_loopback(const char *url) {
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    if (!uri) return FALSE;
    gboolean loopback = is_loopback_host(g_uri_get_host(uri));
    g_uri_unref(uri);
    return loopback;
}

static gboolean authority_has_encoded_delimiter(const char *url) {
    const char *authority = strstr(url, "://");
    if (!authority) return FALSE;
    authority += 3;
    size_t length = strcspn(authority, "/?#");
    char *value = g_ascii_strdown(authority, (gssize)length);
    const char *blocked[] = {"%2f", "%5c", "%3f", "%23", "%40", "%3a"};
    gboolean found = FALSE;
    for (size_t index = 0; index < G_N_ELEMENTS(blocked); index++) {
        found = found || strstr(value, blocked[index]) != NULL;
    }
    g_free(value);
    return found;
}

char *codexbar_http_normalize_endpoint(
    const char *raw, CodexBarHttpProtocolPolicy protocol_policy, GError **error) {
    if (!raw) {
        g_set_error_literal(error, http_error_quark(), 1, "HTTP endpoint is missing");
        return NULL;
    }
    char *clean = g_strdup(raw);
    g_strstrip(clean);
    if (clean[0] == '\0') {
        g_free(clean);
        g_set_error_literal(error, http_error_quark(), 1, "HTTP endpoint is empty");
        return NULL;
    }
    char *candidate = strstr(clean, "://") ? g_strdup(clean) : g_strdup_printf("https://%s", clean);
    g_free(clean);
    if (authority_has_encoded_delimiter(candidate)) {
        g_free(candidate);
        g_set_error_literal(error, http_error_quark(), 1, "HTTP endpoint contains an encoded authority delimiter");
        return NULL;
    }

    GError *parse_error = NULL;
    GUri *uri = g_uri_parse(candidate, G_URI_FLAGS_NONE, &parse_error);
    const char *scheme = uri ? g_uri_get_scheme(uri) : NULL;
    const char *host = uri ? g_uri_get_host(uri) : NULL;
    const char *userinfo = uri ? g_uri_get_userinfo(uri) : NULL;
    gboolean https = scheme && g_ascii_strcasecmp(scheme, "https") == 0;
    gboolean allowed_http = scheme && g_ascii_strcasecmp(scheme, "http") == 0 &&
                            protocol_policy == CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP && is_loopback_host(host);
    if (!uri || !host || host[0] == '\0' || userinfo || (!https && !allowed_http)) {
        g_clear_error(&parse_error);
        if (uri) g_uri_unref(uri);
        g_free(candidate);
        g_set_error_literal(error,
                            http_error_quark(),
                            1,
                            "HTTP endpoint must use HTTPS without credentials, except explicit loopback HTTP");
        return NULL;
    }
    g_clear_error(&parse_error);
    g_uri_unref(uri);
    return candidate;
}

static size_t append_body(char *data, size_t size, size_t count, void *user_data) {
    BoundedBuffer *body = user_data;
    if (count != 0 && size > G_MAXSIZE / count) {
        body->exceeded = TRUE;
        return 0;
    }
    size_t bytes = size * count;
    if (bytes > body->maximum - MIN(body->maximum, body->data->len)) {
        body->exceeded = TRUE;
        return 0;
    }
    g_byte_array_append(body->data, (const guint8 *)data, bytes);
    return bytes;
}

static size_t append_header(char *data, size_t size, size_t count, void *user_data) {
    HeaderBuffer *buffer = user_data;
    if (count != 0 && size > G_MAXSIZE / count) {
        buffer->exceeded = TRUE;
        return 0;
    }
    size_t bytes = size * count;
    if (bytes >= 5 && g_ascii_strncasecmp(data, "HTTP/", 5) == 0) {
        g_ptr_array_set_size(buffer->headers, 0);
        buffer->bytes = bytes;
        return bytes;
    }
    if (bytes > MAXIMUM_HEADER_BYTES - MIN(MAXIMUM_HEADER_BYTES, buffer->bytes)) {
        buffer->exceeded = TRUE;
        return 0;
    }
    buffer->bytes += bytes;

    const char *colon = memchr(data, ':', bytes);
    if (!colon) return bytes;
    size_t name_length = (size_t)(colon - data);
    size_t value_length = bytes - name_length - 1;
    char *name = g_strndup(data, name_length);
    char *value = g_strndup(colon + 1, value_length);
    g_strstrip(name);
    g_strstrip(value);
    if (name[0] != '\0') {
        CodexBarHttpResponseHeader *header = g_new0(CodexBarHttpResponseHeader, 1);
        header->name = name;
        header->value = value;
        g_ptr_array_add(buffer->headers, header);
    } else {
        g_free(name);
        g_free(value);
    }
    return bytes;
}

static int transfer_progress(void *user_data,
                             curl_off_t download_total,
                             curl_off_t download_now,
                             curl_off_t upload_total,
                             curl_off_t upload_now) {
    (void)download_total;
    (void)download_now;
    (void)upload_total;
    (void)upload_now;
    GCancellable *cancellable = user_data;
    return cancellable && g_cancellable_is_cancelled(cancellable) ? 1 : 0;
}

static gboolean token_character(unsigned char character) {
    return g_ascii_isalnum(character) || strchr("!#$%&'*+-.^_`|~", character) != NULL;
}

static gboolean valid_request_header(const CodexBarHttpRequestHeader *header) {
    if (!header->name || !header->value || header->name[0] == '\0') return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)header->name; *cursor; cursor++) {
        if (!token_character(*cursor)) return FALSE;
    }
    for (const unsigned char *cursor = (const unsigned char *)header->value; *cursor; cursor++) {
        if ((*cursor < 32 && *cursor != '\t') || *cursor == 127) return FALSE;
    }
    return TRUE;
}

static gboolean supported_method(const char *method) {
    const char *methods[] = {"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"};
    for (size_t index = 0; index < G_N_ELEMENTS(methods); index++) {
        if (g_str_equal(method, methods[index])) return TRUE;
    }
    return FALSE;
}

static gboolean validate_request(const CodexBarHttpRequest *request, GError **error) {
    const char *method = request->method && request->method[0] != '\0' ? request->method : "GET";
    if (!request->url || !supported_method(method) || (request->header_count > 0 && !request->headers) ||
        (request->body_length > 0 && !request->body) ||
        ((g_str_equal(method, "GET") || g_str_equal(method, "HEAD")) &&
         (request->body || request->body_length > 0)) ||
        request->protocol_policy > CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP ||
        request->redirect_policy > CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN ||
        request->body_length > (size_t)INT64_MAX) {
        g_set_error_literal(error, http_error_quark(), 3, "HTTP request is invalid");
        return FALSE;
    }
    for (size_t index = 0; index < request->header_count; index++) {
        if (!valid_request_header(&request->headers[index])) {
            g_set_error_literal(error, http_error_quark(), 3, "HTTP request contains an invalid header");
            return FALSE;
        }
    }
    return TRUE;
}

static CodexBarHttpResponse *send_once(const CodexBarHttpRequest *request, const char *url, GError **error) {
    if (request->cancellable && g_cancellable_set_error_if_cancelled(request->cancellable, error)) return NULL;
    CURL *curl = curl_easy_init();
    if (!curl) {
        g_set_error_literal(error, http_error_quark(), 2, "Could not initialize libcurl");
        return NULL;
    }

    BoundedBuffer body = {
        .data = g_byte_array_new(),
        .maximum = request->maximum_response_bytes ? request->maximum_response_bytes : DEFAULT_MAXIMUM_RESPONSE_BYTES,
    };
    HeaderBuffer response_headers = {
        .headers = g_ptr_array_new_with_free_func(response_header_free),
    };
    struct curl_slist *headers = NULL;
    for (size_t index = 0; index < request->header_count; index++) {
        char *line = g_strdup_printf("%s: %s", request->headers[index].name, request->headers[index].value);
        headers = curl_slist_append(headers, line);
        g_free(line);
    }

    const char *method = request->method && request->method[0] != '\0' ? request->method : "GET";
    curl_easy_setopt(curl, CURLOPT_URL, url);
    if (g_str_equal(method, "GET")) {
        curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    } else if (g_str_equal(method, "POST")) {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
    } else if (g_str_equal(method, "HEAD")) {
        curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
    } else {
        curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method);
    }
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, append_header);
    curl_easy_setopt(curl, CURLOPT_HEADERDATA, &response_headers);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, request->timeout_seconds > 0 ? request->timeout_seconds : 15L);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L);
    curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L);
    if (request->cancellable) {
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, transfer_progress);
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, request->cancellable);
    }
    curl_easy_setopt(curl,
                     CURLOPT_PROTOCOLS_STR,
                     request->protocol_policy == CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP ? "http,https" : "https");
    if (url_is_loopback(url)) curl_easy_setopt(curl, CURLOPT_NOPROXY, "*");
    if (request->body) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request->body);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)request->body_length);
    } else if (g_str_equal(method, "POST")) {
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, "");
        curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE_LARGE, (curl_off_t)0);
    }

    CURLcode result = curl_easy_perform(curl);
    if (result != CURLE_OK) {
        if (!request->cancellable || !g_cancellable_set_error_if_cancelled(request->cancellable, error)) {
            const char *message = body.exceeded ? "HTTP response exceeded the configured size limit"
                                   : response_headers.exceeded ? "HTTP response headers exceeded the size limit"
                                                               : curl_easy_strerror(result);
            g_set_error(error, http_error_quark(), (int)result, "HTTP request failed: %s", message);
        }
        curl_slist_free_all(headers);
        curl_easy_cleanup(curl);
        g_byte_array_unref(body.data);
        g_ptr_array_unref(response_headers.headers);
        return NULL;
    }

    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response->status);
    char *effective_url = NULL;
    curl_easy_getinfo(curl, CURLINFO_EFFECTIVE_URL, &effective_url);
    response->effective_url = g_strdup(effective_url ? effective_url : url);
    response->body_length = body.data->len;
    g_byte_array_append(body.data, (const guint8 *)"", 1);
    response->body = (char *)g_byte_array_free(body.data, FALSE);
    response->headers = response_headers.headers;
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
}

const char *codexbar_http_response_header_first(const CodexBarHttpResponse *response, const char *name) {
    if (!response || !name) return NULL;
    for (guint index = 0; index < response->headers->len; index++) {
        const CodexBarHttpResponseHeader *header = g_ptr_array_index(response->headers, index);
        if (g_ascii_strcasecmp(header->name, name) == 0) return header->value;
    }
    return NULL;
}

static gboolean same_origin(const char *left, const char *right) {
    GUri *left_uri = g_uri_parse(left, G_URI_FLAGS_NONE, NULL);
    GUri *right_uri = g_uri_parse(right, G_URI_FLAGS_NONE, NULL);
    if (!left_uri || !right_uri) {
        if (left_uri) g_uri_unref(left_uri);
        if (right_uri) g_uri_unref(right_uri);
        return FALSE;
    }
    const char *left_scheme = g_uri_get_scheme(left_uri);
    const char *right_scheme = g_uri_get_scheme(right_uri);
    const char *left_host = g_uri_get_host(left_uri);
    const char *right_host = g_uri_get_host(right_uri);
    int left_port = g_uri_get_port(left_uri);
    int right_port = g_uri_get_port(right_uri);
    if (left_port < 0) left_port = g_ascii_strcasecmp(left_scheme, "https") == 0 ? 443 : 80;
    if (right_port < 0) right_port = g_ascii_strcasecmp(right_scheme, "https") == 0 ? 443 : 80;
    gboolean equal = left_scheme && right_scheme && left_host && right_host &&
                     g_ascii_strcasecmp(left_scheme, right_scheme) == 0 &&
                     g_ascii_strcasecmp(left_host, right_host) == 0 && left_port == right_port;
    g_uri_unref(left_uri);
    g_uri_unref(right_uri);
    return equal;
}

CodexBarHttpResponse *codexbar_http_send(const CodexBarHttpRequest *request, GError **error) {
    g_return_val_if_fail(request != NULL, NULL);
    if (!validate_request(request, error)) return NULL;
    CURLcode initialization = initialize_curl();
    if (initialization != CURLE_OK) {
        g_set_error(error,
                    http_error_quark(),
                    (int)initialization,
                    "Could not initialize libcurl: %s",
                    curl_easy_strerror(initialization));
        return NULL;
    }
    char *current_url = codexbar_http_normalize_endpoint(request->url, request->protocol_policy, error);
    if (!current_url) return NULL;

    for (int redirects = 0; redirects <= MAXIMUM_REDIRECTS; redirects++) {
        CodexBarHttpResponse *response = send_once(request, current_url, error);
        if (!response) {
            g_free(current_url);
            return NULL;
        }
        gboolean redirect = response->status == 301 || response->status == 302 || response->status == 307 ||
                            response->status == 308;
        const char *location = redirect ? codexbar_http_response_header_first(response, "Location") : NULL;
        if (!location || request->redirect_policy == CODEXBAR_HTTP_REDIRECT_DENY) {
            g_free(current_url);
            return response;
        }
        if (g_ascii_strcasecmp(request->method ? request->method : "GET", "GET") != 0) {
            codexbar_http_response_free(response);
            g_free(current_url);
            g_set_error_literal(error, http_error_quark(), 4, "HTTP redirects are only supported for GET requests");
            return NULL;
        }
        char *resolved_url = g_uri_resolve_relative(current_url, location, G_URI_FLAGS_NONE, error);
        char *next_url = resolved_url
                             ? codexbar_http_normalize_endpoint(resolved_url, request->protocol_policy, error)
                             : NULL;
        g_free(resolved_url);
        if (!next_url || !same_origin(current_url, next_url)) {
            g_free(next_url);
            codexbar_http_response_free(response);
            g_free(current_url);
            if (!error || !*error) {
                g_set_error_literal(error, http_error_quark(), 4, "HTTP redirect changed origin");
            }
            return NULL;
        }
        codexbar_http_response_free(response);
        g_free(current_url);
        current_url = next_url;
    }

    g_free(current_url);
    g_set_error_literal(error, http_error_quark(), 4, "HTTP request exceeded the redirect limit");
    return NULL;
}

CodexBarHttpResponse *codexbar_http_get(
    const char *url, const char *auth_header, const char *auth_value, GError **error) {
    const CodexBarHttpRequestHeader headers[] = {
        {auth_header, auth_value},
        {"Accept", "application/json"},
        {"X-Title", "CodexBar"},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 15,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
    };
    return codexbar_http_send(&request, error);
}

void codexbar_http_response_free(CodexBarHttpResponse *response) {
    if (!response) return;
    g_free(response->body);
    g_clear_pointer(&response->headers, g_ptr_array_unref);
    g_free(response->effective_url);
    g_free(response);
}
