#include "stepfun.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define RESPONSE_LIMIT (1024U * 1024U)
#define CREDENTIAL_LIMIT 16384U
#define DEFAULT_WEB_ID "c8a1002d2c457e758785a9979832217c7c0b884c"
#define PLATFORM_URL "https://platform.stepfun.com"
#define REGISTER_URL PLATFORM_URL "/passport/proto.api.passport.v1.PassportService/RegisterDevice"
#define LOGIN_URL PLATFORM_URL "/passport/proto.api.passport.v1.PassportService/SignInByPassword"
#define REFRESH_URL PLATFORM_URL "/passport/proto.api.passport.v1.PassportService/RefreshToken"
#define USAGE_URL PLATFORM_URL "/api/step.openapi.devcenter.Dashboard/QueryStepPlanRateLimit"
#define PLAN_URL PLATFORM_URL "/api/step.openapi.devcenter.Dashboard/GetStepPlanStatus"

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

static char *string_value(json_object *value) {
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

static char *string_member(json_object *object, const char *key) {
    return string_value(member(object, key));
}

static gboolean flexible_double(json_object *object, const char *key, double *result) {
    json_object *value = member(object, key);
    if (!value) return FALSE;
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        *result = json_object_get_double(value);
        return isfinite(*result);
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    const char *raw = json_object_get_string(value);
    char *end = NULL;
    double parsed = g_ascii_strtod(raw, &end);
    if (!raw[0] || !end || *end || !isfinite(parsed)) return FALSE;
    *result = parsed;
    return TRUE;
}

static gboolean flexible_int64(json_object *object, const char *key, gint64 *result) {
    json_object *value = member(object, key);
    if (!value) return FALSE;
    if (json_object_is_type(value, json_type_int)) {
        *result = json_object_get_int64(value);
        return TRUE;
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    const char *raw = json_object_get_string(value);
    char *end = NULL;
    gint64 parsed = g_ascii_strtoll(raw, &end, 10);
    if (!raw[0] || !end || *end) return FALSE;
    *result = parsed;
    return TRUE;
}

static char *api_message(json_object *root) {
    char *message = string_member(root, "message");
    if (!message) message = string_member(root, "desc");
    if (message) return message;
    json_object *code = member(root, "code");
    return code && json_object_is_type(code, json_type_int)
               ? g_strdup_printf("code %" G_GINT64_FORMAT, json_object_get_int64(code))
               : g_strdup("unknown response");
}

static char *reset_description(gint64 reset_ms, gint64 now_ms) {
    gint64 seconds = (reset_ms - now_ms) / 1000;
    if (seconds <= 0) return g_strdup("Reset due");
    gint64 days = seconds / 86400;
    gint64 hours = seconds % 86400 / 3600;
    gint64 minutes = seconds % 3600 / 60;
    if (days) return g_strdup_printf("Resets in %" G_GINT64_FORMAT "d %" G_GINT64_FORMAT "h", days, hours);
    if (hours) return g_strdup_printf("Resets in %" G_GINT64_FORMAT "h %" G_GINT64_FORMAT "m", hours, minutes);
    return g_strdup_printf("Resets in %" G_GINT64_FORMAT "m", minutes);
}

static gboolean has_credit_pool(json_object *credit) {
    return credit && (member(credit, "subscription_credit_left_rate") ||
                      member(credit, "topup_credit_left_rate") ||
                      (member(credit, "credit_buckets") &&
                       json_object_is_type(member(credit, "credit_buckets"), json_type_array) &&
                       json_object_array_length(member(credit, "credit_buckets")) > 0));
}

static gboolean credit_left_rate(json_object *credit, double *result) {
    json_object *buckets = member(credit, "credit_buckets");
    if (buckets && json_object_is_type(buckets, json_type_array) && json_object_array_length(buckets) > 0) {
        double total = 0;
        double residual = 0;
        guint count = json_object_array_length(buckets);
        gboolean complete = TRUE;
        for (guint index = 0; index < count; index++) {
            json_object *bucket = json_object_array_get_idx(buckets, index);
            double bucket_total = 0;
            double bucket_residual = 0;
            if (!flexible_double(bucket, "credit_total", &bucket_total) ||
                !flexible_double(bucket, "credit_residual", &bucket_residual) ||
                bucket_total <= 0 || bucket_residual < 0 || bucket_residual > bucket_total) {
                complete = FALSE;
                break;
            }
            total += bucket_total;
            residual += bucket_residual;
        }
        if (complete && total > 0) {
            *result = residual / total;
            return TRUE;
        }
    }
    if (flexible_double(credit, "subscription_credit_left_rate", result)) return TRUE;
    return flexible_double(credit, "topup_credit_left_rate", result);
}

static void add_window(CodexBarProvider *provider,
                       const char *id,
                       const char *title,
                       double left_rate,
                       gint64 minutes,
                       gint64 reset_seconds,
                       gint64 now_ms) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = CLAMP((1.0 - left_rate) * 100.0, 0.0, 100.0);
    if (minutes > 0) {
        window->has_window_minutes = TRUE;
        window->window_minutes = minutes;
    }
    if (reset_seconds > 0 && reset_seconds <= G_MAXINT64 / 1000) {
        window->has_resets_at = TRUE;
        window->resets_at_ms = reset_seconds * 1000;
        window->reset_description = reset_description(window->resets_at_ms, now_ms);
    }
    codexbar_provider_add_quota_window(provider, window);
}

CodexBarProvider *codexbar_stepfun_parse_usage(const char *json,
                                               size_t length,
                                               gint64 now_ms,
                                               GError **error) {
    json_object *root = parse_json(json, length);
    json_object *status = member(root, "status");
    if (!root || !status || !json_object_is_type(status, json_type_int)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse StepFun usage response");
        return NULL;
    }
    if (json_object_get_int(status) != 1) {
        char *message = api_message(root);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "StepFun API error: %s", message);
        g_free(message);
        json_object_put(root);
        return NULL;
    }

    double five_left = 0;
    double weekly_left = 0;
    gint64 five_reset = 0;
    gint64 weekly_reset = 0;
    gboolean has_five_left = flexible_double(root, "five_hour_usage_left_rate", &five_left);
    gboolean has_weekly_left = flexible_double(root, "weekly_usage_left_rate", &weekly_left);
    gboolean has_five_reset = flexible_int64(root, "five_hour_usage_reset_time", &five_reset);
    gboolean has_weekly_reset = flexible_int64(root, "weekly_usage_reset_time", &weekly_reset);
    json_object *credit = member(root, "plan_credit_rate_limit");
    double family = 0;
    gboolean live_window = (has_five_reset && five_reset > 0) || (has_weekly_reset && weekly_reset > 0);
    gboolean is_credit = !live_window &&
                         (has_credit_pool(credit) ||
                          (flexible_double(root, "plan_family", &family) && family == 2));
    if (!is_credit && (!has_five_left || !has_weekly_left || !has_five_reset || !has_weekly_reset)) {
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "StepFun response is missing usage rates or reset times");
        return NULL;
    }

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("stepfun");
    provider->source = g_strdup("web");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = g_strdup("password");
    if (is_credit) {
        double left = 0;
        if (!credit_left_rate(credit, &left)) {
            codexbar_provider_free(provider);
            json_object_put(root);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "StepFun credit plan is missing a credit balance");
            return NULL;
        }
        gint64 credit_reset = 0;
        flexible_int64(credit, "subscription_credit_reset_time", &credit_reset);
        add_window(provider, "stepfun.credit", "Credit", left,
                   credit_reset > 0 ? 30 * 24 * 60 : 0, credit_reset, now_ms);
    } else {
        add_window(provider, "stepfun.five-hour", "5h Window", five_left, 300, five_reset, now_ms);
        add_window(provider, "stepfun.weekly", "Weekly Window", weekly_left, 10080, weekly_reset, now_ms);
    }
    json_object_put(root);
    return provider;
}

