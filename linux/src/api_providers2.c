#include "api_providers2.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>
#include <sys/utsname.h>

#define SYNTHETIC_URL "https://api.synthetic.new/v2/quotas"
#define WARP_URL "https://app.warp.dev/graphql/v2?op=GetRequestLimitInfo"
#define GROQ_DEFAULT_BASE "https://api.groq.com/v1"
#define PROVIDER_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)

static const char warp_query[] =
    "query GetRequestLimitInfo($requestContext: RequestContext!) {"
    " user(requestContext: $requestContext) { __typename ... on UserOutput { user {"
    " requestLimitInfo { isUnlimited nextRefreshTime requestLimit requestsUsedSinceLastRefresh }"
    " bonusGrants { requestCreditsGranted requestCreditsRemaining expiration }"
    " workspaces { bonusGrantsInfo { grants { requestCreditsGranted requestCreditsRemaining expiration } } }"
    " } } } }";

static gboolean json_whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

static json_object *parse_json_document(const char *json, size_t length) {
    if (!json || length > G_MAXINT || !g_utf8_validate(json, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
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

static char *clean_api_key(const char *raw) {
    if (!raw || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *key = g_strdup(raw);
    strip_unicode_whitespace(key);
    size_t length = strlen(key);
    if (length >= 2 && ((key[0] == '\'' && key[length - 1] == '\'') ||
                        (key[0] == '"' && key[length - 1] == '"'))) {
        key[length - 1] = '\0';
        memmove(key, key + 1, length - 1);
        strip_unicode_whitespace(key);
    }
    for (const char *cursor = key; *cursor; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        if (g_unichar_isspace(character) || g_unichar_iscntrl(character)) {
            g_free(key);
            return NULL;
        }
    }
    if (key[0] != '\0') return key;
    g_free(key);
    return NULL;
}

static char *resolve_api_key(const CodexBarProviderConfig *config, const char *const *environment_keys) {
    char *key = clean_api_key(config ? config->api_key : NULL);
    for (size_t index = 0; !key && environment_keys[index]; index++) {
        key = clean_api_key(g_getenv(environment_keys[index]));
    }
    return key;
}

static gboolean has_api_key(const CodexBarProviderConfig *config, const char *const *environment_keys) {
    char *key = resolve_api_key(config, environment_keys);
    gboolean present = key != NULL;
    g_free(key);
    return present;
}

static const char *const synthetic_environment_keys[] = {"SYNTHETIC_API_KEY", NULL};
static const char *const warp_environment_keys[] = {"WARP_API_KEY", "WARP_TOKEN", NULL};
static const char *const groq_environment_keys[] = {"GROQ_API_KEY", NULL};

gboolean codexbar_synthetic_has_api_key(const CodexBarProviderConfig *config) {
    return has_api_key(config, synthetic_environment_keys);
}

gboolean codexbar_warp_has_api_key(const CodexBarProviderConfig *config) {
    return has_api_key(config, warp_environment_keys);
}

gboolean codexbar_groq_has_api_key(const CodexBarProviderConfig *config) {
    return has_api_key(config, groq_environment_keys);
}

static gboolean check_cancelled(GCancellable *cancellable, GError **error) {
    return cancellable && g_cancellable_set_error_if_cancelled(cancellable, error);
}

static CodexBarHttpResponse *send_request(const CodexBarHttpRequest *request,
                                          CodexBarApiProviders2Transport transport,
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

static gboolean json_double_value(json_object *value, double *result) {
    if (!value || json_object_is_type(value, json_type_null) || json_object_is_type(value, json_type_boolean)) {
        return FALSE;
    }
    double parsed;
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        parsed = json_object_get_double(value);
    } else if (json_object_is_type(value, json_type_string)) {
        const char *text = json_object_get_string(value);
        size_t length = (size_t)json_object_get_string_len(value);
        if (!text || memchr(text, '\0', length)) return FALSE;
        char *clean = g_strndup(text, length);
        g_strstrip(clean);
        char *end = NULL;
        parsed = g_ascii_strtod(clean, &end);
        gboolean valid = clean[0] != '\0' && end && *end == '\0' && isfinite(parsed);
        g_free(clean);
        if (!valid) return FALSE;
    } else {
        return FALSE;
    }
    if (!isfinite(parsed)) return FALSE;
    *result = parsed;
    return TRUE;
}

static gboolean json_int_value(json_object *value, gint64 *result) {
    double number;
    if (!json_double_value(value, &number) || number < -9223372036854775808.0 ||
        number >= 9223372036854775808.0 ||
        trunc(number) != number) {
        return FALSE;
    }
    *result = (gint64)number;
    return TRUE;
}

static char *json_clean_string(json_object *object, const char *key) {
    json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!text || memchr(text, '\0', length)) return NULL;
    char *clean = g_strndup(text, length);
    g_strstrip(clean);
    if (clean[0] != '\0') return clean;
    g_free(clean);
    return NULL;
}

static gboolean object_first_double(json_object *object,
                                    const char *const *keys,
                                    double *result) {
    for (size_t index = 0; keys[index]; index++) {
        json_object *value = NULL;
        if (json_object_object_get_ex(object, keys[index], &value) && json_double_value(value, result)) return TRUE;
    }
    return FALSE;
}

static gboolean object_first_currency(json_object *object,
                                      const char *const *keys,
                                      double *result) {
    for (size_t index = 0; keys[index]; index++) {
        json_object *value = NULL;
        if (!json_object_object_get_ex(object, keys[index], &value)) continue;
        if (json_double_value(value, result)) return TRUE;
        if (!json_object_is_type(value, json_type_string)) continue;
        const char *text = json_object_get_string(value);
        size_t length = (size_t)json_object_get_string_len(value);
        if (!text || memchr(text, '\0', length)) continue;
        GString *clean = g_string_sized_new(length);
        for (size_t cursor = 0; cursor < length; cursor++) {
            if (text[cursor] != '$' && text[cursor] != ',') g_string_append_c(clean, text[cursor]);
        }
        g_strstrip(clean->str);
        char *end = NULL;
        double parsed = g_ascii_strtod(clean->str, &end);
        gboolean valid = clean->str[0] != '\0' && end && *end == '\0' && isfinite(parsed);
        g_string_free(clean, TRUE);
        if (valid) {
            *result = parsed;
            return TRUE;
        }
    }
    return FALSE;
}

static char *object_first_string(json_object *object, const char *const *keys) {
    for (size_t index = 0; keys[index]; index++) {
        char *value = json_clean_string(object, keys[index]);
        if (value) return value;
    }
    return NULL;
}

static gboolean parse_timestamp(json_object *value, gint64 *timestamp_ms) {
    double number;
    if (json_double_value(value, &number)) {
        if (number > 1000000000000.0) number /= 1000.0;
        if (number <= 1000000000.0 || number > (double)G_MAXINT64 / 1000.0) return FALSE;
        *timestamp_ms = (gint64)llround(number * 1000.0);
        return TRUE;
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!text || memchr(text, '\0', length)) return FALSE;
    char *copy = g_strndup(text, length);
    GDateTime *time = g_date_time_new_from_iso8601(copy, NULL);
    g_free(copy);
    if (!time) return FALSE;
    *timestamp_ms = g_date_time_to_unix(time) * 1000 + g_date_time_get_microsecond(time) / 1000;
    g_date_time_unref(time);
    return TRUE;
}

static gboolean object_first_timestamp(json_object *object,
                                       const char *const *keys,
                                       gint64 *timestamp_ms) {
    for (size_t index = 0; keys[index]; index++) {
        json_object *value = NULL;
        if (json_object_object_get_ex(object, keys[index], &value) && parse_timestamp(value, timestamp_ms)) {
            return TRUE;
        }
    }
    return FALSE;
}

static CodexBarProvider *new_provider(const char *provider_id, gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup(provider_id);
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));
    return provider;
}

/* Synthetic */

static const char *const synthetic_plan_keys[] = {
    "plan", "planName", "plan_name", "subscription", "subscriptionPlan", "tier", "package", "packageName", NULL,
};
static const char *const synthetic_label_keys[] = {"name", "label", "type", "period", "scope", "title", "id", NULL};
static const char *const synthetic_percent_used_keys[] = {
    "percentUsed", "usedPercent", "usagePercent", "usage_percent", "used_percent", "percent_used", "percent", NULL,
};
static const char *const synthetic_percent_remaining_keys[] = {
    "percentRemaining", "remainingPercent", "remaining_percent", "percent_remaining", NULL,
};
static const char *const synthetic_limit_keys[] = {
    "limit", "messageLimit", "message_limit", "messages", "maxRequests", "max_requests", "requestLimit",
    "request_limit", "quota", "max", "total", "capacity", "allowance", NULL,
};
static const char *const synthetic_used_keys[] = {
    "used", "usage", "usedMessages", "used_messages", "messagesUsed", "messages_used", "requests", "requestCount",
    "request_count", "consumed", "spent", NULL,
};
static const char *const synthetic_remaining_keys[] = {"remaining", "left", "available", "balance", NULL};
static const char *const synthetic_reset_keys[] = {
    "resetAt", "reset_at", "resetsAt", "resets_at", "renewAt", "renew_at", "renewsAt", "renews_at", "nextTickAt",
    "next_tick_at", "nextRegenAt", "next_regen_at", "periodEnd", "period_end", "expiresAt", "expires_at", "endAt",
    "end_at", NULL,
};
static const char *const synthetic_cost_limit_keys[] = {"maxCredits", "max_credits", NULL};
static const char *const synthetic_cost_remaining_keys[] = {"remainingCredits", "remaining_credits", NULL};
static const char *const synthetic_cost_used_keys[] = {"usedCredits", "used_credits", NULL};
static const char *const synthetic_regen_keys[] = {"nextRegenCredits", "next_regen_credits", NULL};
static const char *const synthetic_window_minutes_keys[] = {
    "windowMinutes", "window_minutes", "periodMinutes", "period_minutes", NULL,
};
static const char *const synthetic_window_hours_keys[] = {
    "windowHours", "window_hours", "periodHours", "period_hours", NULL,
};
static const char *const synthetic_window_days_keys[] = {"windowDays", "window_days", "periodDays", "period_days", NULL};
static const char *const synthetic_window_seconds_keys[] = {
    "windowSeconds", "window_seconds", "periodSeconds", "period_seconds", NULL,
};
static const char *const synthetic_window_string_keys[] = {
    "window", "windowLabel", "window_label", "period", "periodLabel", "period_label", NULL,
};

static gboolean synthetic_is_quota(json_object *object) {
    if (!object || !json_object_is_type(object, json_type_object)) return FALSE;
    double ignored;
    return object_first_double(object, synthetic_limit_keys, &ignored) ||
           object_first_double(object, synthetic_used_keys, &ignored) ||
           object_first_double(object, synthetic_remaining_keys, &ignored) ||
           object_first_double(object, synthetic_percent_used_keys, &ignored) ||
           object_first_double(object, synthetic_percent_remaining_keys, &ignored);
}

static gboolean parse_duration(const char *raw, gint64 *minutes) {
    if (!raw) return FALSE;
    char *text = g_ascii_strdown(raw, -1);
    g_strstrip(text);
    for (char *cursor = text; *cursor;) {
        if (g_ascii_isspace(*cursor)) {
            memmove(cursor, cursor + 1, strlen(cursor));
        } else {
            cursor++;
        }
    }
    static const struct {
        const char *suffix;
        double multiplier;
    } units[] = {{"minutes", 1}, {"minute", 1}, {"hours", 60}, {"hour", 60}, {"days", 1440},
                 {"mins", 1},    {"min", 1},    {"hrs", 60},   {"day", 1440}, {"hr", 60},
                 {"m", 1},       {"h", 60},     {"d", 1440}};
    gboolean valid = FALSE;
    for (size_t index = 0; index < G_N_ELEMENTS(units); index++) {
        if (!g_str_has_suffix(text, units[index].suffix)) continue;
        size_t value_length = strlen(text) - strlen(units[index].suffix);
        char *value_text = g_strndup(text, value_length);
        char *end = NULL;
        double value = g_ascii_strtod(value_text, &end);
        double calculated = value * units[index].multiplier;
        valid = value_text[0] != '\0' && end && *end == '\0' && isfinite(calculated) && value > 0 &&
                calculated <= (double)G_MAXINT64;
        g_free(value_text);
        if (valid) *minutes = (gint64)llround(calculated);
        break;
    }
    g_free(text);
    return valid;
}

static gboolean synthetic_window_minutes(json_object *object, gint64 *minutes) {
    double value;
    if (object_first_double(object, synthetic_window_minutes_keys, &value)) {
        if (value < (double)G_MININT64 || value > (double)G_MAXINT64) return FALSE;
        *minutes = (gint64)llround(value);
        return TRUE;
    }
    if (object_first_double(object, synthetic_window_hours_keys, &value) && value <= (double)G_MAXINT64 / 60) {
        *minutes = (gint64)llround(value * 60);
        return TRUE;
    }
    if (object_first_double(object, synthetic_window_days_keys, &value) && value <= (double)G_MAXINT64 / 1440) {
        *minutes = (gint64)llround(value * 1440);
        return TRUE;
    }
    if (object_first_double(object, synthetic_window_seconds_keys, &value) && value <= (double)G_MAXINT64 * 60) {
        *minutes = (gint64)llround(value / 60);
        return TRUE;
    }
    char *text = object_first_string(object, synthetic_window_string_keys);
    gboolean parsed = parse_duration(text, minutes);
    g_free(text);
    return parsed;
}

static char *synthetic_window_description(gint64 minutes) {
    if (minutes <= 0) return NULL;
    if (minutes % 1440 == 0) {
        gint64 days = minutes / 1440;
        return g_strdup_printf("%" G_GINT64_FORMAT " day%s window", days, days == 1 ? "" : "s");
    }
    if (minutes % 60 == 0) {
        gint64 hours = minutes / 60;
        return g_strdup_printf("%" G_GINT64_FORMAT " hour%s window", hours, hours == 1 ? "" : "s");
    }
    return g_strdup_printf("%" G_GINT64_FORMAT " minute%s window", minutes, minutes == 1 ? "" : "s");
}

static gboolean synthetic_parse_quota(json_object *object,
                                      const char *id,
                                      const char *default_title,
                                      CodexBarQuotaWindow **window_out,
                                      CodexBarProviderCost **cost_out,
                                      gint64 now_ms) {
    double used_percent;
    if (object_first_double(object, synthetic_percent_used_keys, &used_percent)) {
        if (used_percent <= 1) used_percent *= 100;
    } else {
        double remaining_percent;
        if (object_first_double(object, synthetic_percent_remaining_keys, &remaining_percent)) {
            if (remaining_percent <= 1) remaining_percent *= 100;
            used_percent = 100 - remaining_percent;
        } else {
            double limit = 0, used = 0, remaining = 0;
            gboolean has_limit = object_first_double(object, synthetic_limit_keys, &limit);
            gboolean has_used = object_first_double(object, synthetic_used_keys, &used);
            gboolean has_remaining = object_first_double(object, synthetic_remaining_keys, &remaining);
            if (!has_limit && has_used && has_remaining) {
                limit = used + remaining;
                has_limit = isfinite(limit);
            }
            if (!has_used && has_limit && has_remaining) {
                used = limit - remaining;
                has_used = isfinite(used);
            }
            if (!has_limit || !has_used || limit <= 0) return FALSE;
            used_percent = used / limit * 100;
        }
    }
    if (!isfinite(used_percent)) return FALSE;
    used_percent = CLAMP(used_percent, 0, 100);
    char *title = object_first_string(object, synthetic_label_keys);
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title ? title : default_title);
    g_free(title);
    window->usage_known = TRUE;
    window->used_percent = used_percent;
    gint64 minutes;
    if (synthetic_window_minutes(object, &minutes)) {
        window->has_window_minutes = TRUE;
        window->window_minutes = minutes;
    }
    gint64 reset_ms;
    if (object_first_timestamp(object, synthetic_reset_keys, &reset_ms)) {
        window->has_resets_at = TRUE;
        window->resets_at_ms = reset_ms;
    } else if (window->has_window_minutes) {
        window->reset_description = synthetic_window_description(window->window_minutes);
    }

    double cost_limit;
    if (cost_out && !*cost_out && object_first_currency(object, synthetic_cost_limit_keys, &cost_limit)) {
        double cost_used;
        double cost_remaining;
        if (!object_first_currency(object, synthetic_cost_used_keys, &cost_used)) {
            cost_used = object_first_currency(object, synthetic_cost_remaining_keys, &cost_remaining)
                            ? MAX(0, cost_limit - cost_remaining)
                            : used_percent / 100 * cost_limit;
        }
        if (isfinite(cost_limit) && isfinite(cost_used)) {
            CodexBarProviderCost *cost = g_new0(CodexBarProviderCost, 1);
            cost->used = cost_used;
            cost->limit = cost_limit;
            cost->currency = g_strdup("USD");
            cost->period = g_strdup("Weekly");
            cost->has_updated_at = TRUE;
            cost->updated_at_ms = now_ms;
            if (window->has_resets_at) {
                cost->has_resets_at = TRUE;
                cost->resets_at_ms = window->resets_at_ms;
            }
            double regen;
            if (object_first_currency(object, synthetic_regen_keys, &regen)) {
                cost->has_next_regen = TRUE;
                cost->next_regen = regen;
            }
            *cost_out = cost;
        }
    }
    *window_out = window;
    return TRUE;
}

