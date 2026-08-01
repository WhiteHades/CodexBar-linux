#include "qwen_cloud.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define QWEN_CLOUD_DASHBOARD_ORIGIN "https://home.qwencloud.com"
#define QWEN_CLOUD_DATA_ORIGIN "https://cs-data.qwencloud.com"
#define QWEN_CLOUD_DASHBOARD_PATH "/billing/subscription/token-plan-individual"
#define QWEN_CLOUD_USER_INFO_PATH "/tool/user/info.json"
#define QWEN_CLOUD_USAGE_API "zeldaHttp.apikeyMgr./tokenplan/personal/api/v2/usage"
#define QWEN_CLOUD_SUBSCRIPTION_API "zeldaHttp.apikeyMgr./tokenplan/personal/api/v2/subscription"
#define QWEN_CLOUD_QUOTA_CONFIG_API "zeldaHttp.apikeyMgr./tokenplan/personal/api/v2/quota-config"
#define QWEN_CLOUD_PRODUCT_CODE "sfm_tokenplansolo_public_intl"
#define QWEN_CLOUD_CONSOLE_PRODUCT "sfm_bailian"
#define QWEN_CLOUD_CONSOLE_ACTION "IntlBroadScopeAspnGateway"
#define QWEN_CLOUD_REGION "ap-southeast-1"
#define QWEN_CLOUD_LANGUAGE "en-US"
#define QWEN_CLOUD_MAXIMUM_RESPONSE_BYTES (2U * 1024U * 1024U)

typedef struct {
    char *dashboard_origin;
    char *dashboard_url;
    char *user_info_url;
    char *quota_override;
} QwenCloudEndpoints;

static json_object *object_member_case(json_object *object, const char *name) {
    if (!object || !json_object_is_type(object, json_type_object)) return NULL;
    json_object_object_foreach(object, key, value) {
        if (g_ascii_strcasecmp(key, name) == 0) return value;
    }
    return NULL;
}