gboolean codexbar_stepfun_apply_plan(CodexBarProvider *provider,
                                     const char *json,
                                     size_t length) {
    json_object *root = parse_json(json, length);
    json_object *subscription = member(root, "subscription");
    char *name = string_member(subscription, "name");
    if (root) json_object_put(root);
    if (!provider || !provider->identity || !name) {
        g_free(name);
        return FALSE;
    }
    g_free(provider->plan);
    provider->plan = name;
    g_free(provider->identity->login_method);
    provider->identity->login_method = g_strdup(name);
    return TRUE;
}

static char *clean_credential(const char *raw) {
    if (!raw || strlen(raw) > CREDENTIAL_LIMIT || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *copy = g_strstrip(g_strdup(raw));
    size_t length = strlen(copy);
    if (length >= 2 && ((copy[0] == '"' && copy[length - 1] == '"') ||
                        (copy[0] == '\'' && copy[length - 1] == '\''))) {
        copy[length - 1] = '\0';
        memmove(copy, copy + 1, length - 1);
        g_strstrip(copy);
    }
    for (const unsigned char *cursor = (const unsigned char *)copy; *cursor; cursor++) {
        if (g_ascii_iscntrl(*cursor)) {
            g_free(copy);
            return NULL;
        }
    }
    if (copy[0]) return copy;
    g_free(copy);
    return NULL;
}

static char *config_string(const CodexBarProviderConfig *config, const char *key) {
    return string_member(config ? config->raw : NULL, key);
}

static char *normalize_token(const char *raw) {
    char *token = clean_credential(raw);
    if (!token) return NULL;
    char *marker = strstr(token, "Oasis-Token=");
    if (!marker) return token;
    marker += strlen("Oasis-Token=");
    char *end = strchr(marker, ';');
    char *normalized = end ? g_strndup(marker, (size_t)(end - marker)) : g_strdup(marker);
    g_strstrip(normalized);
    g_free(token);
    if (normalized[0]) return normalized;
    g_free(normalized);
    return NULL;
}

static char *device_id_from_jwt(const char *jwt) {
    char **parts = g_strsplit(jwt, ".", 3);
    if (!parts[0] || !parts[1]) {
        g_strfreev(parts);
        return NULL;
    }
    char *payload = g_strdup(parts[1]);
    for (char *cursor = payload; *cursor; cursor++) {
        if (*cursor == '-') *cursor = '+';
        else if (*cursor == '_') *cursor = '/';
    }
    size_t length = strlen(payload);
    size_t padded = (length + 3) / 4 * 4;
    payload = g_realloc(payload, padded + 1);
    while (length < padded) payload[length++] = '=';
    payload[length] = '\0';
    gsize decoded_length = 0;
    guchar *decoded = g_base64_decode(payload, &decoded_length);
    char *device = NULL;
    if (decoded && decoded_length <= RESPONSE_LIMIT) {
        json_object *root = parse_json((const char *)decoded, decoded_length);
        device = string_member(root, "device_id");
        if (root) json_object_put(root);
    }
    g_free(decoded);
    g_free(payload);
    g_strfreev(parts);
    return device;
}

static char *web_id(const char *token) {
    char **halves = g_strsplit(token, "...", 2);
    char *device = halves[1] ? device_id_from_jwt(halves[1]) : NULL;
    if (!device) device = device_id_from_jwt(halves[0]);
    g_strfreev(halves);
    return device ? device : g_strdup(DEFAULT_WEB_ID);
}

static gboolean response_origin_ok(const CodexBarHttpResponse *response) {
    if (!response || !response->effective_url) return TRUE;
    GUri *uri = g_uri_parse(response->effective_url, G_URI_FLAGS_NONE, NULL);
    gboolean valid = uri && g_uri_get_scheme(uri) && g_uri_get_host(uri) &&
                     g_ascii_strcasecmp(g_uri_get_scheme(uri), "https") == 0 &&
                     g_ascii_strcasecmp(g_uri_get_host(uri), "platform.stepfun.com") == 0 &&
                     (g_uri_get_port(uri) < 0 || g_uri_get_port(uri) == 443);
    if (uri) g_uri_unref(uri);
    return valid;
}

static CodexBarHttpResponse *send_request(CodexBarStepFunTransport transport,
                                          const char *url,
                                          const char *method,
                                          const char *body,
                                          const char *token,
                                          const char *ingress,
                                          GCancellable *cancellable,
                                          GError **error) {
    char *webid = token ? web_id(token) : g_strdup(DEFAULT_WEB_ID);
    char *cookie = token && ingress
                       ? g_strdup_printf("Oasis-Token=%s; Oasis-Webid=%s; INGRESSCOOKIE=%s", token, webid, ingress)
                   : token ? g_strdup_printf("Oasis-Token=%s; Oasis-Webid=%s", token, webid)
                   : ingress ? g_strdup_printf("INGRESSCOOKIE=%s", ingress)
                             : NULL;
    CodexBarHttpRequestHeader headers[7] = {
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"oasis-appid", "10300"},
        {"oasis-platform", "web"},
        {"oasis-webid", webid},
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/147 Safari/537.36"},
        {"Cookie", cookie},
    };
    size_t count = cookie ? G_N_ELEMENTS(headers) : G_N_ELEMENTS(headers) - 1;
    CodexBarHttpRequest request = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = count,
        .body = body,
        .body_length = body ? strlen(body) : 0,
        .timeout_seconds = 15,
        .maximum_response_bytes = RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = transport ? transport(&request, error) : NULL;
    g_free(cookie);
    g_free(webid);
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        codexbar_http_response_free(response);
        if (error && *error) g_clear_error(error);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        return NULL;
    }
    if (!transport && (!error || !*error)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "StepFun transport is missing");
    }
    if (response && !response_origin_ok(response)) {
        codexbar_http_response_free(response);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "StepFun redirected outside its trusted origin");
        return NULL;
    }
    return response;
}