static void synthetic_collect_quotas(json_object *candidate, GPtrArray *objects, guint depth) {
    if (!candidate || depth > 16 || objects->len >= 128) return;
    if (json_object_is_type(candidate, json_type_object)) {
        if (synthetic_is_quota(candidate)) {
            g_ptr_array_add(objects, candidate);
            return;
        }
        json_object_object_foreach(candidate, key, value) {
            (void)key;
            synthetic_collect_quotas(value, objects, depth + 1);
        }
    } else if (json_object_is_type(candidate, json_type_array)) {
        size_t count = json_object_array_length(candidate);
        for (size_t index = 0; index < count; index++) {
            synthetic_collect_quotas(json_object_array_get_idx(candidate, index), objects, depth + 1);
        }
    }
}

static json_object *object_member_object(json_object *object, const char *key) {
    json_object *value = NULL;
    return object && json_object_object_get_ex(object, key, &value) &&
                   json_object_is_type(value, json_type_object)
               ? value
               : NULL;
}

CodexBarProvider *codexbar_synthetic_parse_usage_bytes(const char *json,
                                                       size_t length,
                                                       gint64 now_ms,
                                                       GError **error) {
    json_object *document = parse_json_document(json, length);
    if (!document || (!json_object_is_type(document, json_type_object) &&
                      !json_object_is_type(document, json_type_array))) {
        if (document) json_object_put(document);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Synthetic quota response is malformed");
        return NULL;
    }
    json_object *root = document;
    json_object *array_wrapper = NULL;
    if (json_object_is_type(document, json_type_array)) {
        array_wrapper = json_object_new_object();
        json_object_object_add(array_wrapper, "quotas", json_object_get(document));
        root = array_wrapper;
    }
    CodexBarProvider *provider = new_provider("synthetic", now_ms);
    provider->explicit_quota_slots = TRUE;
    char *plan = object_first_string(root, synthetic_plan_keys);
    json_object *data = object_member_object(root, "data");
    if (!plan && data) plan = object_first_string(data, synthetic_plan_keys);
    provider->identity->login_method = plan;

    json_object *rolling = object_member_object(root, "rollingFiveHourLimit");
    json_object *weekly = object_member_object(root, "weeklyTokenLimit");
    json_object *search = object_member_object(root, "search");
    json_object *hourly = object_member_object(search, "hourly");
    if (!rolling && data) rolling = object_member_object(data, "rollingFiveHourLimit");
    if (!weekly && data) weekly = object_member_object(data, "weeklyTokenLimit");
    if (!hourly && data) hourly = object_member_object(object_member_object(data, "search"), "hourly");
    gboolean known_shape = synthetic_is_quota(rolling) || synthetic_is_quota(weekly) || synthetic_is_quota(hourly);
    json_object *slots[] = {rolling, weekly, hourly};
    const char *ids[] = {"primary", "secondary", "tertiary"};
    const char *titles[] = {"Rolling five-hour limit", "Weekly token limit", "Search hourly"};
    guint added = 0;
    if (known_shape) {
        for (size_t index = 0; index < G_N_ELEMENTS(slots); index++) {
            CodexBarQuotaWindow *window = NULL;
            if (synthetic_is_quota(slots[index]) &&
                synthetic_parse_quota(slots[index], ids[index], titles[index], &window, &provider->provider_cost, now_ms)) {
                codexbar_provider_add_quota_window(provider, window);
                added++;
            }
        }
    } else {
        const char *candidate_keys[] = {
            "quotas", "quota", "limits", "usage", "entries", "subscription", "data", NULL,
        };
        GPtrArray *objects = g_ptr_array_new();
        for (size_t index = 0; candidate_keys[index] && objects->len == 0; index++) {
            json_object *candidate = NULL;
            if (json_object_object_get_ex(root, candidate_keys[index], &candidate)) {
                synthetic_collect_quotas(candidate, objects, 0);
            }
        }
        for (guint index = 0; index < MIN(objects->len, 3); index++) {
            CodexBarQuotaWindow *window = NULL;
            if (synthetic_parse_quota(g_ptr_array_index(objects, index),
                                      ids[index],
                                      titles[index],
                                      &window,
                                      &provider->provider_cost,
                                      now_ms)) {
                codexbar_provider_add_quota_window(provider, window);
                added++;
            }
        }
        g_ptr_array_unref(objects);
    }
    if (array_wrapper) json_object_put(array_wrapper);
    json_object_put(document);
    if (added > 0) return provider;
    codexbar_provider_free(provider);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Synthetic quota response is missing quota data");
    return NULL;
}