static json_object *parse_json_bytes(const char *text, size_t length) {
    if (!text || length > G_MAXINT || !g_utf8_validate(text, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new_ex(32);
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, text, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && g_ascii_isspace((guchar)text[consumed])) consumed++;
    json_tokener_free(tokener);
    if (parse_error == json_tokener_success && root && consumed == length) return root;
    if (root) json_object_put(root);
    return NULL;
}

static json_object *expand_embedded_json(json_object *value, unsigned depth) {
    if (!value || depth > 16) return value ? json_object_get(value) : NULL;
    switch (json_object_get_type(value)) {
    case json_type_object: {
        json_object *expanded = json_object_new_object();
        json_object_object_foreach(value, key, child) {
            json_object_object_add(expanded, key, expand_embedded_json(child, depth + 1));
        }
        return expanded;
    }
    case json_type_array: {
        json_object *expanded = json_object_new_array();
        size_t length = json_object_array_length(value);
        for (size_t index = 0; index < length; index++) {
            json_object_array_add(
                expanded, expand_embedded_json(json_object_array_get_idx(value, index), depth + 1));
        }
        return expanded;
    }
    case json_type_string: {
        const char *raw = json_object_get_string(value);
        size_t length = (size_t)json_object_get_string_len(value);
        size_t start = 0;
        while (start < length && g_ascii_isspace((guchar)raw[start])) start++;
        if (start == length || (raw[start] != '{' && raw[start] != '[')) return json_object_get(value);
        json_object *embedded = parse_json_bytes(raw + start, length - start);
        if (!embedded) return json_object_get(value);
        json_object *expanded = expand_embedded_json(embedded, depth + 1);
        json_object_put(embedded);
        return expanded;
    }
    default:
        return json_object_get(value);
    }
}

static json_object *find_value_for_key(json_object *value, const char *key, unsigned depth) {
    if (!value || depth > 20) return NULL;
    if (json_object_is_type(value, json_type_object)) {
        json_object *direct = object_member_case(value, key);
        if (direct && !json_object_is_type(direct, json_type_null)) return direct;
        json_object_object_foreach(value, child_key, child) {
            (void)child_key;
            json_object *found = find_value_for_key(child, key, depth + 1);
            if (found) return found;
        }
    } else if (json_object_is_type(value, json_type_array)) {
        size_t length = json_object_array_length(value);
        for (size_t index = 0; index < length; index++) {
            json_object *found = find_value_for_key(json_object_array_get_idx(value, index), key, depth + 1);
            if (found) return found;
        }
    }
    return NULL;
}

static json_object *find_object_with_any_key(json_object *value,
                                             const char *const *keys,
                                             size_t key_count,
                                             unsigned depth) {
    if (!value || depth > 20) return NULL;
    if (json_object_is_type(value, json_type_object)) {
        for (size_t index = 0; index < key_count; index++) {
            if (object_member_case(value, keys[index])) return value;
        }
        json_object_object_foreach(value, child_key, child) {
            (void)child_key;
            json_object *found = find_object_with_any_key(child, keys, key_count, depth + 1);
            if (found) return found;
        }
    } else if (json_object_is_type(value, json_type_array)) {
        size_t length = json_object_array_length(value);
        for (size_t index = 0; index < length; index++) {
            json_object *found =
                find_object_with_any_key(json_object_array_get_idx(value, index), keys, key_count, depth + 1);
            if (found) return found;
        }
    }
    return NULL;
}

static gboolean json_number(json_object *value, double *result) {
    if (!value) return FALSE;
    enum json_type type = json_object_get_type(value);
    double number = 0.0;
    if (type == json_type_int || type == json_type_double) {
        number = json_object_get_double(value);
    } else if (type == json_type_string) {
        const char *raw = json_object_get_string(value);
        char *clean = g_strdup(raw);
        g_strstrip(clean);
        char *end = NULL;
        number = g_ascii_strtod(clean, &end);
        gboolean valid = end && end != clean && *end == '\0' && isfinite(number);
        g_free(clean);
        if (!valid) return FALSE;
    } else {
        return FALSE;
    }
    if (!isfinite(number)) return FALSE;
    *result = number;
    return TRUE;
}

static gboolean find_number_for_key(json_object *value, const char *key, unsigned depth, double *result) {
    if (!value || depth > 20) return FALSE;
    if (json_object_is_type(value, json_type_object)) {
        json_object *direct = object_member_case(value, key);
        if (json_number(direct, result)) return TRUE;
        json_object_object_foreach(value, child_key, child) {
            (void)child_key;
            if (find_number_for_key(child, key, depth + 1, result)) return TRUE;
        }
    } else if (json_object_is_type(value, json_type_array)) {
        size_t length = json_object_array_length(value);
        for (size_t index = 0; index < length; index++) {
            if (find_number_for_key(json_object_array_get_idx(value, index), key, depth + 1, result)) return TRUE;
        }
    }
    return FALSE;
}

static gboolean find_number_for_keys(json_object *value,
                                     const char *const *keys,
                                     size_t key_count,
                                     double *result) {
    for (size_t index = 0; index < key_count; index++) {
        if (find_number_for_key(value, keys[index], 0, result)) return TRUE;
    }
    return FALSE;
}

static char *find_string_for_key(json_object *value, const char *key, unsigned depth) {
    if (!value || depth > 20) return NULL;
    if (json_object_is_type(value, json_type_object)) {
        json_object *direct = object_member_case(value, key);
        if (direct && json_object_is_type(direct, json_type_string)) {
            char *text = g_strdup(json_object_get_string(direct));
            g_strstrip(text);
            if (text[0] != '\0') return text;
            g_free(text);
        }
        json_object_object_foreach(value, child_key, child) {
            (void)child_key;
            char *found = find_string_for_key(child, key, depth + 1);
            if (found) return found;
        }
    } else if (json_object_is_type(value, json_type_array)) {
        size_t length = json_object_array_length(value);
        for (size_t index = 0; index < length; index++) {
            char *found = find_string_for_key(json_object_array_get_idx(value, index), key, depth + 1);
            if (found) return found;
        }
    }
    return NULL;
}

static char *find_string_for_keys(json_object *value, const char *const *keys, size_t key_count) {
    for (size_t index = 0; index < key_count; index++) {
        char *found = find_string_for_key(value, keys[index], 0);
        if (found) return found;
    }
    return NULL;
}

static gboolean json_boolean(json_object *value, gboolean *result) {
    if (!value) return FALSE;
    if (json_object_is_type(value, json_type_boolean)) {
        *result = json_object_get_boolean(value);
        return TRUE;
    }
    if (json_object_is_type(value, json_type_int)) {
        gint64 number = json_object_get_int64(value);
        if (number == 0 || number == 1) {
            *result = number == 1;
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean text_contains_ascii(const char *text, const char *needle) {
    if (!text) return FALSE;
    char *lower = g_ascii_strdown(text, -1);
    gboolean contains = strstr(lower, needle) != NULL;
    g_free(lower);
    return contains;
}

static gboolean login_error_text(const char *code, const char *message) {
    const char *needles[] = {
        "needlogin", "login", "postonlyortokenerror", "tokenerror", "request has expired", "refresh page",
    };
    for (size_t index = 0; index < G_N_ELEMENTS(needles); index++) {
        if (text_contains_ascii(code, needles[index]) || text_contains_ascii(message, needles[index])) return TRUE;
    }
    return FALSE;
}

static gboolean throw_if_error_payload(json_object *root, GError **error) {
    static const char *const code_keys[] = {"errorCode", "code", "status", "statusCode"};
    static const char *const message_keys[] = {"errorMsg", "message", "msg", "statusMessage"};
    gboolean failed = FALSE;
    gboolean success = TRUE;
    json_object *success_response = find_value_for_key(root, "successResponse", 0);
    if (json_boolean(success_response, &success) && !success) failed = TRUE;
    json_object *success_value = find_value_for_key(root, "success", 0);
    if (json_boolean(success_value, &success) && !success) failed = TRUE;

    double status = 0.0;
    static const char *const status_keys[] = {"statusCode", "status_code", "code"};
    gboolean has_status = find_number_for_keys(root, status_keys, G_N_ELEMENTS(status_keys), &status);
    if (has_status && status != 0.0 && status != 200.0) failed = TRUE;

    char *code = find_string_for_keys(root, code_keys, G_N_ELEMENTS(code_keys));
    char *message = find_string_for_keys(root, message_keys, G_N_ELEMENTS(message_keys));
    if (login_error_text(code, message)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Qwen Cloud login required.");
        g_free(code);
        g_free(message);
        return FALSE;
    }
    if (failed) {
        if (status == 401.0 || status == 403.0) {
            g_set_error_literal(
                error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Qwen Cloud rejected the stored session.");
        } else {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "Qwen Cloud usage API error%s%s",
                        message ? ": " : "",
                        message ? message : "");
        }
        g_free(code);
        g_free(message);
        return FALSE;
    }
    g_free(code);
    g_free(message);
    return TRUE;
}

static gboolean json_date_ms(json_object *value, gint64 *result) {
    double number = 0.0;
    if (json_number(value, &number) && number > 0.0) {
        double milliseconds = number >= 1000000000000.0 ? number : number * 1000.0;
        if (milliseconds > (double)G_MAXINT64) return FALSE;
        *result = (gint64)milliseconds;
        return TRUE;
    }
    if (!value || !json_object_is_type(value, json_type_string)) return FALSE;
    const char *raw = json_object_get_string(value);
    GDateTime *date = g_date_time_new_from_iso8601(raw, NULL);
    if (!date) return FALSE;
    *result = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static gboolean find_date_for_keys(json_object *value,
                                   const char *const *keys,
                                   size_t key_count,
                                   gint64 *result) {
    for (size_t index = 0; index < key_count; index++) {
        json_object *found = find_value_for_key(value, keys[index], 0);
        if (json_date_ms(found, result)) return TRUE;
    }
    return FALSE;
}

static char *format_credits(double value) {
    char raw[96];
    if (fabs(value - round(value)) < 0.0000001) {
        g_snprintf(raw, sizeof(raw), "%.0f", value);
    } else {
        g_snprintf(raw, sizeof(raw), "%.2f", value);
        size_t length = strlen(raw);
        while (length > 0 && raw[length - 1] == '0') raw[--length] = '\0';
        if (length > 0 && raw[length - 1] == '.') raw[--length] = '\0';
    }
    char *dot = strchr(raw, '.');
    size_t whole = dot ? (size_t)(dot - raw) : strlen(raw);
    size_t sign = raw[0] == '-' ? 1U : 0U;
    GString *formatted = g_string_sized_new(strlen(raw) + whole / 3);
    for (size_t index = 0; index < strlen(raw); index++) {
        if (index >= sign && index < whole && index > sign && (whole - index) % 3 == 0) {
            g_string_append_c(formatted, ',');
        }
        g_string_append_c(formatted, raw[index]);
    }
    return g_string_free(formatted, FALSE);
}

static CodexBarQuotaWindow *make_window(const char *id,
                                        const char *title,
                                        double used_percent,
                                        gint64 window_minutes,
                                        gboolean has_reset,
                                        gint64 reset_ms,
                                        gboolean has_total,
                                        double total) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = CLAMP(used_percent, 0.0, 100.0);
    window->has_window_minutes = TRUE;
    window->window_minutes = window_minutes;
    if (has_reset && reset_ms > 0) {
        window->has_resets_at = TRUE;
        window->resets_at_ms = reset_ms;
    }
    if (has_total && total > 0.0) {
        char *used = format_credits(total * window->used_percent / 100.0);
        char *limit = format_credits(total);
        window->reset_description = g_strdup_printf("%s / %s credits used", used, limit);
        g_free(used);
        g_free(limit);
    }
    return window;
}

static char *display_plan_name(const char *code) {
    if (!code) return NULL;
    if (g_ascii_strcasecmp(code, "lite") == 0) return g_strdup("Lite");
    if (g_ascii_strcasecmp(code, "standard") == 0) return g_strdup("Standard");
    if (g_ascii_strcasecmp(code, "pro") == 0) return g_strdup("Pro");
    if (g_ascii_strcasecmp(code, "max") == 0) return g_strdup("Max");
    return g_strdup(code);
}

static json_object *parse_optional_expanded(const char *json) {
    if (!json) return NULL;
    json_object *root = parse_json_bytes(json, strlen(json));
    if (!root) return NULL;
    json_object *expanded = expand_embedded_json(root, 0);
    json_object_put(root);
    return expanded;
}

static CodexBarProvider *parse_current_usage(json_object *usage,
                                             const char *subscription_json,
                                             const char *quota_config_json,
                                             gint64 now_ms) {
    static const char *const current_keys[] = {"per5HourPercentage", "per1WeekPercentage"};
    json_object *current = find_object_with_any_key(usage, current_keys, G_N_ELEMENTS(current_keys), 0);
    if (!current) return NULL;

    double five_ratio = 0.0;
    double weekly_ratio = 0.0;
    gboolean has_five = json_number(object_member_case(current, "per5HourPercentage"), &five_ratio) &&
                        isfinite(five_ratio);
    gboolean has_weekly = json_number(object_member_case(current, "per1WeekPercentage"), &weekly_ratio) &&
                          isfinite(weekly_ratio);
    if (!has_five && !has_weekly) return NULL;

    json_object *subscription = parse_optional_expanded(subscription_json);
    json_object *quota_config = parse_optional_expanded(quota_config_json);
    static const char *const plan_keys[] = {"specCode", "spec_code", "planName", "plan_name"};
    char *plan_code = subscription ? find_string_for_keys(subscription, plan_keys, G_N_ELEMENTS(plan_keys)) : NULL;
    if (plan_code) {
        char *lower = g_ascii_strdown(plan_code, -1);
        g_free(plan_code);
        plan_code = lower;
    }
    char *plan = display_plan_name(plan_code);

    gboolean has_five_total = FALSE;
    gboolean has_weekly_total = FALSE;
    double five_total = 0.0;
    double weekly_total = 0.0;
    if (plan_code && quota_config) {
        json_object *plan_quota = find_value_for_key(quota_config, plan_code, 0);
        if (plan_quota && json_object_is_type(plan_quota, json_type_object)) {
            static const char *const five_total_keys[] = {"five_hour", "fiveHour"};
            static const char *const weekly_total_keys[] = {"weekly"};
            has_five_total =
                find_number_for_keys(plan_quota, five_total_keys, G_N_ELEMENTS(five_total_keys), &five_total);
            has_weekly_total = find_number_for_keys(
                plan_quota, weekly_total_keys, G_N_ELEMENTS(weekly_total_keys), &weekly_total);
        }
    }

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("qwencloud");
    provider->source = g_strdup("web");
    provider->dashboard_url = g_strdup(QWEN_CLOUD_DASHBOARD_ORIGIN QWEN_CLOUD_DASHBOARD_PATH);
    provider->plan = plan ? g_strdup(plan) : NULL;
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    if (plan) {
        provider->identity = g_new0(CodexBarProviderIdentity, 1);
        provider->identity->login_method = g_strdup(plan);
    }
    if (has_five) {
        gint64 reset_ms = 0;
        gboolean has_reset = json_date_ms(object_member_case(current, "per5HourResetTime"), &reset_ms);
        codexbar_provider_add_quota_window(provider,
                                           make_window("primary",
                                                       "5-hour",
                                                       CLAMP(five_ratio, 0.0, 1.0) * 100.0,
                                                       5 * 60,
                                                       has_reset,
                                                       reset_ms,
                                                       has_five_total,
                                                       five_total));
    }
    if (has_weekly) {
        gint64 reset_ms = 0;
        gboolean has_reset = json_date_ms(object_member_case(current, "per1WeekResetTime"), &reset_ms);
        codexbar_provider_add_quota_window(provider,
                                           make_window("secondary",
                                                       "Weekly",
                                                       CLAMP(weekly_ratio, 0.0, 1.0) * 100.0,
                                                       7 * 24 * 60,
                                                       has_reset,
                                                       reset_ms,
                                                       has_weekly_total,
                                                       weekly_total));
    }

    g_free(plan);
    g_free(plan_code);
    if (subscription) json_object_put(subscription);
    if (quota_config) json_object_put(quota_config);
    return provider;
}

static CodexBarProvider *parse_legacy_usage(json_object *root, gint64 now_ms, GError **error) {
    static const char *const used_keys[] = {
        "usedQuota", "used_quota", "usedCredits", "usedCredit", "consumedCredits", "usage", "used",
        "usedAmount", "consumeAmount", "usedValue", "ConsumedValue",
    };
    static const char *const total_keys[] = {
        "totalQuota", "total_quota", "totalCredits", "totalCredit", "quota", "creditLimit", "creditsTotal",
        "monthlyTotalQuota", "amount", "totalValue", "cycleTotalValue",
    };
    static const char *const remaining_keys[] = {
        "remainingQuota", "remainQuota", "remainingCredits", "remainingCredit", "availableCredits", "balance",
        "remaining", "availableAmount", "remainAmount", "totalSurplusValue", "surplusValue", "cycleSurplusValue",
    };
    static const char *const count_keys[] = {"totalCount", "subscriptionTotalNumber"};
    static const char *const plan_keys[] = {
        "planName", "plan_name", "packageName", "commodityName", "specType", "instanceName", "displayName",
        "ProductName", "name", "title", "planType",
    };
    static const char *const reset_keys[] = {
        "nextRefreshTime", "resetTime", "periodEndTime", "billingCycleEnd", "billCycleEndTime", "expireTime",
        "expirationTime", "endTime", "validEndTime", "instanceEndTime", "cycleEndTime", "nearestExpireDate",
    };

    double total = 0.0;
    double remaining = 0.0;
    double used = 0.0;
    double count = 0.0;
    gboolean has_total = find_number_for_keys(root, total_keys, G_N_ELEMENTS(total_keys), &total);
    gboolean has_remaining =
        find_number_for_keys(root, remaining_keys, G_N_ELEMENTS(remaining_keys), &remaining);
    gboolean has_used = find_number_for_keys(root, used_keys, G_N_ELEMENTS(used_keys), &used);
    gboolean has_count = find_number_for_keys(root, count_keys, G_N_ELEMENTS(count_keys), &count);
    if (!has_used && has_total && has_remaining) {
        used = MAX(0.0, total - remaining);
        has_used = TRUE;
    }
    char *plan = find_string_for_keys(root, plan_keys, G_N_ELEMENTS(plan_keys));
    if (!plan && ((has_count && count > 0.0) || has_total)) plan = g_strdup("TOKEN PLAN");
    if (!plan && !has_total && !has_remaining && !has_used && !has_count) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Missing Qwen Cloud token plan data.");
        return NULL;
    }

    gint64 reset_ms = 0;
    gboolean has_reset = find_date_for_keys(root, reset_keys, G_N_ELEMENTS(reset_keys), &reset_ms);
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("qwencloud");
    provider->source = g_strdup("web");
    provider->dashboard_url = g_strdup(QWEN_CLOUD_DASHBOARD_ORIGIN QWEN_CLOUD_DASHBOARD_PATH);
    provider->plan = plan ? g_strdup(plan) : NULL;
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    if (plan) {
        provider->identity = g_new0(CodexBarProviderIdentity, 1);
        provider->identity->login_method = g_strdup(plan);
    }
    if (has_total && total > 0.0 && (has_used || has_remaining)) {
        if (!has_used) used = MAX(0.0, total - remaining);
        used = CLAMP(used, 0.0, total);
        CodexBarQuotaWindow *window = make_window(
            "primary", "5-hour", used / total * 100.0, 30 * 24 * 60, has_reset, reset_ms, TRUE, total);
        if (has_used) {
            char *used_text = format_credits(used);
            char *total_text = format_credits(total);
            g_free(window->reset_description);
            window->reset_description = g_strdup_printf("%s / %s credits used", used_text, total_text);
            g_free(used_text);
            g_free(total_text);
        }
        codexbar_provider_add_quota_window(provider, window);
    }
    g_free(plan);
    return provider;
}

CodexBarProvider *codexbar_qwen_cloud_parse_usage(const char *usage_json,
                                                  const char *subscription_json,
                                                  const char *quota_config_json,
                                                  gint64 now_ms,
                                                  GError **error) {
    if (!usage_json) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Qwen Cloud response is empty.");
        return NULL;
    }
    json_object *root = parse_json_bytes(usage_json, strlen(usage_json));
    if (!root) {
        if (text_contains_ascii(usage_json, "<html") &&
            (text_contains_ascii(usage_json, "login") || text_contains_ascii(usage_json, "sign in"))) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Qwen Cloud login required.");
        } else {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Invalid Qwen Cloud JSON response.");
        }
        return NULL;
    }
    json_object *expanded = expand_embedded_json(root, 0);
    json_object_put(root);
    if (!throw_if_error_payload(expanded, error)) {
        json_object_put(expanded);
        return NULL;
    }
    CodexBarProvider *provider =
        parse_current_usage(expanded, subscription_json, quota_config_json, now_ms);
    if (!provider) provider = parse_legacy_usage(expanded, now_ms, error);
    json_object_put(expanded);
    return provider;
}

static char *clean_setting(const char *raw) {
    if (!raw || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *clean = g_strdup(raw);
    g_strstrip(clean);
    size_t length = strlen(clean);
    if (length >= 2 && ((clean[0] == '\'' && clean[length - 1] == '\'') ||
                        (clean[0] == '"' && clean[length - 1] == '"'))) {
        clean[length - 1] = '\0';
        memmove(clean, clean + 1, length - 1);
        g_strstrip(clean);
    }
    if (clean[0] == '\0') {
        g_free(clean);
        return NULL;
    }
    for (const unsigned char *cursor = (const unsigned char *)clean; *cursor; cursor++) {
        if ((*cursor < 32 && *cursor != '\t') || *cursor == 127) {
            g_free(clean);
            return NULL;
        }
    }
    return clean;
}

static char *normalize_cookie(const char *raw) {
    char *clean = clean_setting(raw);
    if (!clean) return NULL;
    if (g_ascii_strncasecmp(clean, "Cookie:", 7) == 0) {
        memmove(clean, clean + 7, strlen(clean + 7) + 1);
        g_strstrip(clean);
    }
    GString *normalized = g_string_new(NULL);
    char **parts = g_strsplit(clean, ";", -1);
    for (size_t index = 0; parts[index]; index++) {
        char *pair = parts[index];
        g_strstrip(pair);
        char *equals = strchr(pair, '=');
        if (!equals || equals == pair) continue;
        *equals = '\0';
        g_strstrip(pair);
        g_strstrip(equals + 1);
        if (pair[0] == '\0' || strpbrk(pair, "()<>@,;:\\\"/[]?={} \t") != NULL) continue;
        if (normalized->len > 0) g_string_append(normalized, "; ");
        g_string_append_printf(normalized, "%s=%s", pair, equals + 1);
    }
    g_strfreev(parts);
    g_free(clean);
    if (normalized->len > 0) return g_string_free(normalized, FALSE);
    g_string_free(normalized, TRUE);
    return NULL;
}

static const char *config_string(const CodexBarProviderConfig *config, const char *key) {
    json_object *value = NULL;
    if (!config || !config->raw || !json_object_is_type(config->raw, json_type_object) ||
        !json_object_object_get_ex(config->raw, key, &value) || !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    return json_object_get_string(value);
}

static char *resolve_cookie(const CodexBarProviderConfig *config, GError **error) {
    const char *source = config_string(config, "cookieSource");
    if (source && g_str_equal(source, "off")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Qwen Cloud cookies are disabled.");
        return NULL;
    }
    if (source && g_str_equal(source, "manual")) {
        char *manual = normalize_cookie(config_string(config, "cookieHeader"));
        if (manual) return manual;
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Qwen Cloud cookie header is invalid.");
        return NULL;
    }
    char *environment = normalize_cookie(g_getenv("QWEN_CLOUD_COOKIE"));
    if (environment) return environment;
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        "No Qwen Cloud cookie found. Set QWEN_CLOUD_COOKIE or configure a manual Cookie header.");
    return NULL;
}

gboolean codexbar_qwen_cloud_has_cookie(const CodexBarProviderConfig *config) {
    GError *error = NULL;
    char *cookie = resolve_cookie(config, &error);
    g_clear_error(&error);
    g_free(cookie);
    return cookie != NULL;
}

static char *cookie_value(const char *cookie, const char *name) {
    if (!cookie || !name) return NULL;
    char **pairs = g_strsplit(cookie, ";", -1);
    char *result = NULL;
    for (size_t index = 0; pairs[index]; index++) {
        char *pair = pairs[index];
        g_strstrip(pair);
        char *equals = strchr(pair, '=');
        if (!equals) continue;
        *equals = '\0';
        g_strstrip(pair);
        if (g_ascii_strcasecmp(pair, name) != 0) continue;
        char *value = equals + 1;
        g_strstrip(value);
        if (value[0] != '\0') result = g_strdup(value);
        break;
    }
    g_strfreev(pairs);
    return result;
}

static gboolean allowed_credential_host(const char *host, int port) {
    if (!host) return FALSE;
    char *lower = g_ascii_strdown(host, -1);
    gboolean production = g_str_equal(lower, "qwencloud.com") || g_str_has_suffix(lower, ".qwencloud.com");
    gboolean reserved_test = g_str_equal(lower, "test") || g_str_has_suffix(lower, ".test");
    g_free(lower);
    if (production && port > 0 && port != 443) return FALSE;
    return production || reserved_test;
}

static char *validated_endpoint(const char *raw, gboolean origin_only, GError **error) {
    char *clean = clean_setting(raw);
    if (!clean) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Qwen Cloud endpoint is invalid.");
        return NULL;
    }
    char *normalized = codexbar_http_normalize_endpoint(clean, CODEXBAR_HTTP_HTTPS_ONLY, error);
    g_free(clean);
    if (!normalized) return NULL;
    GUri *uri = g_uri_parse(normalized, G_URI_FLAGS_NONE, NULL);
    const char *host = uri ? g_uri_get_host(uri) : NULL;
    const char *path = uri ? g_uri_get_path(uri) : NULL;
    gboolean clean_origin = !origin_only || ((!path || path[0] == '\0' || g_str_equal(path, "/")) &&
                                             !g_uri_get_query(uri) && !g_uri_get_fragment(uri));
    if (!uri || !allowed_credential_host(host, uri ? g_uri_get_port(uri) : -1) || !clean_origin) {
        if (uri) g_uri_unref(uri);
        g_free(normalized);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PERMISSION_DENIED,
                            "Qwen Cloud credentials may only be sent to HTTPS qwencloud.com or reserved .test hosts.");
        return NULL;
    }
    g_uri_unref(uri);
    return normalized;
}

