#include "kimi.h"

#include "http.h"

#include <json-c/json.h>
#include <errno.h>
#include <math.h>
#include <string.h>

static GQuark kimi_error_quark(void) {
    return g_quark_from_static_string("codexbar-kimi-error");
}

static char *clean_token(const char *raw) {
    if (!raw) return NULL;
    char *clean = g_strdup(raw);
    g_strstrip(clean);
    size_t length = strlen(clean);
    if (length >= 2 && ((clean[0] == '\'' && clean[length - 1] == '\'') ||
                        (clean[0] == '"' && clean[length - 1] == '"'))) {
        clean[length - 1] = '\0';
        memmove(clean, clean + 1, length - 1);
        g_strstrip(clean);
    }
    if (clean[0] != '\0') return clean;
    g_free(clean);
    return NULL;
}

static char *resolve_token(const CodexBarProviderConfig *config, const char *const *environment_keys) {
    char *token = clean_token(config->api_key);
    for (guint index = 0; !token && environment_keys[index]; index++) {
        token = clean_token(g_getenv(environment_keys[index]));
    }
    return token;
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

static CodexBarQuotaWindow *kimi_window(json_object *detail, const char *id, const char *title, gboolean rate) {
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
        window->window_minutes = 300;
        window->detail = g_strdup_printf("Rate: %" G_GINT64_FORMAT "/%" G_GINT64_FORMAT " per 5 hours", used, limit);
    } else {
        window->detail = g_strdup_printf("%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT " requests", used, limit);
    }
    window->has_resets_at = iso_timestamp_ms(detail, &window->resets_at_ms);
    return window;
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

CodexBarProvider *codexbar_kimi_parse_usage(const char *json, gint64 now_ms, GError **error) {
    json_object *root = json_tokener_parse(json);
    json_object *usage = NULL;
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "usage", &usage) || !json_object_is_type(usage, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, kimi_error_quark(), 1, "Kimi usage response is malformed");
        return NULL;
    }
    CodexBarQuotaWindow *weekly = kimi_window(usage, "primary", "weekly", FALSE);
    if (!weekly) {
        json_object_put(root);
        g_set_error_literal(error, kimi_error_quark(), 1, "Kimi usage response is malformed");
        return NULL;
    }

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("kimi");
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    codexbar_provider_add_quota_window(provider, weekly);

    json_object *limits = NULL;
    if (json_object_object_get_ex(root, "limits", &limits) && json_object_is_type(limits, json_type_array) &&
        json_object_array_length(limits) > 0) {
        json_object *first = json_object_array_get_idx(limits, 0);
        json_object *detail = NULL;
        if (json_object_is_type(first, json_type_object) && json_object_object_get_ex(first, "detail", &detail) &&
            json_object_is_type(detail, json_type_object)) {
            CodexBarQuotaWindow *rate = kimi_window(detail, "secondary", "rate limit", TRUE);
            gint64 limit = 0;
            if (rate && json_int64(detail, "limit", &limit) && limit > 0) {
                codexbar_provider_add_quota_window(provider, rate);
            } else {
                codexbar_quota_window_free(rate);
            }
        }
    }
    json_object_put(root);
    return provider;
}

static gboolean fetch_json(
    const char *provider_id, const char *url, const char *token, CodexBarHttpResponse **result, GError **error) {
    char *authorization = g_strdup_printf("Bearer %s", token);
    const CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
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
    *result = codexbar_http_send(&request, error);
    g_free(authorization);
    if (*result) return TRUE;
    if (!error || !*error) g_set_error(error, kimi_error_quark(), 2, "%s request failed", provider_id);
    return FALSE;
}

CodexBarProvider *codexbar_kimi_fetch(const CodexBarProviderConfig *config, GError **error) {
    const char *keys[] = {"KIMI_CODE_API_KEY", NULL};
    char *token = resolve_token(config, keys);
    if (!token) {
        g_set_error_literal(error, kimi_error_quark(), 2,
                            "Kimi Code API key is missing. Set it in config or KIMI_CODE_API_KEY.");
        return NULL;
    }
    const char *base = config->enterprise_host ? config->enterprise_host : g_getenv("KIMI_CODE_BASE_URL");
    char *url = codexbar_kimi_usage_url(base, error);
    if (!url) {
        g_free(token);
        return NULL;
    }
    CodexBarHttpResponse *response = NULL;
    if (!fetch_json("Kimi", url, token, &response, error)) {
        g_free(url);
        g_free(token);
        return NULL;
    }
    g_free(url);
    g_free(token);
    if (response->status != 200) {
        if (response->status == 400) {
            g_set_error_literal(error, kimi_error_quark(), 3, "Invalid Kimi request: Bad request");
        } else if (response->status == 401) {
            g_set_error_literal(error, kimi_error_quark(), 3, "Kimi Code API key is invalid or expired.");
        } else if (response->status == 403) {
            g_set_error_literal(error, kimi_error_quark(), 3, "Kimi API returned HTTP 403 (permission or quota denied)");
        } else {
            g_set_error(error, kimi_error_quark(), 3, "Kimi API returned HTTP %ld", response->status);
        }
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_kimi_parse_usage(response->body, g_get_real_time() / 1000, error);
    codexbar_http_response_free(response);
    return provider;
}
