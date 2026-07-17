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
    window->used_percent = limit > 0 ? CLAMP(((double)used / (double)limit) * 100.0, 0.0, 100.0) : 0.0;
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

static gboolean json_double(json_object *value, double *result) {
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

static json_object *path_value(json_object *context, const char *path) {
    char **parts = g_strsplit(path, ".", -1);
    json_object *value = context;
    for (guint index = 0; parts[index] && value; index++) {
        json_object *next = NULL;
        value = json_object_is_type(value, json_type_object) && json_object_object_get_ex(value, parts[index], &next)
                    ? next
                    : NULL;
    }
    g_strfreev(parts);
    return value;
}

static gboolean context_double(GPtrArray *contexts, const char *const *paths, double *result) {
    for (guint path = 0; paths[path]; path++) {
        for (guint context = 0; context < contexts->len; context++) {
            json_object *value = path_value(g_ptr_array_index(contexts, context), paths[path]);
            if (value && json_double(value, result)) return TRUE;
        }
    }
    return FALSE;
}

static gboolean timestamp_value(json_object *value, gint64 *result) {
    double numeric = 0.0;
    if (json_double(value, &numeric)) {
        if (numeric <= 0.0) return FALSE;
        double seconds = numeric >= 1000000000000.0 ? numeric / 1000.0 : numeric;
        if (!isfinite(seconds) || seconds > 64092211200.0 || seconds * 1000.0 > (double)G_MAXINT64) return FALSE;
        *result = (gint64)(seconds * 1000.0);
        return TRUE;
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    GDateTime *time = g_date_time_new_from_iso8601(json_object_get_string(value), NULL);
    if (!time) return FALSE;
    *result = g_date_time_to_unix(time) * 1000 + g_date_time_get_microsecond(time) / 1000;
    g_date_time_unref(time);
    return TRUE;
}

static gboolean context_timestamp(GPtrArray *contexts, const char *const *paths, gint64 *result) {
    for (guint path = 0; paths[path]; path++) {
        for (guint context = 0; context < contexts->len; context++) {
            json_object *value = path_value(g_ptr_array_index(contexts, context), paths[path]);
            if (value && timestamp_value(value, result)) return TRUE;
        }
    }
    return FALSE;
}

static char *credits_number(double value) {
    char buffer[G_ASCII_DTOSTR_BUF_SIZE];
    char *plain = g_strdup(g_ascii_formatd(buffer, sizeof(buffer), "%.2f", value));
    while (g_str_has_suffix(plain, "0")) plain[strlen(plain) - 1] = '\0';
    if (g_str_has_suffix(plain, ".")) plain[strlen(plain) - 1] = '\0';
    char *dot = strchr(plain, '.');
    size_t integer_length = dot ? (size_t)(dot - plain) : strlen(plain);
    GString *grouped = g_string_new(NULL);
    for (size_t index = 0; index < integer_length; index++) {
        if (index > 0 && (integer_length - index) % 3 == 0) g_string_append_c(grouped, ',');
        g_string_append_c(grouped, plain[index]);
    }
    if (dot) g_string_append(grouped, dot);
    g_free(plain);
    return g_string_free(grouped, FALSE);
}

CodexBarProvider *codexbar_kimik2_parse_credits(
    const char *json, const char *remaining_header, gint64 now_ms, GError **error) {
    json_object *root = json_tokener_parse(json);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, kimi_error_quark(), 4, "Kimi K2 credits response root is not an object");
        return NULL;
    }
    GPtrArray *contexts = g_ptr_array_new();
    g_ptr_array_add(contexts, root);
    const char *containers[] = {"data", "result", NULL};
    for (guint index = 0; containers[index]; index++) {
        json_object *container = NULL;
        if (!json_object_object_get_ex(root, containers[index], &container) ||
            !json_object_is_type(container, json_type_object)) {
            continue;
        }
        g_ptr_array_add(contexts, container);
        json_object *nested = NULL;
        if (json_object_object_get_ex(container, "usage", &nested) && json_object_is_type(nested, json_type_object))
            g_ptr_array_add(contexts, nested);
        if (json_object_object_get_ex(container, "credits", &nested) && json_object_is_type(nested, json_type_object))
            g_ptr_array_add(contexts, nested);
    }
    json_object *nested = NULL;
    if (json_object_object_get_ex(root, "usage", &nested) && json_object_is_type(nested, json_type_object))
        g_ptr_array_add(contexts, nested);
    if (json_object_object_get_ex(root, "credits", &nested) && json_object_is_type(nested, json_type_object))
        g_ptr_array_add(contexts, nested);

    const char *remaining_paths[] = {
        "credits_remaining", "creditsRemaining", "remaining_credits", "remainingCredits",
        "available_credits", "availableCredits", "credits_left", "creditsLeft",
        "usage.credits_remaining", "usage.remaining", NULL,
    };
    const char *timestamp_paths[] = {"updated_at", "updatedAt", "timestamp", "time", "last_update", "lastUpdated", NULL};
    double remaining = 0.0;
    if (!context_double(contexts, remaining_paths, &remaining) && remaining_header) {
        json_object *header = json_object_new_string(remaining_header);
        if (!json_double(header, &remaining)) remaining = 0.0;
        json_object_put(header);
    }
    remaining = MAX(0.0, remaining);
    gint64 updated_at_ms = now_ms;
    context_timestamp(contexts, timestamp_paths, &updated_at_ms);

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("kimik2");
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = updated_at_ms;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    char *number = credits_number(remaining);
    provider->identity->login_method = g_strdup_printf("Credits: %s left", number);
    g_free(number);
    g_ptr_array_unref(contexts);
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_kimik2_fetch(const CodexBarProviderConfig *config, GError **error) {
    const char *keys[] = {"KIMI_K2_API_KEY", "KIMI_API_KEY", "KIMI_KEY", NULL};
    char *token = resolve_token(config, keys);
    if (!token) {
        g_set_error_literal(error, kimi_error_quark(), 5, "Missing Kimi K2 API key.");
        return NULL;
    }
    CodexBarHttpResponse *response = NULL;
    if (!fetch_json("Kimi K2", "https://kimi-k2.ai/api/user/credits", token, &response, error)) {
        g_free(token);
        return NULL;
    }
    g_free(token);
    if (response->status != 200) {
        if (response->status == 401) {
            g_set_error_literal(error, kimi_error_quark(), 6, "Kimi K2 API key is invalid or expired.");
        } else {
            const char *body = response->body && response->body[0] != '\0' &&
                                       g_utf8_validate(response->body, (gssize)response->body_length, NULL)
                                   ? response->body
                                   : NULL;
            char *fallback = body ? NULL : g_strdup_printf("HTTP %ld", response->status);
            g_set_error(error,
                        kimi_error_quark(),
                        6,
                        "Kimi K2 API error: %s",
                        body ? body : fallback);
            g_free(fallback);
        }
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_kimik2_parse_credits(
        response->body,
        codexbar_http_response_header_first(response, "X-Credits-Remaining"),
        g_get_real_time() / 1000,
        error);
    codexbar_http_response_free(response);
    return provider;
}