CodexBarProvider *codexbar_synthetic_parse_usage(const char *json, gint64 now_ms, GError **error) {
    return codexbar_synthetic_parse_usage_bytes(json, json ? strlen(json) : 0, now_ms, error);
}

/* Warp */

static gboolean warp_bool_value(json_object *value) {
    if (!value || json_object_is_type(value, json_type_null)) return FALSE;
    if (json_object_is_type(value, json_type_boolean)) return json_object_get_boolean(value);
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        return json_object_get_double(value) != 0;
    }
    if (json_object_is_type(value, json_type_string)) {
        const char *text = json_object_get_string(value);
        return g_ascii_strcasecmp(text, "true") == 0 || g_ascii_strcasecmp(text, "yes") == 0 ||
               g_str_equal(text, "1");
    }
    return FALSE;
}

static gint64 warp_int_member(json_object *object, const char *key) {
    json_object *value = NULL;
    gint64 result = 0;
    if (json_object_object_get_ex(object, key, &value)) json_int_value(value, &result);
    return result;
}

typedef struct {
    gint64 granted;
    gint64 remaining;
    gboolean has_expiration;
    gint64 expiration_ms;
} WarpGrant;

static void warp_add_grants(json_object *array, GArray *grants) {
    if (!array || !json_object_is_type(array, json_type_array)) return;
    size_t count = json_object_array_length(array);
    for (size_t index = 0; index < count; index++) {
        json_object *object = json_object_array_get_idx(array, index);
        if (!object || !json_object_is_type(object, json_type_object)) continue;
        WarpGrant grant = {.granted = warp_int_member(object, "requestCreditsGranted"),
                           .remaining = warp_int_member(object, "requestCreditsRemaining")};
        json_object *expiration = NULL;
        grant.has_expiration = json_object_object_get_ex(object, "expiration", &expiration) &&
                               parse_timestamp(expiration, &grant.expiration_ms);
        g_array_append_val(grants, grant);
    }
}

