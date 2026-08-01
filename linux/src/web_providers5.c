#include "web_providers5.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define WEB5_RESPONSE_LIMIT (1024U * 1024U)
#define WEB5_CREDENTIAL_LIMIT 16384U
#define WEB5_USER_AGENT \
    "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/143.0 Safari/537.36"

static json_object *parse_json(const char *text, size_t length) {
    if (!text || !length || length > WEB5_RESPONSE_LIMIT || length > G_MAXINT ||
        memchr(text, '\0', length) || !g_utf8_validate(text, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, text, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && strchr(" \t\r\n", text[consumed])) consumed++;
    json_tokener_free(tokener);
    if (parse_error == json_tokener_success && root && consumed == length) return root;
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

static gboolean number(json_object *value, double *result) {
    if (!value || json_object_is_type(value, json_type_boolean) ||
        json_object_is_type(value, json_type_null)) return FALSE;
    double parsed = 0;
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        parsed = json_object_get_double(value);
    } else if (json_object_is_type(value, json_type_string)) {
        const char *text = json_object_get_string(value);
        char *end = NULL;
        parsed = g_ascii_strtod(text, &end);
        if (!text[0] || !end || *end) return FALSE;
    } else {
        return FALSE;
    }
    if (!isfinite(parsed)) return FALSE;
    *result = parsed;
    return TRUE;
}

static char *string_value(json_object *value) {
    if (!value || !json_object_is_type(value, json_type_string)) return NULL;
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!text || memchr(text, '\0', length) || !g_utf8_validate(text, (gssize)length, NULL)) return NULL;
    char *copy = g_strndup(text, length);
    g_strstrip(copy);
    if (copy[0]) return copy;
    g_free(copy);
    return NULL;
}

static char *string_member(json_object *object, const char *key) {
    return string_value(member(object, key));
}

static gboolean timestamp(json_object *value, gint64 *result) {
    double raw = 0;
    if (number(value, &raw)) {
        if (raw < 100000000000.0) raw *= 1000;
        if (raw <= 0 || raw > (double)G_MAXINT64) return FALSE;
        *result = (gint64)llround(raw);
        return TRUE;
    }
    char *text = string_value(value);
    if (!text) return FALSE;
    GDateTime *date = g_date_time_new_from_iso8601(text, NULL);
    if (!date) {
        char *iso = g_strdup_printf("%.10sT%sZ", text, strlen(text) > 11 ? text + 11 : "00:00:00");
        date = g_date_time_new_from_iso8601(iso, NULL);
        g_free(iso);
    }
    g_free(text);
    if (!date) return FALSE;
    *result = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static char *clean_header(const char *raw) {
    if (!raw || strlen(raw) > WEB5_CREDENTIAL_LIMIT || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *value = g_strdup(raw);
    g_strstrip(value);
    char *lower = g_ascii_strdown(value, -1);
    char *marker = strstr(lower, "cookie:");
    if (marker) {
        size_t offset = (size_t)(marker - lower) + 7;
        memmove(value, value + offset, strlen(value + offset) + 1);
        g_strstrip(value);
    }
    g_free(lower);
    size_t length = strlen(value);
    if (length > 1 && ((value[0] == '\'' && value[length - 1] == '\'') ||
                       (value[0] == '"' && value[length - 1] == '"'))) {
        memmove(value, value + 1, length - 2);
        value[length - 2] = '\0';
        g_strstrip(value);
    } else if (length > 0 && (value[length - 1] == '\'' || value[length - 1] == '"')) {
        value[length - 1] = '\0';
        g_strstrip(value);
    }
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (*cursor < 32 || *cursor == 127) {
            g_free(value);
            return NULL;
        }
    }
    if (value[0]) return value;
    g_free(value);
    return NULL;
}

static char *config_string(const CodexBarProviderConfig *config, const char *key) {
    return string_member(config ? config->raw : NULL, key);
}

static char *cookie_credential(const CodexBarProviderConfig *config, const char *environment_key) {
    char *raw = config_string(config, "cookieHeader");
    char *cookie = clean_header(raw);
    g_free(raw);
    if (!cookie) cookie = clean_header(g_getenv(environment_key));
    return cookie;
}

static gboolean same_origin(const CodexBarHttpResponse *response, const char *url) {
    if (!response || !response->effective_url) return TRUE;
    GUri *actual = g_uri_parse(response->effective_url, G_URI_FLAGS_NONE, NULL);
    GUri *expected = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    const char *actual_scheme = actual ? g_uri_get_scheme(actual) : NULL;
    const char *actual_host = actual ? g_uri_get_host(actual) : NULL;
    const char *expected_host = expected ? g_uri_get_host(expected) : NULL;
    gboolean valid = actual_scheme && actual_host && expected_host &&
                     g_ascii_strcasecmp(actual_scheme, "https") == 0 &&
                     g_ascii_strcasecmp(actual_host, expected_host) == 0 &&
                     g_uri_get_port(actual) == g_uri_get_port(expected);
    if (actual) g_uri_unref(actual);
    if (expected) g_uri_unref(expected);
    return valid;
}

static CodexBarHttpResponse *send_request(const CodexBarHttpRequest *request,
                                          CodexBarWebProviders5Transport transport,
                                          GError **error) {
    if (!transport) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Provider transport is missing");
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
                            "Provider redirected outside its trusted origin");
        return NULL;
    }
    if (response->status >= 200 && response->status < 300) return response;
    long status = response->status;
    codexbar_http_response_free(response);
    g_set_error(error,
                G_IO_ERROR,
                status == 401 || status == 403 || (status >= 300 && status < 400)
                    ? G_IO_ERROR_PERMISSION_DENIED
                    : G_IO_ERROR_FAILED,
                "Provider returned HTTP %ld",
                status);
    return NULL;
}

