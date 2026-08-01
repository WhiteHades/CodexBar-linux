#include "api_providers3.h"

#include <errno.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define PROVIDER_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)
#define PROVIDER_TIMEOUT_SECONDS 15
#define MAX_CREDENTIAL_BYTES 16384U

#define MINIMAX_GLOBAL_TOKEN_URL "https://api.minimax.io/v1/token_plan/remains"
#define MINIMAX_GLOBAL_LEGACY_URL "https://api.minimax.io/v1/api/openplatform/coding_plan/remains"
#define MINIMAX_CHINA_TOKEN_URL "https://api.minimaxi.com/v1/token_plan/remains"
#define MINIMAX_CHINA_LEGACY_URL "https://api.minimaxi.com/v1/api/openplatform/coding_plan/remains"

#define ALIBABA_INTL_URL                                                                                         \
    "https://modelstudio.console.alibabacloud.com/data/api.json?"                                               \
    "action=zeldaEasy.broadscope-bailian.codingPlan.queryCodingPlanInstanceInfoV2&"                            \
    "product=broadscope-bailian&api=queryCodingPlanInstanceInfoV2&currentRegionId=ap-southeast-1"
#define ALIBABA_CHINA_URL                                                                                        \
    "https://bailian.console.aliyun.com/data/api.json?"                                                        \
    "action=zeldaEasy.broadscope-bailian.codingPlan.queryCodingPlanInstanceInfoV2&"                            \
    "product=broadscope-bailian&api=queryCodingPlanInstanceInfoV2&currentRegionId=cn-beijing"

#define DOUBAO_ARK_URL "https://ark.cn-beijing.volces.com/api/coding/v3/chat/completions"
#define DOUBAO_CODING_PLAN_URL "https://open.volcengineapi.com/?Action=GetCodingPlanUsage&Version=2024-01-01"
#define DOUBAO_AGENT_PLAN_URL "https://open.volcengineapi.com/?Action=GetAFPUsage&Version=2024-01-01"

static gboolean json_whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

static json_object *parse_json_document(const char *json, size_t length) {
    if (!json || length == 0 || length > G_MAXINT || !g_utf8_validate(json, (gssize)length, NULL) ||
        memchr(json, '\0', length)) {
        return NULL;
    }
    json_tokener *tokener = json_tokener_new();
    if (!tokener) return NULL;
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, json, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && json_whitespace(json[consumed])) consumed++;
    gboolean valid = parse_error == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static void strip_unicode_whitespace(char *value) {
    char *start = value;
    while (*start && g_unichar_isspace(g_utf8_get_char(start))) start = g_utf8_next_char(start);
    char *end = value + strlen(value);
    while (end > start) {
        char *previous = g_utf8_find_prev_char(start, end);
        if (!previous || !g_unichar_isspace(g_utf8_get_char(previous))) break;
        end = previous;
    }
    size_t length = (size_t)(end - start);
    memmove(value, start, length);
    value[length] = '\0';
}

static char *clean_credential(const char *raw) {
    if (!raw || !g_utf8_validate(raw, -1, NULL) || strlen(raw) > MAX_CREDENTIAL_BYTES) return NULL;
    char *value = g_strdup(raw);
    strip_unicode_whitespace(value);
    size_t length = strlen(value);
    if (length >= 2 && ((value[0] == '\'' && value[length - 1] == '\'') ||
                        (value[0] == '"' && value[length - 1] == '"'))) {
        value[length - 1] = '\0';
        memmove(value, value + 1, length - 1);
        strip_unicode_whitespace(value);
    }
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (*cursor < 33 || *cursor == 127) {
            g_free(value);
            return NULL;
        }
    }
    if (value[0] != '\0') return value;
    g_free(value);
    return NULL;
}

static char *resolve_first(const char *configured, const char *const *environment_keys) {
    char *value = clean_credential(configured);
    for (size_t index = 0; !value && environment_keys[index]; index++) {
        value = clean_credential(g_getenv(environment_keys[index]));
    }
    return value;
}

static gboolean has_value(const char *configured, const char *const *environment_keys) {
    char *value = resolve_first(configured, environment_keys);
    gboolean present = value != NULL;
    g_free(value);
    return present;
}

static gboolean check_cancelled(GCancellable *cancellable, GError **error) {
    return cancellable && g_cancellable_set_error_if_cancelled(cancellable, error);
}

static CodexBarHttpResponse *send_request(const CodexBarHttpRequest *request,
                                          CodexBarApiProviders3Transport transport,
                                          GError **error) {
    if (check_cancelled(request->cancellable, error)) return NULL;
    CodexBarHttpResponse *response = transport(request, error);
    if (request->cancellable && g_cancellable_is_cancelled(request->cancellable)) {
        codexbar_http_response_free(response);
        if (error && *error) g_clear_error(error);
        g_cancellable_set_error_if_cancelled(request->cancellable, error);
        return NULL;
    }
    if (!response && error && !*error) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Provider network request failed");
    }
    return response;
}

static gboolean json_double(json_object *value, double *result) {
    if (!value || json_object_is_type(value, json_type_null) || json_object_is_type(value, json_type_boolean)) {
        return FALSE;
    }
    double number = 0;
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        number = json_object_get_double(value);
    } else if (json_object_is_type(value, json_type_string)) {
        const char *text = json_object_get_string(value);
        size_t length = (size_t)json_object_get_string_len(value);
        if (!text || memchr(text, '\0', length)) return FALSE;
        char *copy = g_strndup(text, length);
        g_strstrip(copy);
        char *end = NULL;
        number = g_ascii_strtod(copy, &end);
        gboolean valid = copy[0] != '\0' && end && *end == '\0' && isfinite(number);
        g_free(copy);
        if (!valid) return FALSE;
    } else {
        return FALSE;
    }
    if (!isfinite(number)) return FALSE;
    *result = number;
    return TRUE;
}

static gboolean object_double(json_object *object, const char *key, double *result) {
    json_object *value = NULL;
    return object && json_object_object_get_ex(object, key, &value) && json_double(value, result);
}

static gboolean object_first_double(json_object *object, const char *const *keys, double *result) {
    for (size_t index = 0; keys[index]; index++) {
        if (object_double(object, keys[index], result)) return TRUE;
    }
    return FALSE;
}

static char *object_string(json_object *object, const char *key) {
    json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!text || memchr(text, '\0', length) || !g_utf8_validate(text, (gssize)length, NULL)) return NULL;
    char *copy = g_strndup(text, length);
    strip_unicode_whitespace(copy);
    if (copy[0] != '\0') return copy;
    g_free(copy);
    return NULL;
}

static char *object_first_string(json_object *object, const char *const *keys) {
    for (size_t index = 0; keys[index]; index++) {
        char *value = object_string(object, keys[index]);
        if (value) return value;
    }
    return NULL;
}

static gboolean epoch_value_ms(json_object *value, gint64 *result) {
    double number = 0;
    if (!json_double(value, &number) || number <= 0) return FALSE;
    if (number > 1000000000000.0) {
        if (number > (double)G_MAXINT64) return FALSE;
        *result = (gint64)llround(number);
        return TRUE;
    }
    if (number > 1000000000.0 && number <= (double)G_MAXINT64 / 1000.0) {
        *result = (gint64)llround(number * 1000.0);
        return TRUE;
    }
    return FALSE;
}

static gboolean object_epoch_ms(json_object *object, const char *key, gint64 *result) {
    json_object *value = NULL;
    return object && json_object_object_get_ex(object, key, &value) && epoch_value_ms(value, result);
}

static CodexBarProvider *provider_new(const char *id, gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup(id);
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    return provider;
}

