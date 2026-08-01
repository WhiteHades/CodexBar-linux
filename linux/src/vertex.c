#include "vertex.h"

#include "process.h"

#include <json-c/json.h>
#include <string.h>

#define RESPONSE_LIMIT (1024U * 1024U)
#define CREDENTIAL_LIMIT 65536U
#define GCLOUD_TIMEOUT_MILLISECONDS 20000

typedef struct {
    gboolean service_account;
    char *access_token;
    char *refresh_token;
    char *client_id;
    char *client_secret;
    char *project_id;
    char *email;
    gboolean has_expiry;
    gint64 expiry_ms;
} VertexCredentials;

static json_object *parse_json(const char *text, size_t length) {
    if (!text || !length || length > RESPONSE_LIMIT || length > G_MAXINT ||
        memchr(text, '\0', length) || !g_utf8_validate(text, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, text, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && strchr(" \t\r\n", text[consumed])) consumed++;
    gboolean valid = parse_error == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static json_object *member(json_object *object, const char *key) {
    json_object *value = NULL;
    return object && json_object_is_type(object, json_type_object) &&
                   json_object_object_get_ex(object, key, &value)
               ? value
               : NULL;
}

static char *string_member(json_object *object, const char *key) {
    json_object *value = member(object, key);
    if (!value || !json_object_is_type(value, json_type_string)) return NULL;
    const char *raw = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!raw || length > CREDENTIAL_LIMIT || memchr(raw, '\0', length) ||
        !g_utf8_validate(raw, (gssize)length, NULL)) return NULL;
    char *copy = g_strndup(raw, length);
    g_strstrip(copy);
    if (copy[0]) return copy;
    g_free(copy);
    return NULL;
}

static void credentials_free(VertexCredentials *credentials) {
    if (!credentials) return;
    g_free(credentials->access_token);
    g_free(credentials->refresh_token);
    g_free(credentials->client_id);
    g_free(credentials->client_secret);
    g_free(credentials->project_id);
    g_free(credentials->email);
    g_free(credentials);
}

static char *base64url_decode_text(const char *encoded) {
    if (!encoded || !encoded[0] || strlen(encoded) > CREDENTIAL_LIMIT) return NULL;
    char *padded = g_strdup(encoded);
    for (char *cursor = padded; *cursor; cursor++) {
        if (*cursor == '-') *cursor = '+';
        else if (*cursor == '_') *cursor = '/';
    }
    size_t length = strlen(padded);
    size_t padding = (4 - length % 4) % 4;
    padded = g_realloc(padded, length + padding + 1);
    for (size_t index = 0; index < padding; index++) padded[length + index] = '=';
    padded[length + padding] = '\0';
    gsize decoded_length = 0;
    guchar *decoded = g_base64_decode(padded, &decoded_length);
    g_free(padded);
    if (!decoded || !decoded_length || decoded_length > RESPONSE_LIMIT ||
        memchr(decoded, '\0', decoded_length) || !g_utf8_validate((char *)decoded, (gssize)decoded_length, NULL)) {
        g_free(decoded);
        return NULL;
    }
    char *text = g_strndup((char *)decoded, decoded_length);
    g_free(decoded);
    return text;
}

static char *email_from_id_token(const char *token) {
    if (!token) return NULL;
    char **parts = g_strsplit(token, ".", 3);
    char *payload = parts[0] && parts[1] ? base64url_decode_text(parts[1]) : NULL;
    g_strfreev(parts);
    if (!payload) return NULL;
    json_object *root = parse_json(payload, strlen(payload));
    g_free(payload);
    char *email = string_member(root, "email");
    if (root) json_object_put(root);
    return email;
}

static char *project_from_config(const char *text) {
    if (!text || strlen(text) > RESPONSE_LIMIT || !g_utf8_validate(text, -1, NULL)) return NULL;
    char **lines = g_strsplit(text, "\n", -1);
    char *project = NULL;
    for (size_t index = 0; !project && lines[index]; index++) {
        char *line = g_strstrip(lines[index]);
        if (!g_str_has_prefix(line, "project")) continue;
        char *equals = strchr(line, '=');
        if (!equals) continue;
        char *value = g_strstrip(equals + 1);
        if (value[0]) project = g_strdup(value);
    }
    g_strfreev(lines);
    return project;
}

static char *config_string(const CodexBarProviderConfig *config, const char *key) {
    return string_member(config ? config->raw : NULL, key);
}

static gboolean safe_header_value(const char *value) {
    if (!value || !value[0] || strlen(value) > CREDENTIAL_LIMIT || !g_utf8_validate(value, -1, NULL)) return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (g_ascii_iscntrl(*cursor)) return FALSE;
    }
    return TRUE;
}