static CodexBarProvider *new_provider(const char *id, gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup(id);
    provider->source = g_strdup("web");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));
    return provider;
}

static void add_window(CodexBarProvider *provider,
                       const char *title,
                       double used,
                       double limit,
                       gint64 reset_ms,
                       gboolean has_reset) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", title);
    window->usage_known = TRUE;
    window->used_percent = limit > 0 ? CLAMP(used / limit * 100, 0, 100) : 0;
    window->has_resets_at = has_reset;
    window->resets_at_ms = reset_ms;
    window->detail = g_strdup_printf("%.0f / %.0f credits used", used, limit);
    codexbar_provider_add_quota_window(provider, window);
}

static json_object *expand_tree(json_object *value, guint depth) {
    if (!value || depth > 8) return value ? json_object_get(value) : NULL;
    if (json_object_is_type(value, json_type_string)) {
        const char *text = json_object_get_string(value);
        json_object *nested = parse_json(text, strlen(text));
        if (!nested) return json_object_get(value);
        json_object *expanded = expand_tree(nested, depth + 1);
        json_object_put(nested);
        return expanded;
    }
    if (json_object_is_type(value, json_type_array)) {
        json_object *result = json_object_new_array();
        size_t count = json_object_array_length(value);
        for (size_t index = 0; index < count; index++) {
            json_object_array_add(result, expand_tree(json_object_array_get_idx(value, index), depth + 1));
        }
        return result;
    }
    if (json_object_is_type(value, json_type_object)) {
        json_object *result = json_object_new_object();
        json_object_object_foreach(value, key, child) {
            json_object_object_add(result, key, expand_tree(child, depth + 1));
        }
        return result;
    }
    return json_object_get(value);
}

static json_object *find_key(json_object *value, const char *const *keys, guint depth) {
    if (!value || depth > 12) return NULL;
    if (json_object_is_type(value, json_type_object)) {
        {
            json_object_object_foreach(value, key, child) {
                for (size_t index = 0; keys[index]; index++) {
                    if (g_ascii_strcasecmp(key, keys[index]) == 0) return child;
                }
            }
        }
        {
            json_object_object_foreach(value, key, child) {
                (void)key;
                json_object *found = find_key(child, keys, depth + 1);
                if (found) return found;
            }
        }
    } else if (json_object_is_type(value, json_type_array)) {
        size_t count = json_object_array_length(value);
        for (size_t index = 0; index < count; index++) {
            json_object *found = find_key(json_object_array_get_idx(value, index), keys, depth + 1);
            if (found) return found;
        }
    }
    return NULL;
}

