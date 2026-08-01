#include "kimi.h"

#include "http.h"

#include <json-c/json.h>
#include <errno.h>
#include <math.h>
#include <string.h>

#define KIMI_RESPONSE_LIMIT (1024U * 1024U)
#define KIMI_CREDENTIAL_LIMIT (16U * 1024U)
#define KIMI_WEB_USAGE_URL "https://www.kimi.com/apiv2/kimi.gateway.billing.v1.BillingService/GetUsages"

static GQuark kimi_error_quark(void) {
    return g_quark_from_static_string("codexbar-kimi-error");
}

static char *clean_token(const char *raw) {
    if (!raw || strlen(raw) > KIMI_CREDENTIAL_LIMIT || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *clean = g_strdup(raw);
    g_strstrip(clean);
    size_t length = strlen(clean);
    if (length >= 2 && ((clean[0] == '\'' && clean[length - 1] == '\'') ||
                        (clean[0] == '"' && clean[length - 1] == '"'))) {
        clean[length - 1] = '\0';
        memmove(clean, clean + 1, length - 1);
        g_strstrip(clean);
    }
    for (const unsigned char *cursor = (const unsigned char *)clean; *cursor; cursor++) {
        if (*cursor < 33 || *cursor == 127) {
            g_free(clean);
            return NULL;
        }
    }
    if (clean[0] != '\0') return clean;
    g_free(clean);
    return NULL;
}

static char *resolve_token(const CodexBarProviderConfig *config, const char *const *environment_keys) {
    char *token = clean_token(config ? config->api_key : NULL);
    for (guint index = 0; !token && environment_keys[index]; index++) {
        token = clean_token(g_getenv(environment_keys[index]));
    }
    return token;
}

static json_object *parse_json_document(const char *json) {
    if (!json || !g_utf8_validate(json, -1, NULL) || strlen(json) > KIMI_RESPONSE_LIMIT) return NULL;
    size_t length = strlen(json);
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, json, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && g_ascii_isspace(json[consumed])) consumed++;
    gboolean valid = parse_error == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static gboolean json_int64(json_object *object, const char *key, gint64 *result) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value)) return FALSE;
    if (json_object_is_type(value, json_type_int)) {
        *result = json_object_get_int64(value);
        return TRUE;
    }
    if (json_object_is_type(value, json_type_double)) {
        double number = json_object_get_double(value);
        if (!isfinite(number) || trunc(number) != number || number < -9223372036854775808.0 ||
            number >= 9223372036854775808.0) {
            return FALSE;
        }
        *result = (gint64)number;
        return TRUE;
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    char *clean = g_strdup(json_object_get_string(value));
    g_strstrip(clean);
    char *end = NULL;
    errno = 0;
    gint64 number = g_ascii_strtoll(clean, &end, 10);
    gboolean valid = clean[0] != '\0' && end && *end == '\0' && errno != ERANGE;
    g_free(clean);
    if (!valid) return FALSE;
    *result = number;
    return TRUE;
}

static gboolean iso_timestamp_ms(json_object *object, gint64 *result) {
    const char *keys[] = {"resetTime", "resetAt", "reset_time", "reset_at"};
    for (guint index = 0; index < G_N_ELEMENTS(keys); index++) {
        json_object *value = NULL;
        if (!json_object_object_get_ex(object, keys[index], &value) ||
            !json_object_is_type(value, json_type_string)) {
            continue;
        }
        GDateTime *time = g_date_time_new_from_iso8601(json_object_get_string(value), NULL);
        if (!time) continue;
        *result = g_date_time_to_unix(time) * 1000 + g_date_time_get_microsecond(time) / 1000;
        g_date_time_unref(time);
        return TRUE;
    }
    return FALSE;
}