static char *warp_graphql_errors(json_object *root) {
    json_object *errors = NULL;
    if (!json_object_object_get_ex(root, "errors", &errors) || !json_object_is_type(errors, json_type_array) ||
        json_object_array_length(errors) == 0) {
        return NULL;
    }
    GString *summary = g_string_new(NULL);
    size_t count = MIN(json_object_array_length(errors), 3);
    for (size_t index = 0; index < count; index++) {
        json_object *entry = json_object_array_get_idx(errors, index);
        char *message = NULL;
        if (json_object_is_type(entry, json_type_string)) {
            const char *text = json_object_get_string(entry);
            size_t length = (size_t)json_object_get_string_len(entry);
            if (!memchr(text, '\0', length)) message = g_strndup(text, length);
        } else if (json_object_is_type(entry, json_type_object)) {
            message = json_clean_string(entry, "message");
        }
        if (message) {
            g_strstrip(message);
            if (message[0]) {
                if (summary->len) g_string_append(summary, " | ");
                g_string_append(summary, message);
            }
            g_free(message);
        }
    }
    if (summary->len == 0) g_string_assign(summary, "GraphQL request failed.");
    return g_string_free(summary, FALSE);
}

CodexBarProvider *codexbar_warp_parse_usage_bytes(const char *json,
                                                  size_t length,
                                                  gint64 now_ms,
                                                  GError **error) {
    json_object *root = parse_json_document(json, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Warp response root is not an object");
        return NULL;
    }
    char *graphql_error = warp_graphql_errors(root);
    if (graphql_error) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Warp GraphQL error: %s", graphql_error);
        g_free(graphql_error);
        json_object_put(root);
        return NULL;
    }
    json_object *data = object_member_object(root, "data");
    json_object *outer_user = object_member_object(data, "user");
    json_object *inner_user = object_member_object(outer_user, "user");
    json_object *limit_info = object_member_object(inner_user, "requestLimitInfo");
    if (!limit_info) {
        char *type_name = json_clean_string(outer_user, "__typename");
        if (type_name && !g_str_equal(type_name, "UserOutput")) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Warp response has unexpected user type '%s'", type_name);
        } else {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Warp response is missing data.user.user.requestLimitInfo");
        }
        g_free(type_name);
        json_object_put(root);
        return NULL;
    }
    json_object *unlimited_value = NULL;
    json_object_object_get_ex(limit_info, "isUnlimited", &unlimited_value);
    gboolean unlimited = warp_bool_value(unlimited_value);
    gint64 limit = warp_int_member(limit_info, "requestLimit");
    gint64 used = warp_int_member(limit_info, "requestsUsedSinceLastRefresh");
    CodexBarProvider *provider = new_provider("warp", now_ms);
    provider->explicit_quota_slots = TRUE;
    CodexBarQuotaWindow *primary = codexbar_quota_window_new("primary", "Credits");
    primary->usage_known = TRUE;
    primary->used_percent = unlimited || limit <= 0 ? 0 : CLAMP((double)used / (double)limit * 100, 0, 100);
    primary->reset_description = unlimited ? g_strdup("Unlimited")
                                           : g_strdup_printf("%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT " credits",
                                                             used,
                                                             limit);
    json_object *refresh = NULL;
    if (!unlimited && json_object_object_get_ex(limit_info, "nextRefreshTime", &refresh) &&
        parse_timestamp(refresh, &primary->resets_at_ms)) {
        primary->has_resets_at = TRUE;
    }
    codexbar_provider_add_quota_window(provider, primary);

    GArray *grants = g_array_new(FALSE, FALSE, sizeof(WarpGrant));
    json_object *user_grants = NULL;
    if (json_object_object_get_ex(inner_user, "bonusGrants", &user_grants)) warp_add_grants(user_grants, grants);
    json_object *workspaces = NULL;
    if (json_object_object_get_ex(inner_user, "workspaces", &workspaces) &&
        json_object_is_type(workspaces, json_type_array)) {
        size_t count = json_object_array_length(workspaces);
        for (size_t index = 0; index < count; index++) {
            json_object *workspace = json_object_array_get_idx(workspaces, index);
            json_object *info = object_member_object(workspace, "bonusGrantsInfo");
            json_object *workspace_grants = NULL;
            if (info && json_object_object_get_ex(info, "grants", &workspace_grants)) {
                warp_add_grants(workspace_grants, grants);
            }
        }
    }
    gint64 total = 0, remaining = 0, earliest = G_MAXINT64, earliest_remaining = 0;
    for (guint index = 0; index < grants->len; index++) {
        WarpGrant grant = g_array_index(grants, WarpGrant, index);
        if (!__builtin_add_overflow(total, grant.granted, &total) &&
            !__builtin_add_overflow(remaining, grant.remaining, &remaining)) {
            if (grant.remaining > 0 && grant.has_expiration) {
                gint64 second = grant.expiration_ms / 1000;
                if (second < earliest / 1000) {
                    earliest = grant.expiration_ms;
                    earliest_remaining = grant.remaining;
                } else if (second == earliest / 1000) {
                    __builtin_add_overflow(earliest_remaining, grant.remaining, &earliest_remaining);
                }
            }
        }
    }
    g_array_unref(grants);
    if (total > 0 || remaining > 0) {
        CodexBarQuotaWindow *secondary = codexbar_quota_window_new("secondary", "Bonus credits");
        secondary->usage_known = TRUE;
        secondary->used_percent = total > 0 ? CLAMP((double)(total - remaining) / (double)total * 100, 0, 100)
                                            : remaining > 0 ? 0 : 100;
        if (earliest != G_MAXINT64 && earliest_remaining > 0) {
            GDateTime *expiry = g_date_time_new_from_unix_utc(earliest / 1000);
            char *date = expiry ? g_date_time_format(expiry, "%Y-%m-%d %H:%M UTC") : NULL;
            if (date) {
                secondary->reset_description = g_strdup_printf("%" G_GINT64_FORMAT " credits expire on %s",
                                                               earliest_remaining,
                                                               date);
            }
            g_free(date);
            if (expiry) g_date_time_unref(expiry);
        }
        codexbar_provider_add_quota_window(provider, secondary);
    }
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_warp_parse_usage(const char *json, gint64 now_ms, GError **error) {
    return codexbar_warp_parse_usage_bytes(json, json ? strlen(json) : 0, now_ms, error);
}