static gboolean find_number(json_object *root, const char *const *keys, double *value) {
    return number(find_key(root, keys, 0), value);
}

static char *find_string(json_object *root, const char *const *keys) {
    return string_value(find_key(root, keys, 0));
}

static const char *const total_keys[] = {
    "totalQuota", "totalCredits", "quota", "creditLimit", "monthlyTotalQuota",
    "totalValue", "TotalValue", NULL};
static const char *const remaining_keys[] = {
    "remainingQuota", "remainQuota", "remainingCredits", "availableCredits", "balance",
    "remaining", "totalSurplusValue", "TotalSurplusValue", "surplusValue", NULL};
static const char *const used_keys[] = {
    "usedQuota", "usedCredits", "consumedCredits", "usage", "used", "usedAmount",
    "consumeAmount", "usedValue", "UsedValue", "consumedValue", NULL};
static const char *const count_keys[] = {
    "totalCount", "TotalCount", "subscriptionTotalNumber", "SubscriptionTotalNumber", NULL};
static const char *const reset_keys[] = {
    "nextRefreshTime", "resetTime", "periodEndTime", "billingCycleEnd", "expireTime",
    "expirationTime", "endTime", "validEndTime", "nearestExpireDate", "NearestExpireDate", NULL};
static const char *const plan_keys[] = {
    "planName", "packageName", "commodityName", "instanceName", "displayName", "ProductName",
    "productName", "name", "title", "planType", NULL};

CodexBarProvider *codexbar_alibaba_token_plan_parse(const char *json,
                                                    size_t length,
                                                    gint64 now_ms,
                                                    GError **error) {
    json_object *parsed = parse_json(json, length);
    if (!parsed) {
        if (json && g_strstr_len(json, (gssize)length, "<html") &&
            (g_strstr_len(json, (gssize)length, "login") || g_strstr_len(json, (gssize)length, "Login"))) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                "Alibaba Token Plan login required");
        } else {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Alibaba Token Plan response is invalid");
        }
        return NULL;
    }
    json_object *root = expand_tree(parsed, 0);
    json_object_put(parsed);
    const char *const status_keys[] = {"statusCode", NULL};
    const char *const code_keys[] = {"code", "Code", NULL};
    const char *const message_keys[] = {"message", "Message", NULL};
    double status = 0;
    char *code = find_string(root, code_keys);
    char *message = find_string(root, message_keys);
    const char *const success_keys[] = {"success", "Success", NULL};
    json_object *success = find_key(root, success_keys, 0);
    gboolean forbidden = find_number(root, status_keys, &status) && status == 403;
    gboolean login = code && (strstr(code, "NeedLogin") || strstr(code, "TokenError"));
    if (forbidden || login) {
        g_free(message);
        g_free(code);
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Alibaba Token Plan login required");
        return NULL;
    }
    if (success && json_object_is_type(success, json_type_boolean) && !json_object_get_boolean(success)) {
        g_free(message);
        g_free(code);
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                            "Alibaba Token Plan request was unsuccessful");
        return NULL;
    }
    double total = 0, remaining = 0, used = 0, count = 0;
    gboolean has_total = find_number(root, total_keys, &total);
    gboolean has_remaining = find_number(root, remaining_keys, &remaining);
    gboolean has_used = find_number(root, used_keys, &used);
    gboolean has_count = find_number(root, count_keys, &count);
    if (!has_used && has_total && has_remaining) used = MAX(0, total - remaining), has_used = TRUE;
    char *plan = find_string(root, plan_keys);
    if (!plan && ((has_count && count > 0) || has_total)) plan = g_strdup("TOKEN PLAN");
    if ((!plan && !has_count) || (has_total && total < 0) || (has_used && used < 0) ||
        (has_remaining && remaining < 0)) {
        g_free(plan);
        g_free(message);
        g_free(code);
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Alibaba Token Plan payload is invalid");
        return NULL;
    }
    CodexBarProvider *provider = new_provider("alibabatokenplan", now_ms);
    provider->plan = plan;
    gint64 reset_ms = 0;
    gboolean has_reset = timestamp(find_key(root, reset_keys, 0), &reset_ms);
    if (has_total && has_used && total > 0) add_window(provider, "Token plan", used, total, reset_ms, has_reset);
    if (has_remaining) {
        CodexBarBalance *balance = codexbar_balance_new("credits", "Remaining credits", remaining, "credits");
        balance->has_limit = has_total;
        balance->limit = total;
        balance->has_used = has_used;
        balance->used = used;
        codexbar_provider_add_balance(provider, balance);
    }
    g_free(message);
    g_free(code);
    json_object_put(root);
    return provider;
}