static char *credentials_path(const CodexBarProviderConfig *config) {
    char *configured = config_string(config, "credentialsPath");
    if (configured) return configured;
    const char *adc = g_getenv("GOOGLE_APPLICATION_CREDENTIALS");
    if (adc && adc[0]) return g_strdup(adc);
    const char *sdk = g_getenv("CLOUDSDK_CONFIG");
    if (sdk && sdk[0]) return g_build_filename(sdk, "application_default_credentials.json", NULL);
    return g_build_filename(g_get_home_dir(), ".config", "gcloud", "application_default_credentials.json", NULL);
}

static char *project_config_path(const CodexBarProviderConfig *config) {
    char *configured = config_string(config, "projectConfigPath");
    if (configured) return configured;
    const char *sdk = g_getenv("CLOUDSDK_CONFIG");
    if (sdk && sdk[0]) return g_build_filename(sdk, "configurations", "config_default", NULL);
    return g_build_filename(g_get_home_dir(), ".config", "gcloud", "configurations", "config_default", NULL);
}

static char *load_project(const CodexBarProviderConfig *config) {
    char *configured = config_string(config, "projectId");
    if (configured) return configured;
    char *path = project_config_path(config);
    char *contents = NULL;
    gsize length = 0;
    if (g_file_get_contents(path, &contents, &length, NULL) && length <= RESPONSE_LIMIT) {
        configured = project_from_config(contents);
    }
    g_free(contents);
    g_free(path);
    if (configured) return configured;
    const char *environment[] = {
        g_getenv("GOOGLE_CLOUD_PROJECT"),
        g_getenv("GCLOUD_PROJECT"),
        g_getenv("CLOUDSDK_CORE_PROJECT"),
    };
    for (size_t index = 0; index < G_N_ELEMENTS(environment); index++) {
        if (environment[index] && environment[index][0]) return g_strdup(environment[index]);
    }
    return NULL;
}

static VertexCredentials *load_credentials(const CodexBarProviderConfig *config, GError **error) {
    char *path = credentials_path(config);
    char *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &contents, &length, error)) {
        if (error && *error) {
            g_prefix_error(error, "gcloud credentials not found at %s: ", path);
        }
        g_free(path);
        return NULL;
    }
    g_free(path);
    json_object *root = parse_json(contents, length);
    g_free(contents);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Failed to decode gcloud credentials");
        return NULL;
    }
    VertexCredentials *credentials = g_new0(VertexCredentials, 1);
    char *private_key = string_member(root, "private_key");
    credentials->email = string_member(root, "client_email");
    credentials->service_account = credentials->email && private_key;
    g_free(private_key);
    if (credentials->service_account) {
        credentials->project_id = string_member(root, "project_id");
        if (!credentials->project_id) credentials->project_id = load_project(config);
        json_object_put(root);
        return credentials;
    }
    g_free(credentials->email);
    credentials->email = NULL;
    credentials->client_id = string_member(root, "client_id");
    credentials->client_secret = string_member(root, "client_secret");
    credentials->refresh_token = string_member(root, "refresh_token");
    credentials->access_token = string_member(root, "access_token");
    credentials->project_id = load_project(config);
    char *id_token = string_member(root, "id_token");
    credentials->email = email_from_id_token(id_token);
    g_free(id_token);
    char *expiry = string_member(root, "token_expiry");
    if (expiry) {
        GDateTime *date = g_date_time_new_from_iso8601(expiry, NULL);
        if (date) {
            credentials->has_expiry = TRUE;
            credentials->expiry_ms = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
            g_date_time_unref(date);
        }
    }
    g_free(expiry);
    json_object_put(root);
    if (!credentials->client_id || !credentials->client_secret) {
        credentials_free(credentials);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "gcloud credentials are missing client ID or secret");
        return NULL;
    }
    if (!credentials->refresh_token) {
        credentials_free(credentials);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "gcloud credentials contain no refresh token");
        return NULL;
    }
    return credentials;
}