static char *trim_trailing_slash(char *value) {
    size_t length = strlen(value);
    while (length > 0 && value[length - 1] == '/') value[--length] = '\0';
    return value;
}

static void endpoints_clear(QwenCloudEndpoints *endpoints) {
    g_free(endpoints->dashboard_origin);
    g_free(endpoints->dashboard_url);
    g_free(endpoints->user_info_url);
    g_free(endpoints->quota_override);
    memset(endpoints, 0, sizeof(*endpoints));
}

static gboolean resolve_endpoints(QwenCloudEndpoints *endpoints, GError **error) {
    memset(endpoints, 0, sizeof(*endpoints));
    const char *host_override = g_getenv("QWEN_CLOUD_HOST");
    endpoints->dashboard_origin = validated_endpoint(
        host_override && host_override[0] != '\0' ? host_override : QWEN_CLOUD_DASHBOARD_ORIGIN, TRUE, error);
    if (!endpoints->dashboard_origin) return FALSE;
    trim_trailing_slash(endpoints->dashboard_origin);
    endpoints->dashboard_url = g_strdup_printf("%s%s", endpoints->dashboard_origin, QWEN_CLOUD_DASHBOARD_PATH);
    endpoints->user_info_url = g_strdup_printf("%s%s", endpoints->dashboard_origin, QWEN_CLOUD_USER_INFO_PATH);

    const char *quota_override = g_getenv("QWEN_CLOUD_QUOTA_URL");
    if (quota_override && quota_override[0] != '\0') {
        endpoints->quota_override = validated_endpoint(quota_override, FALSE, error);
        if (!endpoints->quota_override) {
            endpoints_clear(endpoints);
            return FALSE;
        }
    }
    return TRUE;
}