static CodexBarQuotaWindow *kimi_window(
    json_object *detail, const char *id, const char *title, gboolean rate, gint64 rate_minutes) {
    gint64 limit = 0;
    gint64 remaining = 0;
    gint64 used = 0;
    gboolean has_remaining = json_int64(detail, "remaining", &remaining);
    gboolean has_used = json_int64(detail, "used", &used);
    if (!json_int64(detail, "limit", &limit)) return NULL;
    if (!has_used && has_remaining) {
        long double derived = (long double)limit - (long double)remaining;
        used = derived <= 0.0L ? 0 : derived >= (long double)G_MAXINT64 ? G_MAXINT64 : (gint64)derived;
    }

    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = limit > 0
                               ? codexbar_usage_percent_display(
                                     codexbar_usage_percent_from_ratio((double)used, (double)limit))
                               : 0.0;
    if (rate) {
        window->has_window_minutes = TRUE;
        window->window_minutes = rate_minutes;
        if (rate_minutes % 60 == 0) {
            gint64 hours = rate_minutes / 60;
            window->detail = g_strdup_printf("Rate: %" G_GINT64_FORMAT "/%" G_GINT64_FORMAT
                                             " per %" G_GINT64_FORMAT " %s",
                                             used,
                                             limit,
                                             hours,
                                             hours == 1 ? "hour" : "hours");
        } else {
            window->detail = g_strdup_printf("Rate: %" G_GINT64_FORMAT "/%" G_GINT64_FORMAT
                                             " per %" G_GINT64_FORMAT " %s",
                                             used,
                                             limit,
                                             rate_minutes,
                                             rate_minutes == 1 ? "minute" : "minutes");
        }
    } else {
        window->detail = g_strdup_printf("%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT " requests", used, limit);
    }
    window->has_resets_at = iso_timestamp_ms(detail, &window->resets_at_ms);
    return window;
}

static gint64 kimi_rate_limit_minutes(json_object *limit) {
    json_object *window = NULL;
    json_object *unit_value = NULL;
    gint64 duration = 0;
    if (!json_object_object_get_ex(limit, "window", &window) || !json_object_is_type(window, json_type_object) ||
        !json_int64(window, "duration", &duration) || duration <= 0 ||
        !json_object_object_get_ex(window, "timeUnit", &unit_value) ||
        !json_object_is_type(unit_value, json_type_string)) {
        return 300;
    }
    const char *unit = json_object_get_string(unit_value);
    gint64 multiplier = g_str_equal(unit, "TIME_UNIT_MINUTE") ? 1
                        : g_str_equal(unit, "TIME_UNIT_HOUR")  ? 60
                        : g_str_equal(unit, "TIME_UNIT_DAY")   ? 24 * 60
                                                               : 0;
    if (multiplier == 0 || duration > G_MAXINT64 / multiplier) return 300;
    return duration * multiplier;
}

char *codexbar_kimi_usage_url(const char *base_url, GError **error) {
    char *normalized = codexbar_http_normalize_endpoint(
        base_url && base_url[0] != '\0' ? base_url : "https://api.kimi.com", CODEXBAR_HTTP_HTTPS_ONLY, error);
    if (!normalized) return NULL;
    GUri *uri = g_uri_parse(normalized, G_URI_FLAGS_NONE, NULL);
    if (!uri || g_uri_get_query(uri) || g_uri_get_fragment(uri)) {
        if (uri) g_uri_unref(uri);
        g_free(normalized);
        g_set_error_literal(error, kimi_error_quark(), 2, "Kimi Code base URL cannot contain a query or fragment");
        return NULL;
    }
    g_uri_unref(uri);
    while (g_str_has_suffix(normalized, "/")) normalized[strlen(normalized) - 1] = '\0';
    char *url = NULL;
    if (g_str_has_suffix(normalized, "/coding/v1")) {
        url = g_strdup_printf("%s/usages", normalized);
    } else if (g_str_has_suffix(normalized, "/coding")) {
        url = g_strdup_printf("%s/v1/usages", normalized);
    } else {
        url = g_strdup_printf("%s/coding/v1/usages", normalized);
    }
    g_free(normalized);
    return url;
}

static CodexBarProvider *kimi_provider(json_object *usage,
                                      json_object *limits,
                                      const char *source,
                                      gint64 now_ms,
                                      GError **error) {
    CodexBarQuotaWindow *weekly = kimi_window(usage, "primary", "weekly", FALSE, 0);
    if (!weekly) {
        g_set_error_literal(error, kimi_error_quark(), 1, "Kimi usage response is malformed");
        return NULL;
    }

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("kimi");
    provider->source = g_strdup(source);
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    codexbar_provider_add_quota_window(provider, weekly);

    if (limits && json_object_is_type(limits, json_type_array) && json_object_array_length(limits) > 0) {
        json_object *first = json_object_array_get_idx(limits, 0);
        json_object *detail = NULL;
        if (json_object_is_type(first, json_type_object) && json_object_object_get_ex(first, "detail", &detail) &&
            json_object_is_type(detail, json_type_object)) {
            CodexBarQuotaWindow *rate =
                kimi_window(detail, "secondary", "rate limit", TRUE, kimi_rate_limit_minutes(first));
            gint64 limit = 0;
            if (rate && json_int64(detail, "limit", &limit) && limit > 0) {
                codexbar_provider_add_quota_window(provider, rate);
            } else {
                codexbar_quota_window_free(rate);
            }
        }
    }
    return provider;
}

CodexBarProvider *codexbar_kimi_parse_usage(const char *json, gint64 now_ms, GError **error) {
    json_object *root = parse_json_document(json);
    json_object *usage = NULL;
    json_object *limits = NULL;
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "usage", &usage) || !json_object_is_type(usage, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, kimi_error_quark(), 1, "Kimi usage response is malformed");
        return NULL;
    }
    json_object_object_get_ex(root, "limits", &limits);
    CodexBarProvider *provider = kimi_provider(usage, limits, "api", now_ms, error);
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_kimi_parse_web_usage(const char *json, gint64 now_ms, GError **error) {
    json_object *root = parse_json_document(json);
    json_object *usages = NULL;
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "usages", &usages) || !json_object_is_type(usages, json_type_array)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, kimi_error_quark(), 1, "Kimi web usage response is malformed");
        return NULL;
    }
    CodexBarProvider *provider = NULL;
    size_t count = json_object_array_length(usages);
    for (size_t index = 0; index < count && !provider; index++) {
        json_object *usage = json_object_array_get_idx(usages, index);
        json_object *scope = NULL;
        json_object *detail = NULL;
        json_object *limits = NULL;
        if (!json_object_is_type(usage, json_type_object) ||
            !json_object_object_get_ex(usage, "scope", &scope) || !json_object_is_type(scope, json_type_string) ||
            !g_str_equal(json_object_get_string(scope), "FEATURE_CODING") ||
            !json_object_object_get_ex(usage, "detail", &detail) || !json_object_is_type(detail, json_type_object)) {
            continue;
        }
        json_object_object_get_ex(usage, "limits", &limits);
        provider = kimi_provider(detail, limits, "web", now_ms, error);
    }
    json_object_put(root);
    if (!provider && (!error || !*error)) {
        g_set_error_literal(error, kimi_error_quark(), 1,
                            "Kimi web usage response has no FEATURE_CODING scope");
    }
    return provider;
}