static gboolean same_origin(const CodexBarHttpResponse *response, const char *url) {
    if (!response || !response->effective_url) return TRUE;
    GUri *expected = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    GUri *actual = g_uri_parse(response->effective_url, G_URI_FLAGS_NONE, NULL);
    const char *expected_scheme = expected ? g_uri_get_scheme(expected) : NULL;
    const char *actual_scheme = actual ? g_uri_get_scheme(actual) : NULL;
    const char *expected_host = expected ? g_uri_get_host(expected) : NULL;
    const char *actual_host = actual ? g_uri_get_host(actual) : NULL;
    gboolean same = expected_scheme && actual_scheme && expected_host && actual_host &&
                    g_ascii_strcasecmp(actual_scheme, "https") == 0 &&
                    g_ascii_strcasecmp(expected_scheme, actual_scheme) == 0 &&
                    g_ascii_strcasecmp(expected_host, actual_host) == 0 &&
                    g_uri_get_port(expected) == g_uri_get_port(actual);
    if (expected) g_uri_unref(expected);
    if (actual) g_uri_unref(actual);
    return same;
}

static CodexBarHttpResponse *send_request(const CodexBarHttpRequest *request,
                                          CodexBarVertexTransport transport,
                                          GError **error) {
    if (!transport) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Vertex AI transport is missing");
        return NULL;
    }
    if (request->cancellable && g_cancellable_set_error_if_cancelled(request->cancellable, error)) return NULL;
    CodexBarHttpResponse *response = transport(request, error);
    if (request->cancellable && g_cancellable_is_cancelled(request->cancellable)) {
        codexbar_http_response_free(response);
        if (error && *error) g_clear_error(error);
        g_cancellable_set_error_if_cancelled(request->cancellable, error);
        return NULL;
    }
    if (!response) return NULL;
    if (!same_origin(response, request->url)) {
        codexbar_http_response_free(response);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Vertex AI redirected outside its trusted origin");
        return NULL;
    }
    return response;
}

static char *form_escape(const char *value) {
    return g_uri_escape_string(value, NULL, TRUE);
}

static gboolean refresh_user_credentials(VertexCredentials *credentials,
                                         CodexBarVertexTransport transport,
                                         GCancellable *cancellable,
                                         gint64 now_ms,
                                         GError **error) {
    char *client_id = form_escape(credentials->client_id);
    char *client_secret = form_escape(credentials->client_secret);
    char *refresh_token = form_escape(credentials->refresh_token);
    char *body = g_strdup_printf("client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
                                 client_id, client_secret, refresh_token);
    g_free(client_id);
    g_free(client_secret);
    g_free(refresh_token);
    CodexBarHttpRequestHeader headers[] = {{"Content-Type", "application/x-www-form-urlencoded"}};
    CodexBarHttpRequest request = {
        .url = "https://oauth2.googleapis.com/token",
        .method = "POST",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .body = body,
        .body_length = strlen(body),
        .timeout_seconds = 30,
        .maximum_response_bytes = RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    g_free(body);
    if (!response) return FALSE;
    json_object *root = parse_json(response->body, response->body_length);
    if (response->status == 400 || response->status == 401) {
        char *code = string_member(root, "error");
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    code && g_ascii_strcasecmp(code, "unauthorized_client") == 0
                        ? "Vertex AI refresh token was revoked"
                        : "Vertex AI refresh token expired");
        g_free(code);
        if (root) json_object_put(root);
        codexbar_http_response_free(response);
        return FALSE;
    }
    if (response->status != 200 || !root) {
        long status = response->status;
        if (root) json_object_put(root);
        codexbar_http_response_free(response);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Vertex AI token refresh returned HTTP %ld or invalid JSON", status);
        return FALSE;
    }
    char *access_token = string_member(root, "access_token");
    if (access_token) {
        g_free(credentials->access_token);
        credentials->access_token = access_token;
    }
    json_object *expires = member(root, "expires_in");
    double expires_in = expires && (json_object_is_type(expires, json_type_int) ||
                                    json_object_is_type(expires, json_type_double))
                            ? json_object_get_double(expires)
                            : 3600;
    credentials->has_expiry = TRUE;
    credentials->expiry_ms = now_ms + (gint64)(MAX(0, expires_in) * 1000);
    char *id_token = string_member(root, "id_token");
    char *email = email_from_id_token(id_token);
    g_free(id_token);
    if (email) {
        g_free(credentials->email);
        credentials->email = email;
    }
    json_object_put(root);
    codexbar_http_response_free(response);
    if (!credentials->access_token) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Vertex AI token refresh returned no access token");
        return FALSE;
    }
    return TRUE;
}