/* Groq */

gboolean codexbar_groq_parse_scalar_bytes(const char *json, size_t length, double *value, GError **error) {
    g_return_val_if_fail(value != NULL, FALSE);
    json_object *root = parse_json_document(json, length);
    char *status = root ? json_clean_string(root, "status") : NULL;
    if (!root || !json_object_is_type(root, json_type_object) || !status) {
        if (root) json_object_put(root);
        g_free(status);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Groq metrics response is malformed");
        return FALSE;
    }
    if (!g_str_equal(status, "success")) {
        char *message = json_clean_string(root, "error");
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Groq metrics query failed: %s",
                    message ? message : "query failed");
        g_free(message);
        g_free(status);
        json_object_put(root);
        return FALSE;
    }
    g_free(status);
    json_object *data = NULL;
    json_object *results = NULL;
    if (json_object_object_get_ex(root, "data", &data) && !json_object_is_type(data, json_type_null)) {
        if (!json_object_is_type(data, json_type_object) ||
            !json_object_object_get_ex(data, "result", &results) ||
            !json_object_is_type(results, json_type_array)) {
            json_object_put(root);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Groq metrics response is malformed");
            return FALSE;
        }
    }
    double total = 0;
    if (results) {
        size_t count = json_object_array_length(results);
        for (size_t index = 0; index < count; index++) {
            json_object *series = json_object_array_get_idx(results, index);
            json_object *samples = NULL;
            if (!series || !json_object_is_type(series, json_type_object)) {
                json_object_put(root);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Groq metrics response is malformed");
                return FALSE;
            }
            if (!json_object_object_get_ex(series, "value", &samples) || json_object_is_type(samples, json_type_null)) {
                continue;
            }
            if (!json_object_is_type(samples, json_type_array)) {
                json_object_put(root);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Groq metrics response is malformed");
                return FALSE;
            }
            size_t sample_count = json_object_array_length(samples);
            double sample;
            if (sample_count > 0 && json_double_value(json_object_array_get_idx(samples, sample_count - 1), &sample)) {
                total += sample;
                if (!isfinite(total)) {
                    json_object_put(root);
                    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Groq metrics response is malformed");
                    return FALSE;
                }
            }
        }
    }
    json_object_put(root);
    *value = total;
    return TRUE;
}