static CodexBarHttpResponse *kimi_request(const char *url,
                                          const char *method,
                                          const char *token,
                                          const char *body,
                                          gboolean web,
                                          CodexBarKimiTransport transport,
                                          GCancellable *cancellable,
                                          GError **error) {
    char *authorization = g_strdup_printf("Bearer %s", token);
    char *cookie = web ? g_strdup_printf("kimi-auth=%s", token) : NULL;
    const CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"Cookie", cookie},
        {"Origin", "https://www.kimi.com"},
        {"Referer", "https://www.kimi.com/code/console"},
        {"Connect-Protocol-Version", "1"},
        {"X-Msh-Platform", "web"},
    };
    CodexBarHttpRequest request = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = web ? G_N_ELEMENTS(headers) : 2,
        .body = body,
        .body_length = body ? strlen(body) : 0,
        .timeout_seconds = 15,
        .maximum_response_bytes = KIMI_RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *result = transport(&request, error);
    g_free(cookie);
    g_free(authorization);
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        codexbar_http_response_free(result);
        if (error && *error) g_clear_error(error);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        return NULL;
    }
    if (!result && (!error || !*error)) g_set_error_literal(error, kimi_error_quark(), 2, "Kimi request failed");
    return result;
}

static CodexBarProvider *kimi_fetch_api_token(const CodexBarProviderConfig *config,
                                             const char *token,
                                             CodexBarKimiTransport transport,
                                             GCancellable *cancellable,
                                             gint64 now_ms,
                                             GError **error) {
    const char *base = config && config->enterprise_host ? config->enterprise_host : g_getenv("KIMI_CODE_BASE_URL");
    char *url = codexbar_kimi_usage_url(base, error);
    if (!url) return NULL;
    CodexBarHttpResponse *response = kimi_request(url, "GET", token, NULL, FALSE, transport, cancellable, error);
    g_free(url);
    if (!response) return NULL;
    if (response->status != 200) {
        long status = response->status;
        codexbar_http_response_free(response);
        if (status == 401) {
            g_set_error_literal(error, kimi_error_quark(), 3, "Kimi Code API key is invalid or expired.");
        } else {
            g_set_error(error, kimi_error_quark(), 3, "Kimi API returned HTTP %ld", status);
        }
        return NULL;
    }
    CodexBarProvider *provider = codexbar_kimi_parse_usage(response->body, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

static char *config_string(const CodexBarProviderConfig *config, const char *field) {
    json_object *value = NULL;
    if (!config || !config->raw || !json_object_object_get_ex(config->raw, field, &value) ||
        !json_object_is_type(value, json_type_string)) return NULL;
    return g_strdup(json_object_get_string(value));
}

static char *kimi_web_token_from_raw(const char *raw) {
    if (!raw || !g_utf8_validate(raw, -1, NULL) || strlen(raw) > KIMI_CREDENTIAL_LIMIT) return NULL;
    char *lower = g_ascii_strdown(raw, -1);
    char *match = strstr(lower, "kimi-auth=");
    if (!match) match = strstr(lower, "kimi-auth:");
    char *token = NULL;
    if (match) {
        size_t offset = (size_t)(match - lower) + strlen("kimi-auth=");
        while (g_ascii_isspace(raw[offset])) offset++;
        size_t length = strspn(raw + offset, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789._-+=/");
        if (length > 0) token = g_strndup(raw + offset, length);
    }
    g_free(lower);
    if (token) return token;
    char *clean = clean_token(raw);
    if (!clean) return NULL;
    guint dots = 0;
    for (const char *cursor = clean; *cursor; cursor++) dots += *cursor == '.';
    if (g_str_has_prefix(clean, "eyJ") && dots == 2) return clean;
    g_free(clean);
    return NULL;
}

static char *kimi_web_token(const CodexBarProviderConfig *config) {
    char *raw = config_string(config, "cookieHeader");
    if (!raw) raw = config_string(config, "manualCookieHeader");
    if (!raw) raw = g_strdup(g_getenv("KIMI_MANUAL_COOKIE"));
    if (!raw) raw = g_strdup(g_getenv("KIMI_AUTH_TOKEN"));
    char *token = kimi_web_token_from_raw(raw);
    g_free(raw);
    return token;
}

static char *kimi_cli_token(const CodexBarProviderConfig *config, gint64 now_ms) {
    char *path = config_string(config, "codeCredentialsPath");
    if (!path) {
        const char *home = g_getenv("KIMI_CODE_HOME");
        char *root = home && home[0] ? g_strdup(home) : g_build_filename(g_get_home_dir(), ".kimi-code", NULL);
        path = g_build_filename(root, "credentials", "kimi-code.json", NULL);
        g_free(root);
    }
    char *contents = NULL;
    gsize length = 0;
    gboolean loaded = g_file_get_contents(path, &contents, &length, NULL);
    g_free(path);
    if (!loaded || length > KIMI_CREDENTIAL_LIMIT || memchr(contents, '\0', length)) {
        g_free(contents);
        return NULL;
    }
    json_object *root = parse_json_document(contents);
    g_free(contents);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        return NULL;
    }
    json_object *access = NULL;
    json_object *expiry = NULL;
    char *token = NULL;
    if (json_object_object_get_ex(root, "access_token", &access) && json_object_is_type(access, json_type_string)) {
        token = clean_token(json_object_get_string(access));
    }
    if (token && json_object_object_get_ex(root, "expires_at", &expiry)) {
        double expires_at = json_object_get_double(expiry);
        if (!isfinite(expires_at) || expires_at * 1000 <= (double)now_ms + 60000) g_clear_pointer(&token, g_free);
    }
    json_object_put(root);
    return token;
}

static CodexBarProvider *kimi_fetch_web(const CodexBarProviderConfig *config,
                                       CodexBarKimiTransport transport,
                                       GCancellable *cancellable,
                                       gint64 now_ms,
                                       GError **error) {
    char *token = kimi_web_token(config);
    if (!token) {
        g_set_error_literal(error, kimi_error_quark(), 2,
                            "Kimi web token is missing; configure cookieHeader or KIMI_AUTH_TOKEN");
        return NULL;
    }
    const char *body = "{\"scope\":[\"FEATURE_CODING\"]}";
    CodexBarHttpResponse *response = kimi_request(
        KIMI_WEB_USAGE_URL, "POST", token, body, TRUE, transport, cancellable, error);
    g_free(token);
    if (!response) return NULL;
    if (response->status != 200) {
        long status = response->status;
        codexbar_http_response_free(response);
        if (status == 401 || status == 403) {
            g_set_error_literal(error, kimi_error_quark(), 3, "Kimi web session is invalid or expired");
        } else {
            g_set_error(error, kimi_error_quark(), 3, "Kimi web API returned HTTP %ld", status);
        }
        return NULL;
    }
    CodexBarProvider *provider = codexbar_kimi_parse_web_usage(response->body, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

CodexBarProvider *codexbar_kimi_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    CodexBarKimiTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    const char *mode = source && source[0] ? source : "auto";
    if (g_str_equal(mode, "web")) return kimi_fetch_web(config, transport, cancellable, now_ms, error);
    const char *keys[] = {"KIMI_CODE_API_KEY", NULL};
    char *token = resolve_token(config, keys);
    if (g_str_equal(mode, "api")) {
        if (!token) {
            g_set_error_literal(error, kimi_error_quark(), 2,
                                "Kimi Code API key is missing. Set it in config or KIMI_CODE_API_KEY.");
            return NULL;
        }
        CodexBarProvider *provider = kimi_fetch_api_token(
            config, token, transport, cancellable, now_ms, error);
        g_free(token);
        return provider;
    }
    if (!g_str_equal(mode, "auto")) {
        g_free(token);
        g_set_error(error, kimi_error_quark(), 2, "Kimi source '%s' is unsupported", mode);
        return NULL;
    }
    if (token) {
        GError *api_error = NULL;
        CodexBarProvider *provider = kimi_fetch_api_token(
            config, token, transport, cancellable, now_ms, &api_error);
        g_free(token);
        if (provider) return provider;
        if (api_error && g_error_matches(api_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            if (error) *error = api_error;
            else g_clear_error(&api_error);
            return NULL;
        }
        g_clear_error(&api_error);
    }
    char *cli_token = kimi_cli_token(config, now_ms);
    if (cli_token) {
        GError *cli_error = NULL;
        CodexBarProvider *provider = kimi_fetch_api_token(
            config, cli_token, transport, cancellable, now_ms, &cli_error);
        g_free(cli_token);
        if (provider) {
            g_free(provider->source);
            provider->source = g_strdup("oauth");
            return provider;
        }
        if (cli_error && g_error_matches(cli_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            if (error) *error = cli_error;
            else g_clear_error(&cli_error);
            return NULL;
        }
        g_clear_error(&cli_error);
    }
    return kimi_fetch_web(config, transport, cancellable, now_ms, error);
}

CodexBarProvider *codexbar_kimi_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                       const char *source,
                                                       GCancellable *cancellable,
                                                       GError **error) {
    return codexbar_kimi_fetch_with_transport_and_cancellable(
        config, source, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_kimi_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_kimi_fetch_with_cancellable(config, "auto", NULL, error);
}