static char *gcloud_token(GCancellable *cancellable, GError **error) {
    const char *argv[] = {
        "/usr/bin/env", "gcloud", "auth", "application-default", "print-access-token", NULL,
    };
    CodexBarProcessRequest request = {
        .arguments = argv,
        .timeout_milliseconds = GCLOUD_TIMEOUT_MILLISECONDS,
        .termination_grace_milliseconds = 400,
        .maximum_output_bytes = CREDENTIAL_LIMIT,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, cancellable, error);
    if (!result) return NULL;
    if (!codexbar_process_result_succeeded(result)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "gcloud application-default token command exited with status %d: %s",
                    result->exit_status,
                    result->standard_error_length ? result->standard_error : "no diagnostic output");
        codexbar_process_result_free(result);
        return NULL;
    }
    char *token = g_strstrip(g_strdup(result->standard_output));
    codexbar_process_result_free(result);
    if (!token[0] || !g_utf8_validate(token, -1, NULL)) {
        g_free(token);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "gcloud application-default token command returned no token");
        return NULL;
    }
    return token;
}

static char *monitoring_url(const char *project,
                            const char *filter,
                            const char *page_token,
                            gint64 now_ms) {
    char *escaped_project = g_uri_escape_string(project, NULL, TRUE);
    GDateTime *end = g_date_time_new_from_unix_utc(now_ms / 1000);
    GDateTime *start = g_date_time_add_days(end, -1);
    char *end_text = g_date_time_format(end, "%Y-%m-%dT%H:%M:%SZ");
    char *start_text = g_date_time_format(start, "%Y-%m-%dT%H:%M:%SZ");
    char *escaped_filter = g_uri_escape_string(filter, NULL, TRUE);
    char *escaped_start = g_uri_escape_string(start_text, NULL, TRUE);
    char *escaped_end = g_uri_escape_string(end_text, NULL, TRUE);
    char *escaped_page = page_token ? g_uri_escape_string(page_token, NULL, TRUE) : NULL;
    char *url = g_strdup_printf(
        "https://monitoring.googleapis.com/v3/projects/%s/timeSeries?filter=%s&interval.startTime=%s&"
        "interval.endTime=%s&aggregation.alignmentPeriod=3600s&aggregation.perSeriesAligner=ALIGN_MAX&"
        "view=FULL%s%s",
        escaped_project, escaped_filter, escaped_start, escaped_end,
        escaped_page ? "&pageToken=" : "", escaped_page ? escaped_page : "");
    g_free(escaped_project);
    g_free(end_text);
    g_free(start_text);
    g_free(escaped_filter);
    g_free(escaped_start);
    g_free(escaped_end);
    g_free(escaped_page);
    g_date_time_unref(start);
    g_date_time_unref(end);
    return url;
}

static gboolean fetch_series(const VertexCredentials *credentials,
                             const char *filter,
                             CodexBarVertexTransport transport,
                             GCancellable *cancellable,
                             gint64 now_ms,
                             gboolean *has_data,
                             GError **error) {
    char *page_token = NULL;
    guint pages = 0;
    do {
        if (++pages > 100) {
            g_free(page_token);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Vertex AI Monitoring returned too many pages");
            return FALSE;
        }
        char *url = monitoring_url(credentials->project_id, filter, page_token, now_ms);
        char *authorization = g_strdup_printf("Bearer %s", credentials->access_token);
        CodexBarHttpRequestHeader headers[] = {{"Authorization", authorization}};
        CodexBarHttpRequest request = {
            .url = url,
            .method = "GET",
            .headers = headers,
            .header_count = G_N_ELEMENTS(headers),
            .timeout_seconds = 30,
            .maximum_response_bytes = RESPONSE_LIMIT,
            .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
            .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
            .cancellable = cancellable,
        };
        CodexBarHttpResponse *response = send_request(&request, transport, error);
        g_free(authorization);
        g_free(url);
        if (!response) {
            g_free(page_token);
            return FALSE;
        }
        long status = response->status;
        if (status == 401 || status == 403) {
            codexbar_http_response_free(response);
            g_free(page_token);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                status == 401
                                    ? "Vertex AI request unauthorized; refresh gcloud application-default login"
                                    : "Vertex AI Monitoring access forbidden; check IAM permissions");
            return FALSE;
        }
        if (status != 200) {
            codexbar_http_response_free(response);
            g_free(page_token);
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Vertex AI Monitoring returned HTTP %ld", status);
            return FALSE;
        }
        json_object *root = parse_json(response->body, response->body_length);
        codexbar_http_response_free(response);
        if (!root || !json_object_is_type(root, json_type_object)) {
            if (root) json_object_put(root);
            g_free(page_token);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Vertex AI Monitoring returned invalid JSON");
            return FALSE;
        }
        json_object *series = member(root, "timeSeries");
        if (series && !json_object_is_type(series, json_type_array)) {
            json_object_put(root);
            g_free(page_token);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Vertex AI Monitoring timeSeries is not an array");
            return FALSE;
        }
        if (series) {
            for (size_t index = 0; index < json_object_array_length(series); index++) {
                json_object *entry = json_object_array_get_idx(series, index);
                json_object *metric = member(entry, "metric");
                json_object *resource = member(entry, "resource");
                json_object *points = member(entry, "points");
                if (!json_object_is_type(entry, json_type_object) ||
                    !json_object_is_type(metric, json_type_object) ||
                    !json_object_is_type(resource, json_type_object) ||
                    !json_object_is_type(points, json_type_array)) {
                    json_object_put(root);
                    g_free(page_token);
                    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                        "Vertex AI Monitoring returned a malformed time series");
                    return FALSE;
                }
                for (size_t point_index = 0; point_index < json_object_array_length(points); point_index++) {
                    json_object *point = json_object_array_get_idx(points, point_index);
                    if (!json_object_is_type(member(point, "value"), json_type_object)) {
                        json_object_put(root);
                        g_free(page_token);
                        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                            "Vertex AI Monitoring returned a malformed data point");
                        return FALSE;
                    }
                }
            }
        }
        if (series && json_object_array_length(series) > 0) *has_data = TRUE;
        g_free(page_token);
        json_object *next_page = member(root, "nextPageToken");
        if (next_page && !json_object_is_type(next_page, json_type_null) &&
            !json_object_is_type(next_page, json_type_string)) {
            json_object_put(root);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Vertex AI Monitoring returned an invalid page token");
            return FALSE;
        }
        page_token = string_member(root, "nextPageToken");
        json_object_put(root);
    } while (page_token);
    return TRUE;
}