static char *cookie_value(const char *cookie, const char *name) {
    char **parts = g_strsplit(cookie, ";", -1);
    char *result = NULL;
    for (size_t index = 0; parts[index] && !result; index++) {
        char *part = g_strstrip(parts[index]);
        char *separator = strchr(part, '=');
        if (!separator) continue;
        *separator = '\0';
        if (g_str_equal(g_strstrip(part), name)) result = g_strdup(g_strstrip(separator + 1));
    }
    g_strfreev(parts);
    return result;
}

static char *https_override(const char *raw) {
    if (!raw || !raw[0]) return NULL;
    char *candidate = strstr(raw, "://") ? g_strdup(raw) : g_strdup_printf("https://%s", raw);
    GUri *uri = g_uri_parse(candidate, G_URI_FLAGS_NONE, NULL);
    gboolean valid = uri && g_uri_get_scheme(uri) && g_uri_get_host(uri) &&
                     g_ascii_strcasecmp(g_uri_get_scheme(uri), "https") == 0 &&
                     !g_uri_get_userinfo(uri);
    if (uri) g_uri_unref(uri);
    if (valid) return candidate;
    g_free(candidate);
    return NULL;
}

static CodexBarHttpResponse *alibaba_request(const char *url,
                                             const char *method,
                                             const char *cookie,
                                             const char *origin,
                                             const char *referer,
                                             const char *body,
                                             const char *csrf,
                                             long timeout,
                                             CodexBarWebProviders5Transport transport,
                                             GCancellable *cancellable,
                                             GError **error) {
    CodexBarHttpRequestHeader headers[] = {
        {"Cookie", cookie}, {"Accept", "*/*"}, {"Accept-Language", "en-US,en;q=0.9"},
        {"User-Agent", WEB5_USER_AGENT}, {"Origin", origin}, {"Referer", referer},
        {"X-Requested-With", "XMLHttpRequest"},
        {"Content-Type", body ? "application/x-www-form-urlencoded" : NULL},
        {"x-xsrf-token", csrf}, {"x-csrf-token", csrf},
    };
    size_t header_count = body ? G_N_ELEMENTS(headers) : 7;
    CodexBarHttpRequest request = {
        .url = url, .method = method, .headers = headers, .header_count = header_count,
        .body = body, .body_length = body ? strlen(body) : 0,
        .timeout_seconds = timeout, .maximum_response_bytes = WEB5_RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    return send_request(&request, transport, error);
}

CodexBarProvider *codexbar_alibaba_token_plan_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders5Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *cookie = cookie_credential(config, "ALIBABA_TOKEN_PLAN_COOKIE");
    if (!cookie) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "Alibaba Token Plan cookie is missing");
        return NULL;
    }
    gboolean china = config && config->region && g_ascii_strcasecmp(config->region, "cn") == 0;
    const char *default_origin = china ? "https://bailian.console.aliyun.com"
                                       : "https://modelstudio.console.alibabacloud.com";
    const char *raw_host_override = g_getenv("ALIBABA_TOKEN_PLAN_HOST");
    char *host_override = https_override(raw_host_override);
    if (raw_host_override && raw_host_override[0] && !host_override) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Alibaba Token Plan host override must use HTTPS");
        g_free(cookie);
        return NULL;
    }
    const char *base = host_override ? host_override : default_origin;
    char *referer = g_strdup_printf(
        china ? "%s/cn-beijing?tab=plan#/efm/subscription/token-plan"
              : "%s/ap-southeast-1/?tab=plan#/efm/subscription/token-plan",
        base);
    char *sec_token = cookie_value(cookie, "sec_token");
    char *csrf = cookie_value(cookie, "login_aliyunid_csrf");
    if (!csrf) csrf = cookie_value(cookie, "csrf");
    const char *region = china ? "cn-beijing" : "ap-southeast-1";
    char *escaped_params = g_uri_escape_string(
        china ? "{\"ProductCode\":\"sfm_tokenplanteams_dp_cn\"}"
              : "{\"ProductCode\":\"sfm_tokenplanteams_dp_intl\"}",
        NULL, TRUE);
    char *escaped_sec = sec_token ? g_uri_escape_string(sec_token, NULL, TRUE) : NULL;
    char *body = g_strdup_printf("product=BssOpenAPI-V3&action=GetSubscriptionSummary&params=%s&region=%s%s%s",
                                 escaped_params, region, escaped_sec ? "&sec_token=" : "",
                                 escaped_sec ? escaped_sec : "");
    const char *raw_override = g_getenv("ALIBABA_TOKEN_PLAN_QUOTA_URL");
    char *override = https_override(raw_override);
    if (raw_override && raw_override[0] && !override) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Alibaba Token Plan endpoint override must use HTTPS");
        g_free(body);
        g_free(escaped_sec);
        g_free(escaped_params);
        g_free(csrf);
        g_free(sec_token);
        g_free(referer);
        g_free(host_override);
        g_free(cookie);
        return NULL;
    }
    char *url = override
        ? override
        : g_strdup_printf("%s/data/api.json?action=GetSubscriptionSummary&product=BssOpenAPI-V3&_tag=", base);
    CodexBarHttpResponse *response = alibaba_request(
        url, "POST", cookie, default_origin, referer, body, csrf, 20, transport, cancellable, error);
    CodexBarProvider *provider = response
        ? codexbar_alibaba_token_plan_parse(response->body, response->body_length, now_ms, error)
        : NULL;
    codexbar_http_response_free(response);
    g_free(url);
    g_free(body);
    g_free(escaped_sec);
    g_free(escaped_params);
    g_free(csrf);
    g_free(sec_token);
    g_free(referer);
    g_free(host_override);
    g_free(cookie);
    return provider;
}