static CodexBarQuotaWindow *add_window(CodexBarProvider *provider,
                                       const char *id,
                                       const char *title,
                                       double used_percent,
                                       gint64 window_minutes,
                                       gint64 resets_at_ms,
                                       const char *detail) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = CLAMP(used_percent, 0.0, 100.0);
    if (window_minutes > 0) {
        window->has_window_minutes = TRUE;
        window->window_minutes = window_minutes;
    }
    if (resets_at_ms > 0) {
        window->has_resets_at = TRUE;
        window->resets_at_ms = resets_at_ms;
    }
    window->detail = g_strdup(detail);
    codexbar_provider_add_quota_window(provider, window);
    return window;
}

static void add_identity(CodexBarProvider *provider, const char *plan) {
    if (!plan) return;
    provider->plan = g_strdup(plan);
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = g_strdup(plan);
}

static const char *const minimax_environment_keys[] = {
    "MINIMAX_CODING_API_KEY",
    "MINIMAX_API_KEY",
    NULL,
};

gboolean codexbar_minimax_has_api_key(const CodexBarProviderConfig *config) {
    return has_value(config ? config->api_key : NULL, minimax_environment_keys);
}

static const char *minimax_service_title(const char *model) {
    if (!model) return "MiniMax";
    if (g_ascii_strcasecmp(model, "general") == 0) return "General";
    if (g_ascii_strcasecmp(model, "video") == 0) return "Video";
    return model;
}

static gboolean minimax_add_quota(CodexBarProvider *provider,
                                  json_object *item,
                                  const char *model,
                                  gboolean weekly,
                                  guint index,
                                  gint64 now_ms) {
    const char *total_key = weekly ? "current_weekly_total_count" : "current_interval_total_count";
    const char *remaining_key = weekly ? "current_weekly_usage_count" : "current_interval_usage_count";
    const char *percent_key = weekly ? "current_weekly_remaining_percent" : "current_interval_remaining_percent";
    const char *status_key = weekly ? "current_weekly_status" : "current_interval_status";
    const char *start_key = weekly ? "weekly_start_time" : "start_time";
    const char *end_key = weekly ? "weekly_end_time" : "end_time";
    const char *remains_key = weekly ? "weekly_remains_time" : "remains_time";
    double total = 0;
    double remaining = 0;
    double remaining_percent = 0;
    double status = 0;
    gboolean has_total = object_double(item, total_key, &total);
    gboolean has_remaining = object_double(item, remaining_key, &remaining);
    gboolean has_percent = object_double(item, percent_key, &remaining_percent);
    gboolean has_status = object_double(item, status_key, &status);
    if (has_status && status == 3 && (!has_total || total == 0) && has_percent && remaining_percent >= 100) {
        return FALSE;
    }
    if (!has_percent && (!has_total || total <= 0 || !has_remaining)) return FALSE;

    double used_percent = has_percent ? 100.0 - remaining_percent : (total - remaining) / total * 100.0;
    gint64 start_ms = 0;
    gint64 end_ms = 0;
    object_epoch_ms(item, start_key, &start_ms);
    object_epoch_ms(item, end_key, &end_ms);
    gint64 window_minutes = weekly ? 7 * 24 * 60 : 0;
    if (start_ms > 0 && end_ms > start_ms) window_minutes = (end_ms - start_ms) / 60000;
    gint64 resets_at_ms = end_ms > now_ms ? end_ms : 0;
    if (resets_at_ms == 0) {
        double remains = 0;
        if (object_double(item, remains_key, &remains) && remains > 0) {
            double seconds = remains > 1000000.0 ? remains / 1000.0 : remains;
            if (seconds <= (double)(G_MAXINT64 - now_ms) / 1000.0) {
                resets_at_ms = now_ms + (gint64)llround(seconds * 1000.0);
            }
        }
    }
    char *id = g_strdup_printf("minimax-%u-%s", index, weekly ? "weekly" : "interval");
    char *title = weekly ? g_strdup_printf("%s weekly", minimax_service_title(model))
                         : g_strdup(minimax_service_title(model));
    char *detail = NULL;
    if (has_total && total > 0 && has_remaining) {
        double used = MAX(0.0, total - remaining);
        detail = g_strdup_printf("%.0f / %.0f used", used, total);
    } else {
        detail = g_strdup_printf("%.1f%% used", CLAMP(used_percent, 0.0, 100.0));
    }
    add_window(provider, id, title, used_percent, window_minutes, resets_at_ms, detail);
    g_free(detail);
    g_free(title);
    g_free(id);
    return TRUE;
}

static CodexBarProvider *minimax_parse_multi_service(json_object *root, gint64 now_ms) {
    json_object *data = NULL;
    json_object *services = NULL;
    if (!json_object_object_get_ex(root, "data", &data) || !json_object_is_type(data, json_type_object) ||
        !json_object_object_get_ex(data, "services", &services) ||
        !json_object_is_type(services, json_type_array) || json_object_array_length(services) == 0) {
        return NULL;
    }
    CodexBarProvider *provider = provider_new("minimax", now_ms);
    size_t count = json_object_array_length(services);
    for (size_t index = 0; index < count; index++) {
        json_object *service = json_object_array_get_idx(services, index);
        if (!service || !json_object_is_type(service, json_type_object)) continue;
        char *service_type = object_string(service, "service_type");
        char *window_type = object_string(service, "window_type");
        double usage = 0;
        double limit = 0;
        double percent = 0;
        if (!service_type || !window_type || !object_double(service, "usage", &usage) ||
            !object_double(service, "limit", &limit) || limit <= 0) {
            g_free(service_type);
            g_free(window_type);
            continue;
        }
        if (!object_double(service, "percent", &percent)) percent = usage / limit * 100.0;
        gint64 minutes = 0;
        char *end = NULL;
        double value = g_ascii_strtod(window_type, &end);
        if (end && end != window_type) {
            while (*end && g_ascii_isspace(*end)) end++;
            if (g_ascii_strncasecmp(end, "hour", 4) == 0) minutes = (gint64)llround(value * 60.0);
            else if (g_ascii_strncasecmp(end, "day", 3) == 0) minutes = (gint64)llround(value * 1440.0);
            else if (g_ascii_strncasecmp(end, "min", 3) == 0) minutes = (gint64)llround(value);
        } else if (g_ascii_strcasecmp(window_type, "today") == 0) {
            minutes = 1440;
        } else if (g_ascii_strcasecmp(window_type, "weekly") == 0) {
            minutes = 10080;
        }
        char *id = g_strdup_printf("minimax-%zu", index);
        char *detail = g_strdup_printf("%.0f / %.0f used", usage, limit);
        add_window(provider, id, service_type, percent, minutes, 0, detail);
        g_free(detail);
        g_free(id);
        g_free(service_type);
        g_free(window_type);
    }
    if (provider->quota_windows->len > 0) return provider;
    codexbar_provider_free(provider);
    return NULL;
}