gboolean codexbar_groq_parse_scalar(const char *json, double *value, GError **error) {
    return codexbar_groq_parse_scalar_bytes(json, json ? strlen(json) : 0, value, error);
}

static char *groq_format_decimal(double value) {
    if (value >= 100) return g_strdup_printf("%.0f", value);
    if (value >= 10) return g_strdup_printf("%.1f", value);
    return g_strdup_printf("%.2f", value);
}

static CodexBarProvider *groq_provider(double requests,
                                      double input_tokens,
                                      double output_tokens,
                                      double cache_hits,
                                      gint64 now_ms) {
    CodexBarProvider *provider = new_provider("groq", now_ms);
    provider->explicit_quota_slots = TRUE;
    provider->identity->login_method = g_strdup("Prometheus metrics");
    const char *ids[] = {"primary", "secondary", "tertiary"};
    const char *titles[] = {"Requests", "Tokens", "Prompt cache hits"};
    double values[] = {requests * 60, (input_tokens + output_tokens) * 60, cache_hits * 60};
    const char *units[] = {" req/min", " tok/min", " cache/min"};
    for (size_t index = 0; index < G_N_ELEMENTS(values); index++) {
        if (index == 2 && cache_hits <= 0) continue;
        CodexBarQuotaWindow *window = codexbar_quota_window_new(ids[index], titles[index]);
        window->usage_known = TRUE;
        window->used_percent = 0;
        window->has_window_minutes = TRUE;
        window->window_minutes = 5;
        char *number = groq_format_decimal(values[index]);
        window->reset_description = g_strconcat(number, units[index], NULL);
        g_free(number);
        codexbar_provider_add_quota_window(provider, window);
    }
    return provider;
}