static char *response_token(CodexBarHttpResponse *response, const char *operation, GError **error) {
    if (!response) return NULL;
    if (response->status != 200) {
        long status = response->status;
        codexbar_http_response_free(response);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "StepFun %s returned HTTP %ld", operation, status);
        return NULL;
    }
    json_object *root = parse_json(response->body, response->body_length);
    json_object *access = member(root, "accessToken");
    json_object *refresh = member(root, "refreshToken");
    char *access_raw = string_member(access, "raw");
    char *refresh_raw = string_member(refresh, "raw");
    if (root) json_object_put(root);
    codexbar_http_response_free(response);
    if (!access_raw) {
        g_free(refresh_raw);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "StepFun %s response has no access token", operation);
        return NULL;
    }
    char *combined = refresh_raw ? g_strdup_printf("%s...%s", access_raw, refresh_raw) : g_strdup(access_raw);
    g_free(access_raw);
    g_free(refresh_raw);
    return combined;
}

static char *ingress_cookie(CodexBarHttpResponse *response, GError **error) {
    if (!response) return NULL;
    char *cookie = NULL;
    if (response->status == 200) {
        for (guint index = 0; response->headers && index < response->headers->len; index++) {
            CodexBarHttpResponseHeader *header = g_ptr_array_index(response->headers, index);
            if (g_ascii_strcasecmp(header->name, "Set-Cookie") != 0) continue;
            const char *start = strstr(header->value, "INGRESSCOOKIE=");
            if (!start) continue;
            start += strlen("INGRESSCOOKIE=");
            const char *end = strchr(start, ';');
            cookie = end ? g_strndup(start, (size_t)(end - start)) : g_strdup(start);
            g_strstrip(cookie);
            if (cookie[0]) break;
            g_clear_pointer(&cookie, g_free);
        }
    }
    long status = response->status;
    codexbar_http_response_free(response);
    if (!cookie) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "StepFun login could not obtain INGRESSCOOKIE (HTTP %ld)", status);
    }
    return cookie;
}