CodexBarProvider *codexbar_minimax_parse_api_usage(const char *json,
                                                   size_t length,
                                                   gint64 now_ms,
                                                   GError **error) {
    json_object *root = parse_json_document(json, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "MiniMax remains response is malformed");
        return NULL;
    }
    CodexBarProvider *multi = minimax_parse_multi_service(root, now_ms);
    if (multi) {
        json_object_put(root);
        return multi;
    }
    json_object *data = root;
    json_object *nested = NULL;
    if (json_object_object_get_ex(root, "data", &nested) &&
        json_object_is_type(nested, json_type_object)) {
        data = nested;
    }
    json_object *base = NULL;
    if (!json_object_object_get_ex(data, "base_resp", &base)) json_object_object_get_ex(root, "base_resp", &base);
    if (base && json_object_is_type(base, json_type_object)) {
        double status = 0;
        if (object_double(base, "status_code", &status) && status != 0) {
            char *message = object_string(base, "status_msg");
            char *lower = message ? g_ascii_strdown(message, -1) : NULL;
            gboolean credential_error = status == 1004 || (lower && (strstr(lower, "api key") ||
                                                                       strstr(lower, "cookie") ||
                                                                       strstr(lower, "login") ||
                                                                       strstr(lower, "log in")));
            g_set_error(error,
                        G_IO_ERROR,
                        credential_error ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_FAILED,
                        "MiniMax API error: %s",
                        message ? message : "nonzero status");
            g_free(lower);
            g_free(message);
            json_object_put(root);
            return NULL;
        }
    }
    json_object *remains = NULL;
    if (!json_object_object_get_ex(data, "model_remains", &remains) ||
        !json_object_is_type(remains, json_type_array) || json_object_array_length(remains) == 0) {
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "MiniMax remains response has no quota data");
        return NULL;
    }

    CodexBarProvider *provider = provider_new("minimax", now_ms);
    const char *const plan_keys[] = {
        "current_subscribe_title", "plan_name", "combo_title", "current_plan_title", NULL,
    };
    char *plan = object_first_string(data, plan_keys);
    if (!plan) {
        json_object *combo = NULL;
        if (json_object_object_get_ex(data, "current_combo_card", &combo) &&
            json_object_is_type(combo, json_type_object)) {
            plan = object_string(combo, "title");
        }
    }
    add_identity(provider, plan);
    g_free(plan);
    double points = 0;
    const char *const points_keys[] = {
        "points_balance", "point_balance", "credits_balance", "credit_balance", "balance", NULL,
    };
    if (object_first_double(data, points_keys, &points) && points >= 0) {
        codexbar_provider_add_balance(provider, codexbar_balance_new("points", "points", points, "points"));
    }
    size_t count = json_object_array_length(remains);
    for (size_t index = 0; index < count; index++) {
        json_object *item = json_object_array_get_idx(remains, index);
        if (!item || !json_object_is_type(item, json_type_object)) continue;
        char *model = object_string(item, "model_name");
        minimax_add_quota(provider, item, model, FALSE, (guint)index, now_ms);
        char *lower_model = model ? g_ascii_strdown(model, -1) : NULL;
        gboolean has_weekly = lower_model && (g_str_equal(lower_model, "general") ||
                                               strstr(lower_model, "minimax-m") ||
                                               g_str_has_prefix(lower_model, "m2."));
        if (has_weekly) {
            minimax_add_quota(provider, item, model, TRUE, (guint)index, now_ms);
        }
        g_free(lower_model);
        g_free(model);
    }
    json_object_put(root);
    if (provider->quota_windows->len > 0 || provider->balances->len > 0) return provider;
    codexbar_provider_free(provider);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "MiniMax remains response has no usable quota");
    return NULL;
}

static CodexBarProvider *minimax_request_once(const char *url,
                                              const char *key,
                                              CodexBarApiProviders3Transport transport,
                                              GCancellable *cancellable,
                                              gint64 now_ms,
                                              GError **error) {
    char *authorization = g_strdup_printf("Bearer %s", key);
    const CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"MM-API-Source", "CodexBar"},
    };
    CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = PROVIDER_TIMEOUT_SECONDS,
        .maximum_response_bytes = PROVIDER_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    g_free(authorization);
    if (!response) return NULL;
    if (response->status != 200) {
        g_set_error(error,
                    G_IO_ERROR,
                    response->status == 401 || response->status == 403 ? G_IO_ERROR_PERMISSION_DENIED
                                                                       : G_IO_ERROR_FAILED,
                    "MiniMax API returned HTTP %ld",
                    response->status);
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider =
        codexbar_minimax_parse_api_usage(response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

static CodexBarProvider *minimax_try_region(gboolean china,
                                           const char *key,
                                           CodexBarApiProviders3Transport transport,
                                           GCancellable *cancellable,
                                           gint64 now_ms,
                                           GError **error) {
    const char *token_url = china ? MINIMAX_CHINA_TOKEN_URL : MINIMAX_GLOBAL_TOKEN_URL;
    const char *legacy_url = china ? MINIMAX_CHINA_LEGACY_URL : MINIMAX_GLOBAL_LEGACY_URL;
    GError *first_error = NULL;
    CodexBarProvider *provider =
        minimax_request_once(token_url, key, transport, cancellable, now_ms, &first_error);
    if (provider || (first_error && g_error_matches(first_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))) {
        if (error) *error = first_error;
        else g_clear_error(&first_error);
        return provider;
    }
    GError *legacy_error = NULL;
    provider = minimax_request_once(legacy_url, key, transport, cancellable, now_ms, &legacy_error);
    if (provider) {
        g_clear_error(&first_error);
        return provider;
    }
    if (first_error && g_error_matches(first_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
        g_clear_error(&legacy_error);
        if (error) *error = first_error;
        else g_clear_error(&first_error);
    } else {
        g_clear_error(&first_error);
        if (error) *error = legacy_error;
        else g_clear_error(&legacy_error);
    }
    return NULL;
}

CodexBarProvider *codexbar_minimax_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders3Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    char *key = resolve_first(config ? config->api_key : NULL, minimax_environment_keys);
    if (!key) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "MiniMax API key is missing");
        return NULL;
    }
    gboolean china = config && config->region && g_ascii_strcasecmp(config->region, "cn") == 0;
    GError *first_error = NULL;
    CodexBarProvider *provider =
        minimax_try_region(china, key, transport, cancellable, now_ms, &first_error);
    if (!provider && !china && first_error &&
        g_error_matches(first_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
        g_clear_error(&first_error);
        provider = minimax_try_region(TRUE, key, transport, cancellable, now_ms, &first_error);
        if (!provider && first_error && !g_error_matches(first_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_clear_error(&first_error);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "MiniMax API credentials are invalid");
        }
    }
    g_free(key);
    if (provider) {
        g_clear_error(&first_error);
        return provider;
    }
    if (!error || !*error) {
        if (error) *error = first_error;
        else g_clear_error(&first_error);
    } else {
        g_clear_error(&first_error);
    }
    return NULL;
}

CodexBarProvider *codexbar_minimax_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarApiProviders3Transport transport,
                                                        gint64 now_ms,
                                                        GError **error) {
    return codexbar_minimax_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_minimax_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error) {
    return codexbar_minimax_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_minimax_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_minimax_fetch_with_cancellable(config, NULL, error);
}

static const char *const alibaba_environment_keys[] = {
    "ALIBABA_CODING_PLAN_API_KEY",
    "ALIBABA_QWEN_API_KEY",
    "DASHSCOPE_API_KEY",
    NULL,
};

gboolean codexbar_alibaba_has_api_key(const CodexBarProviderConfig *config) {
    return has_value(config ? config->api_key : NULL, alibaba_environment_keys);
}

static json_object *expand_embedded_json(json_object *value, guint depth) {
    if (!value || depth > 8) return value ? json_object_get(value) : NULL;
    if (json_object_is_type(value, json_type_string)) {
        const char *text = json_object_get_string(value);
        size_t length = (size_t)json_object_get_string_len(value);
        if (text && length > 1 && (text[0] == '{' || text[0] == '[')) {
            json_object *parsed = parse_json_document(text, length);
            if (parsed) {
                json_object *expanded = expand_embedded_json(parsed, depth + 1);
                json_object_put(parsed);
                return expanded;
            }
        }
        return json_object_get(value);
    }
    if (json_object_is_type(value, json_type_array)) {
        json_object *result = json_object_new_array_ext((int)json_object_array_length(value));
        size_t count = json_object_array_length(value);
        for (size_t index = 0; index < count; index++) {
            json_object_array_add(result, expand_embedded_json(json_object_array_get_idx(value, index), depth + 1));
        }
        return result;
    }
    if (json_object_is_type(value, json_type_object)) {
        json_object *result = json_object_new_object();
        json_object_object_foreach(value, key, child) {
            json_object_object_add(result, key, expand_embedded_json(child, depth + 1));
        }
        return result;
    }
    return json_object_get(value);
}