static char *api_url(const QwenCloudEndpoints *endpoints, const char *api) {
    if (endpoints->quota_override) return g_strdup(endpoints->quota_override);
    const char *host_override = g_getenv("QWEN_CLOUD_HOST");
    const char *origin = host_override && host_override[0] != '\0' ? endpoints->dashboard_origin
                                                                   : QWEN_CLOUD_DATA_ORIGIN;
    char *escaped_action = g_uri_escape_string(QWEN_CLOUD_CONSOLE_ACTION, NULL, FALSE);
    char *escaped_product = g_uri_escape_string(QWEN_CLOUD_CONSOLE_PRODUCT, NULL, FALSE);
    char *escaped_api = g_uri_escape_string(api, NULL, FALSE);
    char *url = g_strdup_printf("%s/data/api.json?action=%s&product=%s&api=%s&_v=undefined",
                                origin,
                                escaped_action,
                                escaped_product,
                                escaped_api);
    g_free(escaped_action);
    g_free(escaped_product);
    g_free(escaped_api);
    return url;
}

static char *extract_html_token(const char *html) {
    if (!html) return NULL;
    const char *keys[] = {"secToken", "sec_token", "csrfToken"};
    for (size_t key_index = 0; key_index < G_N_ELEMENTS(keys); key_index++) {
        const char *cursor = html;
        while ((cursor = strstr(cursor, keys[key_index]))) {
            cursor += strlen(keys[key_index]);
            while (*cursor == '\'' || *cursor == '"' || g_ascii_isspace((guchar)*cursor)) cursor++;
            if (*cursor != ':' && *cursor != '=') continue;
            cursor++;
            while (g_ascii_isspace((guchar)*cursor)) cursor++;
            if (*cursor != '\'' && *cursor != '"') continue;
            char quote = *cursor++;
            const char *end = strchr(cursor, quote);
            if (!end || end == cursor) continue;
            char *token = g_strndup(cursor, (gsize)(end - cursor));
            g_strstrip(token);
            if (token[0] != '\0') return token;
            g_free(token);
        }
    }
    return NULL;
}