static char *login(CodexBarStepFunTransport transport,
                   const char *username,
                   const char *password,
                   GCancellable *cancellable,
                   GError **error) {
    CodexBarHttpResponse *response = send_request(
        transport, PLATFORM_URL, "GET", NULL, NULL, NULL, cancellable, error);
    char *ingress = ingress_cookie(response, error);
    if (!ingress) return NULL;
    response = send_request(transport, REGISTER_URL, "POST", "{}", NULL, ingress, cancellable, error);
    char *anonymous = response_token(response, "device registration", error);
    if (!anonymous) {
        g_free(ingress);
        return NULL;
    }
    json_object *body = json_object_new_object();
    json_object_object_add(body, "username", json_object_new_string(username));
    json_object_object_add(body, "password", json_object_new_string(password));
    const char *serialized = json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN);
    response = send_request(transport, LOGIN_URL, "POST", serialized, anonymous, ingress, cancellable, error);
    json_object_put(body);
    g_free(anonymous);
    g_free(ingress);
    return response_token(response, "login", error);
}

static char *refresh(CodexBarStepFunTransport transport,
                     const char *token,
                     GCancellable *cancellable,
                     GError **error) {
    CodexBarHttpResponse *response = send_request(
        transport, REFRESH_URL, "POST", "{}", token, NULL, cancellable, error);
    return response_token(response, "token refresh", error);
}