static json_object *find_array_recursive(json_object *value, const char *const *keys, guint depth) {
    if (!value || depth > 16) return NULL;
    if (json_object_is_type(value, json_type_object)) {
        for (size_t index = 0; keys[index]; index++) {
            json_object *candidate = NULL;
            if (json_object_object_get_ex(value, keys[index], &candidate) &&
                json_object_is_type(candidate, json_type_array)) {
                return candidate;
            }
        }
        json_object_object_foreach(value, key, child) {
            (void)key;
            json_object *found = find_array_recursive(child, keys, depth + 1);
            if (found) return found;
        }
    } else if (json_object_is_type(value, json_type_array)) {
        size_t count = json_object_array_length(value);
        for (size_t index = 0; index < count; index++) {
            json_object *found = find_array_recursive(json_object_array_get_idx(value, index), keys, depth + 1);
            if (found) return found;
        }
    }
    return NULL;
}

static json_object *find_object_recursive(json_object *value, const char *const *keys, guint depth) {
    if (!value || depth > 16) return NULL;
    if (json_object_is_type(value, json_type_object)) {
        for (size_t index = 0; keys[index]; index++) {
            json_object *candidate = NULL;
            if (json_object_object_get_ex(value, keys[index], &candidate) &&
                json_object_is_type(candidate, json_type_object)) {
                return candidate;
            }
        }
        json_object_object_foreach(value, key, child) {
            (void)key;
            json_object *found = find_object_recursive(child, keys, depth + 1);
            if (found) return found;
        }
    } else if (json_object_is_type(value, json_type_array)) {
        size_t count = json_object_array_length(value);
        for (size_t index = 0; index < count; index++) {
            json_object *found = find_object_recursive(json_object_array_get_idx(value, index), keys, depth + 1);
            if (found) return found;
        }
    }
    return NULL;
}

static json_object *find_object_matching_recursive(json_object *value,
                                                   const char *const *keys,
                                                   guint depth) {
    if (!value || depth > 16) return NULL;
    if (json_object_is_type(value, json_type_object)) {
        for (size_t index = 0; keys[index]; index++) {
            json_object *ignored = NULL;
            if (json_object_object_get_ex(value, keys[index], &ignored)) return value;
        }
        json_object_object_foreach(value, key, child) {
            (void)key;
            json_object *found = find_object_matching_recursive(child, keys, depth + 1);
            if (found) return found;
        }
    } else if (json_object_is_type(value, json_type_array)) {
        size_t count = json_object_array_length(value);
        for (size_t index = 0; index < count; index++) {
            json_object *found =
                find_object_matching_recursive(json_object_array_get_idx(value, index), keys, depth + 1);
            if (found) return found;
        }
    }
    return NULL;
}

static gboolean find_double_recursive(json_object *value,
                                      const char *const *keys,
                                      double *result,
                                      guint depth) {
    if (!value || depth > 16) return FALSE;
    if (json_object_is_type(value, json_type_object)) {
        if (object_first_double(value, keys, result)) return TRUE;
        json_object_object_foreach(value, key, child) {
            (void)key;
            if (find_double_recursive(child, keys, result, depth + 1)) return TRUE;
        }
    } else if (json_object_is_type(value, json_type_array)) {
        size_t count = json_object_array_length(value);
        for (size_t index = 0; index < count; index++) {
            if (find_double_recursive(json_object_array_get_idx(value, index), keys, result, depth + 1)) return TRUE;
        }
    }
    return FALSE;
}

static char *find_string_recursive(json_object *value, const char *const *keys, guint depth) {
    if (!value || depth > 16) return NULL;
    if (json_object_is_type(value, json_type_object)) {
        char *result = object_first_string(value, keys);
        if (result) return result;
        json_object_object_foreach(value, key, child) {
            (void)key;
            result = find_string_recursive(child, keys, depth + 1);
            if (result) return result;
        }
    } else if (json_object_is_type(value, json_type_array)) {
        size_t count = json_object_array_length(value);
        for (size_t index = 0; index < count; index++) {
            char *result = find_string_recursive(json_object_array_get_idx(value, index), keys, depth + 1);
            if (result) return result;
        }
    }
    return NULL;
}

static gint alibaba_active_score(json_object *instance, gint64 now_ms) {
    const char *const status_keys[] = {"status", "instanceStatus", NULL};
    char *status = object_first_string(instance, status_keys);
    if (status) {
        gboolean active = g_ascii_strcasecmp(status, "VALID") == 0 || g_ascii_strcasecmp(status, "ACTIVE") == 0;
        gboolean inactive = g_ascii_strcasecmp(status, "EXPIRED") == 0 ||
                            g_ascii_strcasecmp(status, "INVALID") == 0 ||
                            g_ascii_strcasecmp(status, "INACTIVE") == 0 ||
                            g_ascii_strcasecmp(status, "DISABLED") == 0 ||
                            g_ascii_strcasecmp(status, "TERMINATED") == 0 ||
                            g_ascii_strcasecmp(status, "STOPPED") == 0;
        g_free(status);
        if (active) return 3;
        if (inactive) return -1;
    }
    json_object *active_value = NULL;
    if ((json_object_object_get_ex(instance, "isActive", &active_value) ||
         json_object_object_get_ex(instance, "active", &active_value)) &&
        json_object_is_type(active_value, json_type_boolean)) {
        return json_object_get_boolean(active_value) ? 3 : -1;
    }
    const char *const expiry_keys[] = {"endTime", "periodEndTime", "expireTime", "expirationTime", NULL};
    for (size_t index = 0; expiry_keys[index]; index++) {
        json_object *value = NULL;
        gint64 expiry_ms = 0;
        if (!json_object_object_get_ex(instance, expiry_keys[index], &value)) continue;
        if (epoch_value_ms(value, &expiry_ms) && expiry_ms > now_ms) return 1;
        if (json_object_is_type(value, json_type_string)) {
            char *text = object_string(instance, expiry_keys[index]);
            if (!text) continue;
            if (strlen(text) > 10 && text[10] == ' ') text[10] = 'T';
            GTimeZone *zone = g_time_zone_new_identifier("Asia/Shanghai");
            GDateTime *date = g_date_time_new_from_iso8601(text, zone);
            g_time_zone_unref(zone);
            g_free(text);
            if (date) {
                gboolean future = g_date_time_to_unix(date) * 1000 > now_ms;
                g_date_time_unref(date);
                if (future) return 1;
            }
        }
    }
    return 0;
}

static json_object *alibaba_selected_instance(json_object *root,
                                              guint *instance_count,
                                              gint *score,
                                              gint64 now_ms) {
    const char *const keys[] = {"codingPlanInstanceInfos", "coding_plan_instance_infos", NULL};
    json_object *instances = find_array_recursive(root, keys, 0);
    *instance_count = 0;
    *score = 0;
    if (!instances) return NULL;
    json_object *first = NULL;
    json_object *best = NULL;
    gint best_score = G_MININT;
    size_t count = json_object_array_length(instances);
    for (size_t index = 0; index < count; index++) {
        json_object *instance = json_object_array_get_idx(instances, index);
        if (!instance || !json_object_is_type(instance, json_type_object)) continue;
        (*instance_count)++;
        if (!first) first = instance;
        gint candidate_score = alibaba_active_score(instance, now_ms);
        if (candidate_score > best_score) {
            best = instance;
            best_score = candidate_score;
        }
    }
    if (best_score > 0) {
        *score = best_score;
        return best;
    }
    *score = first ? alibaba_active_score(first, now_ms) : 0;
    return first;
}

static char *alibaba_plan_name(json_object *value) {
    const char *const keys[] = {"planName", "plan_name", "instanceName", "instance_name", "packageName",
                                "package_name", NULL};
    return find_string_recursive(value, keys, 0);
}