static gboolean looks_like_login_page(const char *html) {
    return text_contains_ascii(html, "passport.alibabacloud.com") ||
           text_contains_ascii(html, "signin.aliyun.com") ||
           text_contains_ascii(html, "account.alibabacloud.com/login") ||
           text_contains_ascii(html, "login.qwencloud.com") ||
           (text_contains_ascii(html, "login") && text_contains_ascii(html, "password") &&
            text_contains_ascii(html, "sign in"));
}

static char *response_text(const CodexBarHttpResponse *response, GError **error) {
    if (!response || !response->body || memchr(response->body, '\0', response->body_length)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Qwen Cloud response body is invalid.");
        return NULL;
    }
    return g_strndup(response->body, response->body_length);
}

static char *resolve_sec_token(const QwenCloudEndpoints *endpoints,
                               const char *cookie,
                               CodexBarQwenCloudTransport transport,
                               GCancellable *cancellable,
                               GError **error) {
    const CodexBarHttpRequestHeader dashboard_headers[] = {
        {"Accept", "text/html,application/xhtml+xml"},
        {"Cookie", cookie},
    };
    const CodexBarHttpRequest dashboard_request = {
        .url = endpoints->dashboard_url,
        .method = "GET",
        .headers = dashboard_headers,
        .header_count = G_N_ELEMENTS(dashboard_headers),
        .timeout_seconds = 20,
        .maximum_response_bytes = QWEN_CLOUD_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    GError *dashboard_error = NULL;
    CodexBarHttpResponse *dashboard = transport(&dashboard_request, &dashboard_error);
    if (dashboard) {
        if (dashboard->status == 200) {
            char *html = response_text(dashboard, &dashboard_error);
            if (html) {
                if (!looks_like_login_page(html)) {
                    char *token = extract_html_token(html);
                    g_free(html);
                    if (token) {
                        codexbar_http_response_free(dashboard);
                        return token;
                    }
                } else {
                    g_free(html);
                }
            }
        } else if (dashboard->status >= 500) {
            g_set_error(&dashboard_error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "Qwen Cloud dashboard returned HTTP %ld.",
                        dashboard->status);
        }
        codexbar_http_response_free(dashboard);
    }
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) {
        g_clear_error(&dashboard_error);
        return NULL;
    }

    char *cookie_token = cookie_value(cookie, "sec_token");
    if (cookie_token) {
        g_clear_error(&dashboard_error);
        return cookie_token;
    }

    const CodexBarHttpRequestHeader user_headers[] = {
        {"Accept", "application/json, text/plain, */*"},
        {"Cookie", cookie},
    };
    const CodexBarHttpRequest user_request = {
        .url = endpoints->user_info_url,
        .method = "GET",
        .headers = user_headers,
        .header_count = G_N_ELEMENTS(user_headers),
        .timeout_seconds = 20,
        .maximum_response_bytes = QWEN_CLOUD_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    GError *user_error = NULL;
    CodexBarHttpResponse *user_info = transport(&user_request, &user_error);
    if (user_info) {
        if (user_info->status == 200) {
            char *text = response_text(user_info, &user_error);
            if (text) {
                json_object *user_root = parse_json_bytes(text, strlen(text));
                g_free(text);
                if (user_root) {
                    json_object *expanded = expand_embedded_json(user_root, 0);
                    json_object_put(user_root);
                    static const char *const token_keys[] = {"secToken", "sec_token", "csrfToken", "token"};
                    char *token = find_string_for_keys(expanded, token_keys, G_N_ELEMENTS(token_keys));
                    json_object_put(expanded);
                    codexbar_http_response_free(user_info);
                    if (token) {
                        g_clear_error(&dashboard_error);
                        g_clear_error(&user_error);
                        return token;
                    }
                    user_info = NULL;
                }
            }
        }
        if (user_info) codexbar_http_response_free(user_info);
    }
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) {
        g_clear_error(&dashboard_error);
        g_clear_error(&user_error);
        return NULL;
    }
    if (user_error) {
        g_clear_error(&dashboard_error);
        g_propagate_error(error, user_error);
        return NULL;
    }
    if (dashboard_error) {
        g_propagate_error(error, dashboard_error);
        return NULL;
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Qwen Cloud login required.");
    return NULL;
}

static char *form_escape(const char *value) {
    return g_uri_escape_string(value ? value : "", NULL, FALSE);
}

static char *make_api_body(const char *api,
                           const char *sec_token,
                           const char *cookie,
                           const QwenCloudEndpoints *endpoints,
                           gboolean subscription) {
    json_object *cornerstone = json_object_new_object();
    char *uuid = g_uuid_string_random();
    json_object_object_add(cornerstone, "feTraceId", json_object_new_string(uuid));
    json_object_object_add(cornerstone, "feURL", json_object_new_string(endpoints->dashboard_url));
    json_object_object_add(cornerstone, "protocol", json_object_new_string("V2"));
    json_object_object_add(cornerstone, "console", json_object_new_string("ONE_CONSOLE"));
    json_object_object_add(cornerstone, "productCode", json_object_new_string("p_efm"));
    GUri *dashboard_uri = g_uri_parse(endpoints->dashboard_origin, G_URI_FLAGS_NONE, NULL);
    json_object_object_add(cornerstone,
                           "domain",
                           json_object_new_string(dashboard_uri ? g_uri_get_host(dashboard_uri)
                                                                : "home.qwencloud.com"));
    json_object_object_add(cornerstone, "consoleSite", json_object_new_string("QWENCLOUD"));
    json_object_object_add(cornerstone, "userNickName", json_object_new_string(""));
    json_object_object_add(cornerstone, "userPrincipalName", json_object_new_string(""));
    json_object_object_add(cornerstone, "xsp_lang", json_object_new_string(QWEN_CLOUD_LANGUAGE));
    char *anonymous_id = cookie_value(cookie, "cna");
    if (anonymous_id) {
        json_object_object_add(cornerstone, "X-Anonymous-Id", json_object_new_string(anonymous_id));
    }
    json_object *data = json_object_new_object();
    if (subscription) {
        json_object_object_add(data, "commodityCode", json_object_new_string(QWEN_CLOUD_PRODUCT_CODE));
    }
    json_object_object_add(data, "cornerstoneParam", cornerstone);
    json_object *params = json_object_new_object();
    json_object_object_add(params, "Api", json_object_new_string(api));
    json_object_object_add(params, "V", json_object_new_string("1.0"));
    json_object_object_add(params, "Data", data);
    const char *params_json = json_object_to_json_string_ext(params, JSON_C_TO_STRING_PLAIN);

    char *product = form_escape(QWEN_CLOUD_CONSOLE_PRODUCT);
    char *action = form_escape(QWEN_CLOUD_CONSOLE_ACTION);
    char *token = form_escape(sec_token);
    char *region = form_escape(QWEN_CLOUD_REGION);
    char *language = form_escape(QWEN_CLOUD_LANGUAGE);
    char *encoded_params = form_escape(params_json);
    char *body = g_strdup_printf("product=%s&action=%s&sec_token=%s&region=%s&language=%s&params=%s",
                                 product,
                                 action,
                                 token,
                                 region,
                                 language,
                                 encoded_params);
    g_free(product);
    g_free(action);
    g_free(token);
    g_free(region);
    g_free(language);
    g_free(encoded_params);
    g_free(anonymous_id);
    if (dashboard_uri) g_uri_unref(dashboard_uri);
    g_free(uuid);
    json_object_put(params);
    return body;
}

static char *fetch_api(const QwenCloudEndpoints *endpoints,
                       const char *api,
                       gboolean subscription,
                       const char *cookie,
                       const char *sec_token,
                       CodexBarQwenCloudTransport transport,
                       GCancellable *cancellable,
                       GError **error) {
    char *url = api_url(endpoints, api);
    char *body = make_api_body(api, sec_token, cookie, endpoints, subscription);
    char *csrf = cookie_value(cookie, "login_aliyunid_csrf");
    if (!csrf) csrf = cookie_value(cookie, "csrf");
    CodexBarHttpRequestHeader headers[8] = {
        {"Content-Type", "application/x-www-form-urlencoded"},
        {"Accept", "application/json, text/plain, */*"},
        {"Cookie", cookie},
        {"Origin", endpoints->dashboard_origin},
        {"Referer", endpoints->dashboard_url},
        {"X-Requested-With", "XMLHttpRequest"},
    };
    size_t header_count = 6;
    if (csrf) {
        headers[header_count++] = (CodexBarHttpRequestHeader){"x-xsrf-token", csrf};
        headers[header_count++] = (CodexBarHttpRequestHeader){"x-csrf-token", csrf};
    }
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "POST",
        .headers = headers,
        .header_count = header_count,
        .body = body,
        .body_length = strlen(body),
        .timeout_seconds = 30,
        .maximum_response_bytes = QWEN_CLOUD_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = transport(&request, error);
    g_free(csrf);
    g_free(body);
    g_free(url);
    if (!response) {
        if ((!error || !*error) && (!cancellable || !g_cancellable_set_error_if_cancelled(cancellable, error))) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Qwen Cloud network request failed.");
        }
        return NULL;
    }
    if (response->status == 401 || response->status == 403) {
        codexbar_http_response_free(response);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Qwen Cloud rejected the stored session.");
        return NULL;
    }
    if (response->status != 200) {
        long status = response->status;
        codexbar_http_response_free(response);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Qwen Cloud usage API error: HTTP %ld.", status);
        return NULL;
    }
    char *text = response_text(response, error);
    codexbar_http_response_free(response);
    return text;
}