CodexBarProvider *codexbar_groq_parse_usage(const char *requests_json,
                                            const char *input_tokens_json,
                                            const char *output_tokens_json,
                                            const char *cache_hits_json,
                                            gint64 now_ms,
                                            GError **error) {
    double requests, input_tokens, output_tokens, cache_hits;
    if (!codexbar_groq_parse_scalar(requests_json, &requests, error) ||
        !codexbar_groq_parse_scalar(input_tokens_json, &input_tokens, error) ||
        !codexbar_groq_parse_scalar(output_tokens_json, &output_tokens, error) ||
        !codexbar_groq_parse_scalar(cache_hits_json, &cache_hits, error)) {
        return NULL;
    }
    return groq_provider(requests, input_tokens, output_tokens, cache_hits, now_ms);
}

static char *groq_base_url(GError **error) {
    const char *raw = g_getenv("GROQ_API_URL");
    if (!raw || raw[0] == '\0') return g_strdup(GROQ_DEFAULT_BASE);
    char *normalized = codexbar_http_normalize_endpoint(raw, CODEXBAR_HTTP_HTTPS_ONLY, error);
    if (!normalized) return NULL;
    GUri *uri = g_uri_parse(normalized, G_URI_FLAGS_NONE, NULL);
    if (!uri || g_uri_get_query(uri) || g_uri_get_fragment(uri)) {
        if (uri) g_uri_unref(uri);
        g_free(normalized);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Groq endpoint override must be an HTTPS URL without a query or fragment");
        return NULL;
    }
    g_uri_unref(uri);
    size_t length = strlen(normalized);
    while (length > 0 && normalized[length - 1] == '/') normalized[--length] = '\0';
    return normalized;
}