static gboolean alibaba_add_window(CodexBarProvider *provider,
                                   json_object *quota,
                                   const char *id,
                                   const char *title,
                                   const char *const *used_keys,
                                   const char *const *total_keys,
                                   const char *const *reset_keys,
                                   gint64 minutes,
                                   gint64 now_ms) {
    double used = 0;
    double total = 0;
    if (!object_first_double(quota, used_keys, &used) || !object_first_double(quota, total_keys, &total) ||
        total <= 0) {
        return FALSE;
    }
    used = CLAMP(used, 0.0, total);
    gint64 reset_ms = 0;
    for (size_t index = 0; reset_keys[index] && reset_ms == 0; index++) {
        object_epoch_ms(quota, reset_keys[index], &reset_ms);
    }
    if (g_str_equal(id, "primary") && reset_ms > 0 && reset_ms - now_ms < 60000) {
        reset_ms += 5 * 60 * 60 * 1000;
        if (reset_ms - now_ms < 60000) reset_ms = now_ms + 5 * 60 * 60 * 1000;
    }
    char *detail = g_strdup_printf("%.0f / %.0f used", used, total);
    add_window(provider, id, title, used / total * 100.0, minutes, reset_ms, detail);
    g_free(detail);
    return TRUE;
}

CodexBarProvider *codexbar_alibaba_parse_api_usage(const char *json,
                                                   size_t length,
                                                   gint64 now_ms,
                                                   GError **error) {
    json_object *parsed = parse_json_document(json, length);
    if (!parsed) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Alibaba Coding Plan response is malformed");
        return NULL;
    }
    json_object *root = expand_embedded_json(parsed, 0);
    json_object_put(parsed);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Alibaba Coding Plan response is malformed");
        return NULL;
    }
    const char *const status_keys[] = {"statusCode", "status_code", "code", NULL};
    double status = 0;
    if (find_double_recursive(root, status_keys, &status, 0) && status != 0 && status != 200) {
        const char *const message_keys[] = {"statusMessage", "status_msg", "message", "msg", NULL};
        char *message = find_string_recursive(root, message_keys, 0);
        char *lower = message ? g_ascii_strdown(message, -1) : NULL;
        gboolean credentials = status == 401 || status == 403 ||
                               (lower && (strstr(lower, "api key") || strstr(lower, "unauthorized")));
        g_set_error(error,
                    G_IO_ERROR,
                    credentials ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_FAILED,
                    "Alibaba Coding Plan API error: %s",
                    message ? message : "nonzero status");
        g_free(lower);
        g_free(message);
        json_object_put(root);
        return NULL;
    }
    const char *const code_text_keys[] = {"code", "status", "statusCode", NULL};
    const char *const message_keys[] = {"message", "msg", "statusMessage", NULL};
    char *code_text = find_string_recursive(root, code_text_keys, 0);
    char *message = find_string_recursive(root, message_keys, 0);
    char *code_lower = code_text ? g_ascii_strdown(code_text, -1) : NULL;
    char *message_lower = message ? g_ascii_strdown(message, -1) : NULL;
    gboolean login_required = (code_lower && strstr(code_lower, "login")) ||
                              (message_lower && (strstr(message_lower, "login") ||
                                                 strstr(message_lower, "log in") ||
                                                 strstr(message_lower, "console session") ||
                                                 strstr(message_lower, "api key mode may be unavailable")));
    g_free(code_text);
    g_free(message);
    g_free(code_lower);
    g_free(message_lower);
    if (login_required) {
        json_object_put(root);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_SUPPORTED,
                            "Alibaba Coding Plan API key mode is unavailable for this region");
        return NULL;
    }

    guint instance_count = 0;
    gint active_score = 0;
    json_object *instance = alibaba_selected_instance(root, &instance_count, &active_score, now_ms);
    const char *const quota_keys[] = {"codingPlanQuotaInfo", "coding_plan_quota_info", NULL};
    json_object *quota = instance ? find_object_recursive(instance, quota_keys, 0) : NULL;
    if (!quota && instance_count <= 1) quota = find_object_recursive(root, quota_keys, 0);
    if (!quota) {
        const char *const quota_markers[] = {
            "per5HourUsedQuota", "per5HourTotalQuota", "perWeekUsedQuota", "perWeekTotalQuota",
            "perBillMonthUsedQuota", "perBillMonthTotalQuota", NULL,
        };
        json_object *candidate = instance_count > 1 && active_score > 0 ? instance : root;
        quota = find_object_matching_recursive(candidate, quota_markers, 0);
    }
    char *plan = alibaba_plan_name(instance ? instance : root);
    if (!quota && !(plan && active_score > 0)) {
        g_free(plan);
        json_object_put(root);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "Alibaba Coding Plan response has no active quota data");
        return NULL;
    }
    CodexBarProvider *provider = provider_new("alibaba", now_ms);
    add_identity(provider, plan);
    g_free(plan);
    if (quota) {
        const char *const five_used[] = {"per5HourUsedQuota", "perFiveHourUsedQuota", NULL};
        const char *const five_total[] = {"per5HourTotalQuota", "perFiveHourTotalQuota", NULL};
        const char *const five_reset[] = {"per5HourQuotaNextRefreshTime", "perFiveHourQuotaNextRefreshTime", NULL};
        const char *const week_used[] = {"perWeekUsedQuota", NULL};
        const char *const week_total[] = {"perWeekTotalQuota", NULL};
        const char *const week_reset[] = {"perWeekQuotaNextRefreshTime", NULL};
        const char *const month_used[] = {"perBillMonthUsedQuota", "perMonthUsedQuota", NULL};
        const char *const month_total[] = {"perBillMonthTotalQuota", "perMonthTotalQuota", NULL};
        const char *const month_reset[] = {"perBillMonthQuotaNextRefreshTime", "perMonthQuotaNextRefreshTime", NULL};
        alibaba_add_window(provider, quota, "primary", "5-hour", five_used, five_total, five_reset, 300, now_ms);
        alibaba_add_window(provider, quota, "secondary", "weekly", week_used, week_total, week_reset, 10080, now_ms);
        alibaba_add_window(provider, quota, "tertiary", "monthly", month_used, month_total, month_reset, 43200, now_ms);
    }
    json_object_put(root);
    if (provider->quota_windows->len > 0 || provider->plan) return provider;
    codexbar_provider_free(provider);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Alibaba Coding Plan response has no usable quota");
    return NULL;
}