CodexBarProvider *codexbar_qwen_cloud_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarQwenCloudTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    char *cookie = resolve_cookie(config, error);
    if (!cookie) return NULL;
    QwenCloudEndpoints endpoints = {0};
    if (!resolve_endpoints(&endpoints, error)) {
        g_free(cookie);
        return NULL;
    }
    char *sec_token = resolve_sec_token(&endpoints, cookie, transport, cancellable, error);
    if (!sec_token) {
        endpoints_clear(&endpoints);
        g_free(cookie);
        return NULL;
    }
    char *usage = fetch_api(&endpoints,
                            QWEN_CLOUD_USAGE_API,
                            FALSE,
                            cookie,
                            sec_token,
                            transport,
                            cancellable,
                            error);
    if (!usage) {
        g_free(sec_token);
        endpoints_clear(&endpoints);
        g_free(cookie);
        return NULL;
    }

    GError *optional_error = NULL;
    char *subscription = fetch_api(&endpoints,
                                   QWEN_CLOUD_SUBSCRIPTION_API,
                                   TRUE,
                                   cookie,
                                   sec_token,
                                   transport,
                                   cancellable,
                                   &optional_error);
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        g_clear_error(&optional_error);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        g_free(subscription);
        g_free(usage);
        g_free(sec_token);
        endpoints_clear(&endpoints);
        g_free(cookie);
        return NULL;
    }
    g_clear_error(&optional_error);
    char *quota = fetch_api(&endpoints,
                            QWEN_CLOUD_QUOTA_CONFIG_API,
                            FALSE,
                            cookie,
                            sec_token,
                            transport,
                            cancellable,
                            &optional_error);
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        g_clear_error(&optional_error);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        g_free(quota);
        g_free(subscription);
        g_free(usage);
        g_free(sec_token);
        endpoints_clear(&endpoints);
        g_free(cookie);
        return NULL;
    }
    g_clear_error(&optional_error);

    CodexBarProvider *provider =
        codexbar_qwen_cloud_parse_usage(usage, subscription, quota, now_ms, error);
    g_free(quota);
    g_free(subscription);
    g_free(usage);
    g_free(sec_token);
    endpoints_clear(&endpoints);
    g_free(cookie);
    return provider;
}

CodexBarProvider *codexbar_qwen_cloud_fetch_with_transport(const CodexBarProviderConfig *config,
                                                           CodexBarQwenCloudTransport transport,
                                                           gint64 now_ms,
                                                           GError **error) {
    return codexbar_qwen_cloud_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_qwen_cloud_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                             GCancellable *cancellable,
                                                             GError **error) {
    return codexbar_qwen_cloud_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_qwen_cloud_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_qwen_cloud_fetch_with_cancellable(config, NULL, error);
}