CodexBarProvider *codexbar_alibaba_token_plan_fetch_with_cancellable(
    const CodexBarProviderConfig *config,
    GCancellable *cancellable,
    GError **error) {
    return codexbar_alibaba_token_plan_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

static char *mimo_cookie(const CodexBarProviderConfig *config) {
    char *raw = cookie_credential(config, "MIMO_COOKIE");
    if (!raw) return NULL;
    const char *names[] = {
        "api-platform_ph", "api-platform_serviceToken", "api-platform_slh", "userId"};
    GString *normalized = g_string_new(NULL);
    gboolean has_token = FALSE, has_user = FALSE;
    for (size_t index = 0; index < G_N_ELEMENTS(names); index++) {
        char *value = cookie_value(raw, names[index]);
        if (!value) continue;
        if (normalized->len) g_string_append(normalized, "; ");
        g_string_append_printf(normalized, "%s=%s", names[index], value);
        if (index == 1) has_token = TRUE;
        if (index == 3) has_user = TRUE;
        g_free(value);
    }
    g_free(raw);
    if (has_token && has_user) return g_string_free(normalized, FALSE);
    g_string_free(normalized, TRUE);
    return NULL;
}

static CodexBarHttpResponse *mimo_request(const char *url,
                                          const char *cookie,
                                          CodexBarWebProviders5Transport transport,
                                          GCancellable *cancellable,
                                          GError **error) {
    CodexBarHttpRequestHeader headers[] = {
        {"Cookie", cookie}, {"Accept", "application/json, text/plain, */*"},
        {"Accept-Language", "en-US,en;q=0.9"}, {"x-timeZone", "UTC+01:00"},
        {"Origin", "https://platform.xiaomimimo.com"},
        {"Referer", "https://platform.xiaomimimo.com/#/console/balance"},
        {"User-Agent", WEB5_USER_AGENT},
    };
    CodexBarHttpRequest request = {
        .url = url, .method = "GET", .headers = headers, .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 15, .maximum_response_bytes = WEB5_RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    return send_request(&request, transport, error);
}

static json_object *mimo_payload(const char *json, size_t length) {
    json_object *root = parse_json(json, length);
    double code = -1;
    if (!root || !number(member(root, "code"), &code) || code != 0) {
        if (root) json_object_put(root);
        return NULL;
    }
    return root;
}

CodexBarProvider *codexbar_mimo_parse(const char *balance_json,
                                      size_t balance_length,
                                      const char *detail_json,
                                      size_t detail_length,
                                      const char *usage_json,
                                      size_t usage_length,
                                      gint64 now_ms,
                                      GError **error) {
    json_object *balance_root = parse_json(balance_json, balance_length);
    double response_code = -1;
    if (balance_root) number(member(balance_root, "code"), &response_code);
    if (response_code == 401 || response_code == 403) {
        json_object_put(balance_root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "MiMo browser session is invalid");
        return NULL;
    }
    json_object *balance_data = balance_root ? member(balance_root, "data") : NULL;
    double balance = 0;
    char *currency = string_member(balance_data, "currency");
    if (response_code != 0 || !balance_data || !number(member(balance_data, "balance"), &balance) ||
        balance < 0 || !currency) {
        g_free(currency);
        if (balance_root) json_object_put(balance_root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "MiMo balance is invalid");
        return NULL;
    }
    CodexBarProvider *provider = new_provider("mimo", now_ms);
    codexbar_provider_add_balance(provider, codexbar_balance_new("balance", "Balance", balance, currency));
    const char *component_keys[] = {"cashBalance", "giftBalance"};
    const char *component_ids[] = {"cash", "gift"};
    const char *component_titles[] = {"Paid balance", "Granted balance"};
    for (size_t index = 0; index < G_N_ELEMENTS(component_keys); index++) {
        double value = 0;
        if (number(member(balance_data, component_keys[index]), &value) && value >= 0) {
            codexbar_provider_add_balance(
                provider, codexbar_balance_new(component_ids[index], component_titles[index], value, currency));
        }
    }
    json_object *detail_root = detail_json ? mimo_payload(detail_json, detail_length) : NULL;
    json_object *detail = detail_root ? member(detail_root, "data") : NULL;
    provider->plan = string_member(detail, "planCode");
    gint64 reset_ms = 0;
    gboolean has_reset = timestamp(member(detail, "currentPeriodEnd"), &reset_ms);
    json_object *usage_root = usage_json ? mimo_payload(usage_json, usage_length) : NULL;
    json_object *usage_data = usage_root ? member(usage_root, "data") : NULL;
    json_object *month = usage_data ? member(usage_data, "monthUsage") : NULL;
    json_object *items = month ? member(month, "items") : NULL;
    if (items && json_object_is_type(items, json_type_array) && json_object_array_length(items) > 0) {
        json_object *item = json_object_array_get_idx(items, 0);
        double used = 0, limit = 0, fraction = 0;
        if (number(member(item, "used"), &used) && number(member(item, "limit"), &limit) &&
            number(member(item, "percent"), &fraction) && used >= 0 && limit > 0 && fraction >= 0) {
            add_window(provider, "Monthly token plan", used, limit, reset_ms, has_reset);
            codexbar_provider_quota_window(provider, 0)->used_percent = CLAMP(fraction * 100, 0, 100);
        }
    }
    if (usage_root) json_object_put(usage_root);
    if (detail_root) json_object_put(detail_root);
    json_object_put(balance_root);
    g_free(currency);
    return provider;
}

static gint64 nonnegative_integer(json_object *object, const char *key) {
    double value = 0;
    if (!number(member(object, key), &value) || value < 0 || value > (double)G_MAXINT64) return 0;
    return (gint64)value;
}

static gint64 local_window_total(json_object *window) {
    const char *keys[] = {"input", "output", "cache_read", "cache_create"};
    gint64 total = 0;
    for (size_t index = 0; index < G_N_ELEMENTS(keys); index++) {
        gint64 value = nonnegative_integer(window, keys[index]);
        if (G_MAXINT64 - total < value) return G_MAXINT64;
        total += value;
    }
    return total;
}

static char *compact_tokens(gint64 value) {
    if (value >= 1000000) return g_strdup_printf("%.1fM", (double)value / 1000000);
    if (value >= 1000) return g_strdup_printf("%.1fk", (double)value / 1000);
    return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

CodexBarProvider *codexbar_mimo_local_parse(const char *json,
                                            size_t length,
                                            gint64 now_ms,
                                            GError **error) {
    json_object *root = parse_json(json, length);
    json_object *windows = root ? member(root, "windows") : NULL;
    json_object *today = member(windows, "today");
    json_object *week = member(windows, "week");
    json_object *all_time = member(windows, "all_time");
    if (!root || !json_object_is_type(windows, json_type_object) ||
        !json_object_is_type(today, json_type_object) ||
        !json_object_is_type(week, json_type_object) ||
        !json_object_is_type(all_time, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "MiMo local cache is invalid");
        return NULL;
    }
    gint64 today_total = local_window_total(today);
    gint64 week_total = local_window_total(week);
    gint64 all_total = local_window_total(all_time);
    gint64 sessions = nonnegative_integer(root, "sessions_scanned");
    GPtrArray *parts = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(parts, g_strdup("Local"));
    if (today_total > 0) {
        char *count = compact_tokens(today_total);
        g_ptr_array_add(parts, g_strdup_printf("%s today", count));
        g_free(count);
    }
    if (week_total > 0) {
        char *count = compact_tokens(week_total);
        g_ptr_array_add(parts, g_strdup_printf("%s week", count));
        g_free(count);
    }
    if (all_total > 0) {
        char *count = compact_tokens(all_total);
        g_ptr_array_add(parts, g_strdup_printf("%s total", count));
        g_free(count);
    }
    g_ptr_array_add(parts, g_strdup_printf("%" G_GINT64_FORMAT " sessions", sessions));
    gint64 updated_ms = now_ms;
    timestamp(member(root, "updated_at"), &updated_ms);
    gint64 age_ms = now_ms - updated_ms;
    if (age_ms > G_GINT64_CONSTANT(12) * 60 * 60 * 1000) {
        gint64 age_hours = MAX(1, age_ms / (60 * 60 * 1000));
        if (age_hours >= 24) {
            g_ptr_array_add(parts, g_strdup_printf("stale %" G_GINT64_FORMAT "d", age_hours / 24));
        } else {
            g_ptr_array_add(parts, g_strdup_printf("stale %" G_GINT64_FORMAT "h", age_hours));
        }
    }
    g_ptr_array_add(parts, NULL);
    char *summary = g_strjoinv(" · ", (char **)parts->pdata);
    CodexBarProvider *provider = new_provider("mimo", updated_ms);
    g_free(provider->source);
    provider->source = g_strdup("local");
    provider->plan = summary;
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("estimated"));
    json_object_object_add(provider->usage_extensions, "localTodayTokens", json_object_new_int64(today_total));
    json_object_object_add(provider->usage_extensions, "localWeekTokens", json_object_new_int64(week_total));
    json_object_object_add(provider->usage_extensions, "localAllTimeTokens", json_object_new_int64(all_total));
    json_object_object_add(provider->usage_extensions, "sessionsScanned", json_object_new_int64(sessions));
    g_ptr_array_free(parts, TRUE);
    json_object_put(root);
    return provider;
}

static char *mimo_local_cache_path(void) {
    const char *override = g_getenv("MIMO_LOCAL_USAGE_PATH");
    if (override && override[0]) {
        if (g_str_has_prefix(override, "~/")) {
            return g_build_filename(g_get_home_dir(), override + 2, NULL);
        }
        return g_strdup(override);
    }
    return g_build_filename(g_get_home_dir(), ".codexbar", "mimo-local-usage.json", NULL);
}

static CodexBarProvider *mimo_local_fetch(gint64 now_ms, GError **error) {
    char *path = mimo_local_cache_path();
    char *contents = NULL;
    gsize length = 0;
    GError *read_error = NULL;
    if (!g_file_get_contents(path, &contents, &length, &read_error) || length > WEB5_RESPONSE_LIMIT) {
        g_clear_error(&read_error);
        g_free(contents);
        g_free(path);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "MiMo local usage cache is unavailable");
        return NULL;
    }
    CodexBarProvider *provider = codexbar_mimo_local_parse(contents, length, now_ms, error);
    g_free(contents);
    g_free(path);
    return provider;
}

CodexBarProvider *codexbar_mimo_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders5Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *cookie = mimo_cookie(config);
    if (!cookie) {
        if (!config || !config->source || g_str_equal(config->source, "auto")) {
            return mimo_local_fetch(now_ms, error);
        }
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "MiMo requires api-platform_serviceToken and userId cookies");
        return NULL;
    }
    const char *raw_base = g_getenv("MIMO_API_URL");
    char *base = https_override(raw_base);
    if (raw_base && raw_base[0] && !base) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "MiMo endpoint override must use HTTPS");
        g_free(cookie);
        return NULL;
    }
    if (!base) base = g_strdup("https://platform.xiaomimimo.com/api/v1");
    while (base[strlen(base) - 1] == '/') base[strlen(base) - 1] = '\0';
    char *balance_url = g_strdup_printf("%s/balance", base);
    char *detail_url = g_strdup_printf("%s/tokenPlan/detail", base);
    char *usage_url = g_strdup_printf("%s/tokenPlan/usage", base);
    CodexBarHttpResponse *balance = mimo_request(balance_url, cookie, transport, cancellable, error);
    gboolean automatic = !config || !config->source || g_str_equal(config->source, "auto");
    if (!balance && automatic && error && *error &&
        g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
        g_clear_error(error);
        CodexBarProvider *local = mimo_local_fetch(now_ms, error);
        g_free(usage_url);
        g_free(detail_url);
        g_free(balance_url);
        g_free(base);
        g_free(cookie);
        return local;
    }
    GError *optional_error = NULL;
    CodexBarHttpResponse *detail = balance
        ? mimo_request(detail_url, cookie, transport, cancellable, &optional_error)
        : NULL;
    g_clear_error(&optional_error);
    CodexBarHttpResponse *usage = balance
        ? mimo_request(usage_url, cookie, transport, cancellable, &optional_error)
        : NULL;
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        codexbar_http_response_free(usage);
        codexbar_http_response_free(detail);
        codexbar_http_response_free(balance);
        g_clear_error(&optional_error);
        if (!error || !*error) g_cancellable_set_error_if_cancelled(cancellable, error);
        g_free(usage_url);
        g_free(detail_url);
        g_free(balance_url);
        g_free(base);
        g_free(cookie);
        return NULL;
    }
    g_clear_error(&optional_error);
    CodexBarProvider *provider = balance
        ? codexbar_mimo_parse(balance->body, balance->body_length,
                             detail ? detail->body : NULL, detail ? detail->body_length : 0,
                             usage ? usage->body : NULL, usage ? usage->body_length : 0,
                             now_ms, error)
        : NULL;
    codexbar_http_response_free(usage);
    codexbar_http_response_free(detail);
    codexbar_http_response_free(balance);
    g_free(usage_url);
    g_free(detail_url);
    g_free(balance_url);
    g_free(base);
    g_free(cookie);
    return provider;
}

CodexBarProvider *codexbar_mimo_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                       GCancellable *cancellable,
                                                       GError **error) {
    return codexbar_mimo_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}