static CodexBarProvider *alibaba_request_once(gboolean china,
                                              const char *key,
                                              CodexBarApiProviders3Transport transport,
                                              GCancellable *cancellable,
                                              gint64 now_ms,
                                              GError **error) {
    const char *url = china ? ALIBABA_CHINA_URL : ALIBABA_INTL_URL;
    const char *origin = china ? "https://bailian.console.aliyun.com"
                               : "https://modelstudio.console.alibabacloud.com";
    const char *referer = china
                              ? "https://bailian.console.aliyun.com/cn-beijing/?tab=model#/efm/coding_plan"
                              : "https://modelstudio.console.alibabacloud.com/ap-southeast-1/"
                                "?tab=coding-plan#/efm/coding_plan";
    const char *commodity = china ? "sfm_codingplan_public_cn" : "sfm_codingplan_public_intl";
    char *body = g_strdup_printf("{\"queryCodingPlanInstanceInfoRequest\":{\"commodityCode\":\"%s\"}}",
                                 commodity);
    char *bearer = g_strdup_printf("Bearer %s", key);
    const CodexBarHttpRequestHeader headers[] = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
        {"Authorization", bearer},
        {"x-api-key", key},
        {"X-DashScope-API-Key", key},
        {"User-Agent", "CodexBar"},
        {"Origin", origin},
        {"Referer", referer},
    };
    CodexBarHttpRequest request = {
        .url = url,
        .method = "POST",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .body = body,
        .body_length = strlen(body),
        .timeout_seconds = PROVIDER_TIMEOUT_SECONDS,
        .maximum_response_bytes = PROVIDER_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    g_free(bearer);
    g_free(body);
    if (!response) return NULL;
    if (response->status != 200) {
        GIOErrorEnum code = response->status == 401 || response->status == 403
                                ? G_IO_ERROR_PERMISSION_DENIED
                                : (response->status == 404 ? G_IO_ERROR_NOT_SUPPORTED : G_IO_ERROR_FAILED);
        g_set_error(error, G_IO_ERROR, code, "Alibaba Coding Plan API returned HTTP %ld", response->status);
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider =
        codexbar_alibaba_parse_api_usage(response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

CodexBarProvider *codexbar_alibaba_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders3Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    char *key = resolve_first(config ? config->api_key : NULL, alibaba_environment_keys);
    if (!key) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Alibaba Coding Plan API key is missing");
        return NULL;
    }
    gboolean china = config && config->region && g_ascii_strcasecmp(config->region, "cn") == 0;
    GError *first_error = NULL;
    CodexBarProvider *provider =
        alibaba_request_once(china, key, transport, cancellable, now_ms, &first_error);
    if (!provider && !china && first_error &&
        (g_error_matches(first_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED) ||
         g_error_matches(first_error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED) ||
         g_error_matches(first_error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA))) {
        g_clear_error(&first_error);
        provider = alibaba_request_once(TRUE, key, transport, cancellable, now_ms, &first_error);
    }
    g_free(key);
    if (provider) {
        g_clear_error(&first_error);
        return provider;
    }
    if (error) *error = first_error;
    else g_clear_error(&first_error);
    return NULL;
}

CodexBarProvider *codexbar_alibaba_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarApiProviders3Transport transport,
                                                        gint64 now_ms,
                                                        GError **error) {
    return codexbar_alibaba_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_alibaba_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error) {
    return codexbar_alibaba_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_alibaba_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_alibaba_fetch_with_cancellable(config, NULL, error);
}

static const char *const doubao_api_environment_keys[] = {
    "ARK_API_KEY", "VOLCENGINE_API_KEY", "DOUBAO_API_KEY", NULL,
};
static const char *const doubao_access_environment_keys[] = {
    "VOLCENGINE_ACCESS_KEY_ID", "VOLCENGINE_ACCESS_KEY", "VOLC_ACCESSKEY", "DOUBAO_ACCESS_KEY_ID", NULL,
};
static const char *const doubao_secret_environment_keys[] = {
    "VOLCENGINE_SECRET_ACCESS_KEY", "VOLCENGINE_SECRET_KEY", "VOLCENGINE_ACCESS_KEY_SECRET", "VOLC_SECRETKEY",
    "DOUBAO_SECRET_ACCESS_KEY", NULL,
};
static const char *const doubao_region_environment_keys[] = {
    "VOLCENGINE_REGION", "VOLCENGINE_REGION_ID", "VOLC_REGION", "DOUBAO_REGION", NULL,
};

static gboolean doubao_resolve_signed(const CodexBarProviderConfig *config,
                                      char **access,
                                      char **secret,
                                      char **region) {
    *access = NULL;
    *secret = NULL;
    *region = NULL;
    if (config && config->secret_key) {
        *access = clean_credential(config->api_key);
        *secret = clean_credential(config->secret_key);
    } else {
        *access = resolve_first(NULL, doubao_access_environment_keys);
        *secret = resolve_first(NULL, doubao_secret_environment_keys);
    }
    if (!*access || !*secret) {
        g_clear_pointer(access, g_free);
        g_clear_pointer(secret, g_free);
        return FALSE;
    }
    *region = clean_credential(config ? config->region : NULL);
    if (!*region) *region = resolve_first(NULL, doubao_region_environment_keys);
    if (!*region) *region = g_strdup("cn-beijing");
    return TRUE;
}

gboolean codexbar_doubao_has_credentials(const CodexBarProviderConfig *config) {
    char *access = NULL;
    char *secret = NULL;
    char *region = NULL;
    gboolean signed_credentials = doubao_resolve_signed(config, &access, &secret, &region);
    g_free(access);
    g_free(secret);
    g_free(region);
    return signed_credentials || has_value(config ? config->api_key : NULL, doubao_api_environment_keys);
}

static gint64 doubao_reset_ms(const char *raw, gint64 now_ms) {
    if (!raw || !*raw) return 0;
    GDateTime *date = g_date_time_new_from_iso8601(raw, NULL);
    if (date) {
        gint64 result = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
        g_date_time_unref(date);
        return result;
    }
    char *end = NULL;
    double plain = g_ascii_strtod(raw, &end);
    if (raw[0] != '\0' && end && *end == '\0' && isfinite(plain) && plain > 0 &&
        plain <= (double)(G_MAXINT64 - now_ms) / 1000.0) {
        return now_ms + (gint64)llround(plain * 1000.0);
    }
    gint64 seconds = 0;
    const char *cursor = raw;
    gboolean found = FALSE;
    while (*cursor) {
        if (!g_ascii_isdigit(*cursor)) return 0;
        guint64 number = 0;
        while (g_ascii_isdigit(*cursor)) {
            if (number > G_MAXUINT64 / 10) return 0;
            number = number * 10 + (guint)(*cursor - '0');
            cursor++;
        }
        guint64 multiplier = 0;
        switch (*cursor) {
        case 'd': multiplier = 86400; break;
        case 'h': multiplier = 3600; break;
        case 'm': multiplier = 60; break;
        case 's': multiplier = 1; break;
        default: return 0;
        }
        cursor++;
        if (number > (guint64)G_MAXINT64 / multiplier || seconds > G_MAXINT64 - (gint64)(number * multiplier)) {
            return 0;
        }
        seconds += (gint64)(number * multiplier);
        found = TRUE;
    }
    if (!found || seconds <= 0 || seconds > (G_MAXINT64 - now_ms) / 1000) return 0;
    return now_ms + seconds * 1000;
}

static gboolean doubao_add_named_quota(CodexBarProvider *provider,
                                       const char *level,
                                       double percent,
                                       gint64 reset_ms) {
    if (!level || !isfinite(percent)) return FALSE;
    char *lower = g_ascii_strdown(level, -1);
    const char *title = level;
    gint64 minutes = 0;
    if (g_str_equal(lower, "session") || g_str_equal(lower, "5-hour") ||
        g_str_equal(lower, "five_hour") || g_str_equal(lower, "5h") || g_str_has_suffix(lower, "_session") ||
        g_str_has_suffix(lower, "_5-hour") || g_str_has_suffix(lower, "_five_hour") ||
        g_str_has_suffix(lower, "_5h")) {
        title = "5-hour";
        minutes = 300;
    } else if (g_str_equal(lower, "weekly") || g_str_equal(lower, "week") ||
               g_str_has_suffix(lower, "_weekly") || g_str_has_suffix(lower, "_week")) {
        title = "weekly";
        minutes = 10080;
    } else if (g_str_equal(lower, "monthly") || g_str_equal(lower, "month") ||
               g_str_has_suffix(lower, "_monthly") || g_str_has_suffix(lower, "_month")) {
        title = "monthly";
        minutes = 43200;
    }
    char *id = g_strdup_printf("doubao-%s", lower);
    add_window(provider, id, title, percent, minutes, reset_ms, NULL);
    g_free(id);
    g_free(lower);
    return TRUE;
}