static CodexBarProvider *fetch_single_json(const CodexBarProviderConfig *config,
                                           const char *const *environment_keys,
                                           const char *provider_name,
                                           const char *url,
                                           const char *method,
                                           const char *body,
                                           const CodexBarHttpRequestHeader *extra_headers,
                                           size_t extra_header_count,
                                           CodexBarApiProviders2Transport transport,
                                           GCancellable *cancellable,
                                           gint64 now_ms,
                                           CodexBarProvider *(*parser)(const char *, size_t, gint64, GError **),
                                           GError **error) {
    char *key = resolve_api_key(config, environment_keys);
    if (!key) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "%s API key is not configured", provider_name);
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", key);
    g_free(key);
    size_t header_count = extra_header_count + 2;
    CodexBarHttpRequestHeader *headers = g_new0(CodexBarHttpRequestHeader, header_count);
    headers[0] = (CodexBarHttpRequestHeader){"Authorization", authorization};
    headers[1] = (CodexBarHttpRequestHeader){"Accept", "application/json"};
    for (size_t index = 0; index < extra_header_count; index++) headers[index + 2] = extra_headers[index];
    CodexBarHttpRequest request = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = header_count,
        .body = body,
        .body_length = body ? strlen(body) : 0,
        .timeout_seconds = 15,
        .maximum_response_bytes = PROVIDER_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    g_free(headers);
    g_free(authorization);
    if (!response) return NULL;
    if (response->status != 200) {
        int code = response->status == 401 || response->status == 403 ? G_IO_ERROR_PERMISSION_DENIED
                                                                     : G_IO_ERROR_FAILED;
        g_set_error(error, G_IO_ERROR, code, "%s API returned HTTP %ld", provider_name, response->status);
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = parser(response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

CodexBarProvider *codexbar_synthetic_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders2Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    return fetch_single_json(config,
                             synthetic_environment_keys,
                             "Synthetic",
                             SYNTHETIC_URL,
                             "GET",
                             NULL,
                             NULL,
                             0,
                             transport,
                             cancellable,
                             now_ms,
                             codexbar_synthetic_parse_usage_bytes,
                             error);
}

static char *warp_request_body(const char *os_version) {
    json_object *root = json_object_new_object();
    json_object *variables = json_object_new_object();
    json_object *request_context = json_object_new_object();
    json_object *os_context = json_object_new_object();
    json_object_object_add(os_context, "category", json_object_new_string("Linux"));
    json_object_object_add(os_context, "name", json_object_new_string("Linux"));
    json_object_object_add(os_context, "version", json_object_new_string(os_version));
    json_object_object_add(request_context, "clientContext", json_object_new_object());
    json_object_object_add(request_context, "osContext", os_context);
    json_object_object_add(variables, "requestContext", request_context);
    json_object_object_add(root, "query", json_object_new_string(warp_query));
    json_object_object_add(root, "variables", variables);
    json_object_object_add(root, "operationName", json_object_new_string("GetRequestLimitInfo"));
    char *body = g_strdup(json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
    json_object_put(root);
    return body;
}

CodexBarProvider *codexbar_warp_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders2Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    struct utsname system_info;
    const char *os_version =
        uname(&system_info) == 0 && system_info.release[0] != '\0' ? system_info.release : "unknown";
    char *body = warp_request_body(os_version);
    const CodexBarHttpRequestHeader headers[] = {
        {"Content-Type", "application/json"}, {"x-warp-client-id", "warp-app"},
        {"x-warp-os-category", "Linux"},      {"x-warp-os-name", "Linux"},
        {"x-warp-os-version", os_version},    {"User-Agent", "Warp/1.0"},
    };
    CodexBarProvider *provider = fetch_single_json(config,
                                                   warp_environment_keys,
                                                   "Warp",
                                                   WARP_URL,
                                                   "POST",
                                                   body,
                                                   headers,
                                                   G_N_ELEMENTS(headers),
                                                   transport,
                                                   cancellable,
                                                   now_ms,
                                                   codexbar_warp_parse_usage_bytes,
                                                   error);
    g_free(body);
    return provider;
}

static CodexBarProvider *groq_fetch_internal(const CodexBarProviderConfig *config,
                                             CodexBarApiProviders2Transport transport,
                                             GCancellable *cancellable,
                                             gint64 now_ms,
                                             GError **error) {
    char *key = resolve_api_key(config, groq_environment_keys);
    if (!key) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Groq API key is not configured");
        return NULL;
    }
    char *base = groq_base_url(error);
    if (!base) {
        g_free(key);
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", key);
    g_free(key);
    const char *queries[] = {
        "sum(model_project_id_status_code:requests:rate5m)",
        "sum(model_project_id:tokens_in:rate5m)",
        "sum(model_project_id:tokens_out:rate5m)",
        "sum(model_project_id:prompt_cache_hits:rate5m)",
    };
    double values[G_N_ELEMENTS(queries)] = {0};
    for (size_t index = 0; index < G_N_ELEMENTS(queries); index++) {
        char *escaped = g_uri_escape_string(queries[index], NULL, FALSE);
        char *url = g_strdup_printf("%s/metrics/prometheus/api/v1/query?query=%s", base, escaped);
        g_free(escaped);
        const CodexBarHttpRequestHeader headers[] = {
            {"Authorization", authorization}, {"Accept", "application/json"},
        };
        CodexBarHttpRequest request = {
            .url = url,
            .method = "GET",
            .headers = headers,
            .header_count = G_N_ELEMENTS(headers),
            .timeout_seconds = 15,
            .maximum_response_bytes = PROVIDER_MAXIMUM_RESPONSE_BYTES,
            .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
            .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
            .cancellable = cancellable,
        };
        CodexBarHttpResponse *response = send_request(&request, transport, error);
        g_free(url);
        if (!response) {
            g_free(authorization);
            g_free(base);
            return NULL;
        }
        if (response->status < 200 || response->status >= 300) {
            int code = response->status == 401 || response->status == 403 ? G_IO_ERROR_PERMISSION_DENIED
                                                                         : G_IO_ERROR_FAILED;
            g_set_error(error, G_IO_ERROR, code, "Groq metrics API returned HTTP %ld", response->status);
            codexbar_http_response_free(response);
            g_free(authorization);
            g_free(base);
            return NULL;
        }
        gboolean parsed = codexbar_groq_parse_scalar_bytes(response->body, response->body_length, &values[index], error);
        codexbar_http_response_free(response);
        if (!parsed) {
            g_free(authorization);
            g_free(base);
            return NULL;
        }
    }
    g_free(authorization);
    g_free(base);
    return groq_provider(values[0], values[1], values[2], values[3], now_ms);
}

CodexBarProvider *codexbar_groq_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders2Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    return groq_fetch_internal(config, transport, cancellable, now_ms, error);
}

#define DEFINE_FETCH_WRAPPERS(name)                                                                                 \
    CodexBarProvider *codexbar_##name##_fetch_with_transport(const CodexBarProviderConfig *config,                  \
                                                             CodexBarApiProviders2Transport transport,              \
                                                             gint64 now_ms,                                         \
                                                             GError **error) {                                      \
        return codexbar_##name##_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);      \
    }                                                                                                               \
    CodexBarProvider *codexbar_##name##_fetch_with_cancellable(const CodexBarProviderConfig *config,                \
                                                               GCancellable *cancellable,                           \
                                                               GError **error) {                                    \
        return codexbar_##name##_fetch_with_transport_and_cancellable(                                               \
            config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);                              \
    }                                                                                                               \
    CodexBarProvider *codexbar_##name##_fetch(const CodexBarProviderConfig *config, GError **error) {               \
        return codexbar_##name##_fetch_with_cancellable(config, NULL, error);                                        \
    }

DEFINE_FETCH_WRAPPERS(synthetic)
DEFINE_FETCH_WRAPPERS(warp)
DEFINE_FETCH_WRAPPERS(groq)
