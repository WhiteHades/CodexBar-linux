#include "codebuff.h"

#include "http.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>

static GQuark codebuff_error_quark(void) {
    return g_quark_from_static_string("codexbar-codebuff-error");
}

static json_object *parse_json(const char *json) {
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
    size_t length = strlen(json);
    json_object *root = json_tokener_parse_ex(tokener, json, (int)length);
    size_t parsed = json_tokener_get_parse_end(tokener);
    gboolean valid = json_tokener_get_error(tokener) == json_tokener_success;
    while (valid && parsed < length) {
        if (!g_ascii_isspace(json[parsed])) valid = FALSE;
        parsed++;
    }
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static char *clean_value(const char *raw) {
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

static gboolean number_value(json_object *value, double *result) {
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        *result = json_object_get_double(value);
        return isfinite(*result);
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    char *clean = g_strdup(json_object_get_string(value));
    g_strstrip(clean);
    char *end = NULL;
    double number = g_ascii_strtod(clean, &end);
    gboolean valid = clean[0] != '\0' && end && *end == '\0' && isfinite(number);
    g_free(clean);
    if (!valid) return FALSE;
    *result = number;
    return TRUE;
}

static gboolean first_number(json_object *root, const char *primary, const char *fallback, double *result) {
    json_object *value = NULL;
    if (json_object_object_get_ex(root, primary, &value) && number_value(value, result)) return TRUE;
    return json_object_object_get_ex(root, fallback, &value) && number_value(value, result);
}

static gboolean date_value(json_object *value, gint64 *result) {
    double numeric = 0.0;
    if (number_value(value, &numeric)) {
        double seconds = numeric > 10000000000.0 ? numeric / 1000.0 : numeric;
        long double milliseconds = (long double)seconds * 1000.0L;
        if (!isfinite(seconds) || milliseconds >= (long double)G_MAXINT64 ||
            milliseconds <= (long double)G_MININT64) {
            return FALSE;
        }
        gint64 parsed = (gint64)milliseconds;
        GDateTime *time = g_date_time_new_from_unix_utc(parsed / 1000);
        if (!time) return FALSE;
        g_date_time_unref(time);
        *result = parsed;
        return TRUE;
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    GDateTime *time = g_date_time_new_from_iso8601(json_object_get_string(value), NULL);
    if (!time) return FALSE;
    *result = g_date_time_to_unix(time) * 1000 + g_date_time_get_microsecond(time) / 1000;
    g_date_time_unref(time);
    return TRUE;
}

static char *compact_number(double value) {
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    char *plain = fabs(value) >= 1000.0
                      ? g_strdup_printf("%.0f", value)
                      : g_strdup(g_ascii_formatd(buffer, sizeof(buffer), "%.1f", value));
    char *dot = strchr(plain, '.');
    if (dot && dot[1] == '0' && dot[2] == '\0') *dot = '\0';
    dot = strchr(plain, '.');
    size_t integer_length = dot ? (size_t)(dot - plain) : strlen(plain);
    size_t start = plain[0] == '-' ? 1 : 0;
    GString *result = g_string_new_len(plain, start);
    for (size_t index = start; index < integer_length; index++) {
        if (index > start && (integer_length - index) % 3 == 0) g_string_append_c(result, ',');
        g_string_append_c(result, plain[index]);
    }
    if (dot) g_string_append(result, dot);
    g_free(plain);
    return g_string_free(result, FALSE);
}

CodexBarProvider *codexbar_codebuff_parse_usage(const char *json, gint64 now_ms, GError **error) {
    json_object *root = parse_json(json);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, codebuff_error_quark(), 1, "Codebuff usage response is malformed");
        return NULL;
    }
    double used = 0.0;
    double total = 0.0;
    double remaining = 0.0;
    gboolean has_used = first_number(root, "usage", "used", &used);
    gboolean has_total = first_number(root, "quota", "limit", &total);
    gboolean has_remaining = first_number(root, "remainingBalance", "remaining", &remaining);
    json_object *auto_topup = NULL;
    gboolean auto_topup_enabled = FALSE;
    gboolean has_auto_topup = FALSE;
    gboolean found_auto_topup = json_object_object_get_ex(root, "autoTopupEnabled", &auto_topup) &&
                                json_object_is_type(auto_topup, json_type_boolean);
    if (!found_auto_topup) {
        found_auto_topup = json_object_object_get_ex(root, "auto_topup_enabled", &auto_topup) &&
                           json_object_is_type(auto_topup, json_type_boolean);
    }
    if (found_auto_topup) {
        has_auto_topup = TRUE;
        auto_topup_enabled = json_object_get_boolean(auto_topup);
    }
    json_object *reset = NULL;
    gint64 reset_ms = 0;
    gboolean has_reset = json_object_object_get_ex(root, "next_quota_reset", &reset) && date_value(reset, &reset_ms);

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("codebuff");
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    if (has_total) total = MAX(0.0, total);
    if (!has_total && has_used && has_remaining) {
        total = MAX(0.0, used + remaining);
        has_total = TRUE;
    }
    if (has_total && total > 0.0) {
        double resolved_used = has_used ? MAX(0.0, used) : has_remaining ? MAX(0.0, total - remaining) : 0.0;
        CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "credits");
        window->usage_known = TRUE;
        window->used_percent = codexbar_usage_percent_display(
            codexbar_usage_percent_from_ratio(resolved_used, total));
        window->has_resets_at = has_reset;
        window->resets_at_ms = reset_ms;
        codexbar_provider_add_quota_window(provider, window);
    } else if (has_used || has_remaining) {
        CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "credits");
        window->usage_known = TRUE;
        window->used_percent = 100.0;
        window->has_resets_at = has_reset;
        window->resets_at_ms = reset_ms;
        codexbar_provider_add_quota_window(provider, window);
    }
    if (has_remaining || (has_auto_topup && auto_topup_enabled)) {
        char *number = has_remaining ? compact_number(remaining) : NULL;
        if (number && auto_topup_enabled) {
            provider->identity->login_method = g_strdup_printf("%s remaining · auto top-up", number);
        } else if (number) {
            provider->identity->login_method = g_strdup_printf("%s remaining", number);
        } else {
            provider->identity->login_method = g_strdup("auto top-up");
        }
        g_free(number);
    }
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_codebuff_fetch(const CodexBarProviderConfig *config, GError **error) {
    char *token = clean_value(config->api_key);
    if (!token) token = clean_value(g_getenv("CODEBUFF_API_KEY"));
    if (!token) {
        g_set_error_literal(error, codebuff_error_quark(), 2, "Codebuff API token is not configured.");
        return NULL;
    }
    char *base = clean_value(g_getenv("CODEBUFF_API_URL"));
    if (!base) base = g_strdup("https://www.codebuff.com");
    char *normalized = codexbar_http_normalize_endpoint(base, CODEXBAR_HTTP_HTTPS_ONLY, error);
    g_free(base);
    if (!normalized) {
        g_free(token);
        return NULL;
    }
    GUri *uri = g_uri_parse(normalized, G_URI_FLAGS_NONE, NULL);
    if (!uri || g_uri_get_query(uri) || g_uri_get_fragment(uri)) {
        if (uri) g_uri_unref(uri);
        g_free(normalized);
        g_free(token);
        g_set_error_literal(error, codebuff_error_quark(), 2, "Codebuff API URL cannot contain a query or fragment");
        return NULL;
    }
    g_uri_unref(uri);
    while (g_str_has_suffix(normalized, "/")) normalized[strlen(normalized) - 1] = '\0';
    char *url = g_strdup_printf("%s/api/v1/usage", normalized);
    g_free(normalized);
    char *authorization = g_strdup_printf("Bearer %s", token);
    g_free(token);
    const char body[] = "{\"fingerprintId\":\"codexbar-usage\"}";
    const CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "POST",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .body = body,
        .body_length = sizeof(body) - 1,
        .timeout_seconds = 15,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
    };
    CodexBarHttpResponse *response = codexbar_http_send(&request, error);
    g_free(authorization);
    g_free(url);
    if (!response) return NULL;
    if (response->status != 200) {
        if (response->status == 401 || response->status == 403) {
            g_set_error_literal(error, codebuff_error_quark(), 3, "Codebuff API token is unauthorized.");
        } else if (response->status == 404) {
            g_set_error_literal(error, codebuff_error_quark(), 3, "Codebuff usage endpoint was not found.");
        } else if (response->status >= 500 && response->status < 600) {
            g_set_error(error, codebuff_error_quark(), 3, "Codebuff service returned HTTP %ld.", response->status);
        } else {
            g_set_error(error, codebuff_error_quark(), 3, "Codebuff API returned HTTP %ld.", response->status);
        }
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_codebuff_parse_usage(response->body, g_get_real_time() / 1000, error);
    codexbar_http_response_free(response);
    return provider;
}