CodexBarProvider *codexbar_doubao_parse_coding_plan(const char *json,
                                                    size_t length,
                                                    gint64 now_ms,
                                                    GError **error) {
    json_object *root = parse_json_document(json, length);
    json_object *result = NULL;
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "Result", &result) || !json_object_is_type(result, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Doubao Coding Plan response is malformed");
        return NULL;
    }
    CodexBarProvider *provider = provider_new("doubao", now_ms);
    char *status = object_string(result, "Status");
    add_identity(provider, status);
    g_free(status);
    gint64 updated_ms = 0;
    if (object_epoch_ms(result, "UpdateTimestamp", &updated_ms)) provider->updated_at_ms = updated_ms;
    json_object *quotas = NULL;
    if (json_object_object_get_ex(result, "QuotaUsage", &quotas) &&
        !json_object_is_type(quotas, json_type_null)) {
        if (!json_object_is_type(quotas, json_type_array)) {
            json_object_put(root);
            codexbar_provider_free(provider);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Doubao Coding Plan quota is malformed");
            return NULL;
        }
        size_t count = json_object_array_length(quotas);
        for (size_t index = 0; index < count; index++) {
            json_object *quota = json_object_array_get_idx(quotas, index);
            char *level = object_string(quota, "Level");
            double percent = 0;
            gint64 reset_ms = 0;
            if (level && object_double(quota, "Percent", &percent)) {
                object_epoch_ms(quota, "ResetTimestamp", &reset_ms);
                doubao_add_named_quota(provider, level, percent, reset_ms);
            }
            g_free(level);
        }
    }
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_doubao_parse_agent_plan(const char *json,
                                                   size_t length,
                                                   gint64 now_ms,
                                                   GError **error) {
    json_object *root = parse_json_document(json, length);
    json_object *result = NULL;
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "Result", &result) || !json_object_is_type(result, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Doubao Agent Plan response is malformed");
        return NULL;
    }
    CodexBarProvider *provider = provider_new("doubao", now_ms);
    provider->plan = g_strdup("Agent Plan");
    const struct {
        const char *key;
        const char *level;
    } windows[] = {
        {"AFPFiveHour", "agent_5h"},
        {"AFPWeekly", "agent_weekly"},
        {"AFPMonthly", "agent_monthly"},
    };
    for (size_t index = 0; index < G_N_ELEMENTS(windows); index++) {
        json_object *window = NULL;
        if (!json_object_object_get_ex(result, windows[index].key, &window) ||
            !json_object_is_type(window, json_type_object)) {
            continue;
        }
        double quota = 0;
        double used = 0;
        if (!object_double(window, "Quota", &quota) || !object_double(window, "Used", &used) || quota <= 0) {
            continue;
        }
        gint64 reset_ms = 0;
        object_epoch_ms(window, "ResetTime", &reset_ms);
        doubao_add_named_quota(provider, windows[index].level, used / quota * 100.0, reset_ms);
    }
    json_object_put(root);
    return provider;
}

static char *hex_digest(const guint8 *data, size_t length) {
    char *result = g_malloc(length * 2 + 1);
    for (size_t index = 0; index < length; index++) g_snprintf(result + index * 2, 3, "%02x", data[index]);
    return result;
}

static char *sha256_hex(const void *data, size_t length) {
    return g_compute_checksum_for_data(G_CHECKSUM_SHA256, data, length);
}

static GBytes *hmac_sha256(GBytes *key, const char *message) {
    gsize key_length = 0;
    const guint8 *key_data = g_bytes_get_data(key, &key_length);
    GHmac *hmac = g_hmac_new(G_CHECKSUM_SHA256, key_data, key_length);
    g_hmac_update(hmac, (const guint8 *)message, strlen(message));
    guint8 digest[32];
    gsize digest_length = sizeof(digest);
    g_hmac_get_digest(hmac, digest, &digest_length);
    g_hmac_unref(hmac);
    return g_bytes_new(digest, digest_length);
}

static char *doubao_signature(const char *secret,
                              const char *date_stamp,
                              const char *region,
                              const char *string_to_sign) {
    GBytes *secret_key = g_bytes_new(secret, strlen(secret));
    GBytes *date_key = hmac_sha256(secret_key, date_stamp);
    GBytes *region_key = hmac_sha256(date_key, region);
    GBytes *service_key = hmac_sha256(region_key, "ark");
    GBytes *signing_key = hmac_sha256(service_key, "request");
    GBytes *signature = hmac_sha256(signing_key, string_to_sign);
    gsize length = 0;
    const guint8 *data = g_bytes_get_data(signature, &length);
    char *result = hex_digest(data, length);
    g_bytes_unref(signature);
    g_bytes_unref(signing_key);
    g_bytes_unref(service_key);
    g_bytes_unref(region_key);
    g_bytes_unref(date_key);
    g_bytes_unref(secret_key);
    return result;
}

static char *doubao_authorization(const char *access,
                                  const char *secret,
                                  const char *region,
                                  const char *action,
                                  gint64 now_ms,
                                  char **timestamp,
                                  char **payload_hash) {
    GDateTime *date = g_date_time_new_from_unix_utc(now_ms / 1000);
    *timestamp = g_date_time_format(date, "%Y%m%dT%H%M%SZ");
    char *date_stamp = g_date_time_format(date, "%Y%m%d");
    g_date_time_unref(date);
    *payload_hash = sha256_hex("", 0);
    const char *content_type = "application/x-www-form-urlencoded; charset=utf-8";
    char *query = g_strdup_printf("Action=%s&Version=2024-01-01", action);
    char *canonical = g_strdup_printf(
        "POST\n/\n%s\ncontent-type:%s\nhost:open.volcengineapi.com\nx-content-sha256:%s\nx-date:%s\n\n"
        "content-type;host;x-content-sha256;x-date\n%s",
        query,
        content_type,
        *payload_hash,
        *timestamp,
        *payload_hash);
    char *canonical_hash = sha256_hex(canonical, strlen(canonical));
    char *scope = g_strdup_printf("%s/%s/ark/request", date_stamp, region);
    char *string_to_sign = g_strdup_printf("HMAC-SHA256\n%s\n%s\n%s", *timestamp, scope, canonical_hash);
    char *signature = doubao_signature(secret, date_stamp, region, string_to_sign);
    char *authorization = g_strdup_printf(
        "HMAC-SHA256 Credential=%s/%s, SignedHeaders=content-type;host;x-content-sha256;x-date, Signature=%s",
        access,
        scope,
        signature);
    g_free(signature);
    g_free(string_to_sign);
    g_free(scope);
    g_free(canonical_hash);
    g_free(canonical);
    g_free(query);
    g_free(date_stamp);
    return authorization;
}