CodexBarProvider *codexbar_vertex_fetch_with_adapters(const CodexBarProviderConfig *config,
                                                      CodexBarVertexTransport transport,
                                                      CodexBarVertexTokenRunner token_runner,
                                                      GCancellable *cancellable,
                                                      gint64 now_ms,
                                                      GError **error) {
    VertexCredentials *credentials = load_credentials(config, error);
    if (!credentials) return NULL;
    if (!credentials->project_id) {
        credentials_free(credentials);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "No Google Cloud project configured; run `gcloud config set project PROJECT_ID`");
        return NULL;
    }
    if (credentials->service_account) {
        credentials->access_token = token_runner ? token_runner(cancellable, error) : NULL;
        if (!token_runner && (!error || !*error)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "Vertex AI gcloud token runner is missing");
        }
        if (!credentials->access_token) {
            credentials_free(credentials);
            return NULL;
        }
    } else if (!credentials->access_token || !credentials->has_expiry ||
               now_ms + 300000 > credentials->expiry_ms) {
        if (!refresh_user_credentials(credentials, transport, cancellable, now_ms, error)) {
            credentials_free(credentials);
            return NULL;
        }
    }
    if (!safe_header_value(credentials->access_token)) {
        credentials_free(credentials);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Vertex AI access token is invalid");
        return NULL;
    }
    const char *usage_filter =
        "metric.type=\"serviceruntime.googleapis.com/quota/allocation/usage\" AND "
        "resource.type=\"consumer_quota\" AND resource.label.service=\"aiplatform.googleapis.com\"";
    const char *limit_filter =
        "metric.type=\"serviceruntime.googleapis.com/quota/limit\" AND "
        "resource.type=\"consumer_quota\" AND resource.label.service=\"aiplatform.googleapis.com\"";
    gboolean has_usage = FALSE;
    gboolean has_limit = FALSE;
    if (!fetch_series(credentials, usage_filter, transport, cancellable, now_ms, &has_usage, error) ||
        !fetch_series(credentials, limit_filter, transport, cancellable, now_ms, &has_limit, error)) {
        credentials_free(credentials);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("vertexai");
    provider->source = g_strdup("oauth");
    provider->account = g_strdup(credentials->email);
    provider->plan = g_strdup("gcloud");
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->organization = g_strdup(credentials->project_id);
    provider->identity->login_method = g_strdup("gcloud");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    credentials_free(credentials);
    return provider;
}

CodexBarProvider *codexbar_vertex_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error) {
    return codexbar_vertex_fetch_with_adapters(
        config, codexbar_http_send, gcloud_token, cancellable, g_get_real_time() / 1000, error);
}