static CodexBarProvider *query_usage(CodexBarStepFunTransport transport,
                                    const char *token,
                                    GCancellable *cancellable,
                                    gint64 now_ms,
                                    long *status,
                                    GError **error) {
    CodexBarHttpResponse *response = send_request(
        transport, USAGE_URL, "POST", "{}", token, NULL, cancellable, error);
    if (!response) return NULL;
    *status = response->status;
    if (response->status != 200) {
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_stepfun_parse_usage(
        response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    if (!provider) return NULL;
    response = send_request(transport, PLAN_URL, "POST", "{}", token, NULL, cancellable, NULL);
    if (response) {
        if (response->status == 200) codexbar_stepfun_apply_plan(provider, response->body, response->body_length);
        codexbar_http_response_free(response);
    }
    return provider;
}

CodexBarProvider *codexbar_stepfun_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarStepFunTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *cookie_source = config_string(config, "cookieSource");
    if (cookie_source && g_str_equal(cookie_source, "off")) {
        g_free(cookie_source);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "StepFun authentication is disabled");
        return NULL;
    }
    char *manual = config_string(config, "manualToken");
    if (!manual) manual = config_string(config, "cookieHeader");
    if (!manual) manual = config_string(config, "token");
    const char *environment_token = g_getenv("STEPFUN_TOKEN");
    char *token = normalize_token(manual ? manual : config && config->api_key ? config->api_key : environment_token);
    char *username_raw = config_string(config, "username");
    char *password_raw = config_string(config, "password");
    char *username = clean_credential(username_raw ? username_raw : g_getenv("STEPFUN_USERNAME"));
    char *password = clean_credential(password_raw ? password_raw : g_getenv("STEPFUN_PASSWORD"));
    g_free(cookie_source);
    g_free(manual);
    g_free(username_raw);
    g_free(password_raw);
    if (!token && username && password) token = login(transport, username, password, cancellable, error);
    if (!token) {
        g_free(username);
        g_free(password);
        if (!error || !*error) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                "StepFun requires a token or username and password");
        }
        return NULL;
    }

    long status = 0;
    CodexBarProvider *provider = query_usage(transport, token, cancellable, now_ms, &status, error);
    gboolean authentication_failure = status == 401 || status == 403 ||
                                      (error && *error && (*error)->domain == G_IO_ERROR &&
                                       (*error)->code == G_IO_ERROR_PERMISSION_DENIED);
    if (!provider && authentication_failure) {
        if (error) g_clear_error(error);
        char *fresh = refresh(transport, token, cancellable, error);
        if (!fresh && username && password) {
            if (error) g_clear_error(error);
            fresh = login(transport, username, password, cancellable, error);
        }
        if (fresh) {
            g_free(token);
            token = fresh;
            status = 0;
            provider = query_usage(transport, token, cancellable, now_ms, &status, error);
        }
    }
    if (!provider && status && (!error || !*error)) {
        g_set_error(error, G_IO_ERROR,
                    status == 401 || status == 403 ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_FAILED,
                    "StepFun API returned HTTP %ld", status);
    }
    g_free(token);
    g_free(username);
    g_free(password);
    return provider;
}

CodexBarProvider *codexbar_stepfun_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error) {
    return codexbar_stepfun_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}