static CodexBarProvider *doubao_signed_request(const char *url,
                                               const char *action,
                                               const char *access,
                                               const char *secret,
                                               const char *region,
                                               CodexBarApiProviders3Transport transport,
                                               GCancellable *cancellable,
                                               gint64 now_ms,
                                               gboolean agent,
                                               GError **error) {
    char *timestamp = NULL;
    char *payload_hash = NULL;
    char *authorization =
        doubao_authorization(access, secret, region, action, now_ms, &timestamp, &payload_hash);
    const CodexBarHttpRequestHeader headers[] = {
        {"Accept", "application/json"},
        {"Content-Type", "application/x-www-form-urlencoded; charset=utf-8"},
        {"Host", "open.volcengineapi.com"},
        {"X-Date", timestamp},
        {"X-Content-Sha256", payload_hash},
        {"Authorization", authorization},
    };
    CodexBarHttpRequest request = {
        .url = url,
        .method = "POST",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .body = "",
        .body_length = 0,
        .timeout_seconds = PROVIDER_TIMEOUT_SECONDS,
        .maximum_response_bytes = PROVIDER_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    g_free(authorization);
    g_free(payload_hash);
    g_free(timestamp);
    if (!response) return NULL;
    if (response->status != 200) {
        g_set_error(error,
                    G_IO_ERROR,
                    response->status == 401 || response->status == 403 ? G_IO_ERROR_PERMISSION_DENIED
                                                                       : G_IO_ERROR_FAILED,
                    "Doubao plan API returned HTTP %ld",
                    response->status);
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = agent ? codexbar_doubao_parse_agent_plan(
                                            response->body, response->body_length, now_ms, error)
                                      : codexbar_doubao_parse_coding_plan(
                                            response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

static CodexBarProvider *doubao_fetch_signed(const char *access,
                                             const char *secret,
                                             const char *region,
                                             CodexBarApiProviders3Transport transport,
                                             GCancellable *cancellable,
                                             gint64 now_ms,
                                             GError **error) {
    CodexBarProvider *provider = doubao_signed_request(DOUBAO_CODING_PLAN_URL,
                                                      "GetCodingPlanUsage",
                                                      access,
                                                      secret,
                                                      region,
                                                      transport,
                                                      cancellable,
                                                      now_ms,
                                                      FALSE,
                                                      error);
    if (!provider || provider->quota_windows->len > 0) return provider;
    codexbar_provider_free(provider);
    return doubao_signed_request(DOUBAO_AGENT_PLAN_URL,
                                 "GetAFPUsage",
                                 access,
                                 secret,
                                 region,
                                 transport,
                                 cancellable,
                                 now_ms,
                                 TRUE,
                                 error);
}

static CodexBarProvider *doubao_probe(const char *key,
                                     const char *model,
                                     CodexBarApiProviders3Transport transport,
                                     GCancellable *cancellable,
                                     gint64 now_ms,
                                     long *status,
                                     gboolean *reliable,
                                     GError **error) {
    char *bearer = g_strdup_printf("Bearer %s", key);
    char *body = g_strdup_printf(
        "{\"model\":\"%s\",\"max_tokens\":1,\"messages\":[{\"role\":\"user\",\"content\":\"hi\"}]}",
        model);
    const CodexBarHttpRequestHeader headers[] = {
        {"Content-Type", "application/json"},
        {"Accept", "application/json"},
        {"Authorization", bearer},
    };
    CodexBarHttpRequest request = {
        .url = DOUBAO_ARK_URL,
        .method = "POST",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .body = body,
        .body_length = strlen(body),
        .timeout_seconds = PROVIDER_TIMEOUT_SECONDS,
        .maximum_response_bytes = PROVIDER_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    g_free(body);
    g_free(bearer);
    if (!response) return NULL;
    *status = response->status;
    if (response->status != 200 && response->status != 429) {
        g_set_error(error,
                    G_IO_ERROR,
                    response->status == 401 || response->status == 403 ? G_IO_ERROR_PERMISSION_DENIED
                                                                       : G_IO_ERROR_FAILED,
                    "Doubao API returned HTTP %ld",
                    response->status);
        codexbar_http_response_free(response);
        return NULL;
    }
    const char *limit_header = codexbar_http_response_header_first(response, "x-ratelimit-limit-requests");
    const char *remaining_header = codexbar_http_response_header_first(response, "x-ratelimit-remaining-requests");
    const char *reset_header = codexbar_http_response_header_first(response, "x-ratelimit-reset-requests");
    char *end = NULL;
    errno = 0;
    gint64 limit = limit_header ? g_ascii_strtoll(limit_header, &end, 10) : 0;
    gboolean has_limit = limit_header && limit_header[0] && end && *end == '\0' && errno == 0 && limit >= 0;
    end = NULL;
    errno = 0;
    gint64 remaining = remaining_header ? g_ascii_strtoll(remaining_header, &end, 10) : 0;
    gboolean has_remaining =
        remaining_header && remaining_header[0] && end && *end == '\0' && errno == 0 && remaining >= 0;
    *reliable = response->status == 429 ? has_limit : has_limit && has_remaining;
    CodexBarProvider *provider = provider_new("doubao", now_ms);
    if (*reliable && limit > 0) {
        if (!has_remaining) remaining = 0;
        remaining = CLAMP(remaining, 0, limit);
        char *detail = g_strdup_printf("%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT " requests",
                                       limit - remaining,
                                       limit);
        add_window(provider,
                   "primary",
                   "requests",
                   (double)(limit - remaining) / (double)limit * 100.0,
                   0,
                   doubao_reset_ms(reset_header, now_ms),
                   detail);
        g_free(detail);
    }
    codexbar_http_response_free(response);
    return provider;
}

static CodexBarProvider *doubao_fetch_bearer(const char *key,
                                             CodexBarApiProviders3Transport transport,
                                             GCancellable *cancellable,
                                             gint64 now_ms,
                                             GError **error) {
    const char *const models[] = {"doubao-seed-2.0-code", "doubao-1.5-pro-32k", "doubao-lite-32k", NULL};
    GError *last_error = NULL;
    for (size_t index = 0; models[index]; index++) {
        long status = 0;
        gboolean reliable = FALSE;
        CodexBarProvider *provider =
            doubao_probe(key, models[index], transport, cancellable, now_ms, &status, &reliable, &last_error);
        if (!provider) {
            if ((status == 403 || status == 404) && index < 2) {
                g_clear_error(&last_error);
                continue;
            }
            break;
        }
        if (!(status == 200 && reliable && provider->quota_windows->len == 1 &&
              codexbar_provider_quota_window(provider, 0)->used_percent == 100.0)) {
            g_clear_error(&last_error);
            return provider;
        }
        CodexBarProvider *initial = provider;
        long confirm_status = 0;
        gboolean confirm_reliable = FALSE;
        GError *confirm_error = NULL;
        CodexBarProvider *confirmation = doubao_probe(key,
                                                     models[index],
                                                     transport,
                                                     cancellable,
                                                     now_ms,
                                                     &confirm_status,
                                                     &confirm_reliable,
                                                     &confirm_error);
        if (!confirmation) {
            if (confirm_error && g_error_matches(confirm_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                codexbar_provider_free(initial);
                if (error) *error = confirm_error;
                else g_clear_error(&confirm_error);
                return NULL;
            }
            g_clear_error(&confirm_error);
            return initial;
        }
        if (confirm_status == 429) {
            if (confirm_reliable) {
                codexbar_provider_free(initial);
                return confirmation;
            }
            codexbar_provider_free(confirmation);
            return initial;
        }
        if (confirm_reliable && confirmation->quota_windows->len == 1 &&
            codexbar_provider_quota_window(confirmation, 0)->used_percent == 100.0) {
            codexbar_provider_free(initial);
            codexbar_provider_free(confirmation);
            return provider_new("doubao", now_ms);
        }
        codexbar_provider_free(initial);
        return confirmation;
    }
    if (error) *error = last_error;
    else g_clear_error(&last_error);
    return NULL;
}

CodexBarProvider *codexbar_doubao_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders3Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    char *access = NULL;
    char *secret = NULL;
    char *region = NULL;
    if (doubao_resolve_signed(config, &access, &secret, &region)) {
        CodexBarProvider *provider =
            doubao_fetch_signed(access, secret, region, transport, cancellable, now_ms, error);
        g_free(access);
        g_free(secret);
        g_free(region);
        return provider;
    }
    char *key = resolve_first(config ? config->api_key : NULL, doubao_api_environment_keys);
    if (!key) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Doubao API credentials are missing");
        return NULL;
    }
    CodexBarProvider *provider = doubao_fetch_bearer(key, transport, cancellable, now_ms, error);
    g_free(key);
    return provider;
}

CodexBarProvider *codexbar_doubao_fetch_with_transport(const CodexBarProviderConfig *config,
                                                       CodexBarApiProviders3Transport transport,
                                                       gint64 now_ms,
                                                       GError **error) {
    return codexbar_doubao_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_doubao_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error) {
    return codexbar_doubao_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_doubao_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_doubao_fetch_with_cancellable(config, NULL, error);
}
