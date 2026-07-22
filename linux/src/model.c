#include "model.h"

#include <json-c/json.h>
#include <math.h>

static GQuark codexbar_model_error_quark(void) {
    return g_quark_from_static_string("codexbar-model-error");
}

CodexBarUsagePercent codexbar_usage_percent_from_raw(double raw) {
    return (CodexBarUsagePercent){.raw = raw};
}

CodexBarUsagePercent codexbar_usage_percent_from_ratio(double used, double limit) {
    g_return_val_if_fail(limit > 0.0, ((CodexBarUsagePercent){0}));
    return codexbar_usage_percent_from_raw(used / limit * 100.0);
}

double codexbar_usage_percent_display(CodexBarUsagePercent percent) {
    if (!(percent.raw > 0.0)) return 0.0;
    return MIN(percent.raw, 100.0);
}

static char *duplicate_json_string(json_object *object, const char *key) {
    json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) {
        return NULL;
    }
    if (!json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    const char *string = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (memchr(string, '\0', length)) return NULL;
    return g_strdup(string);
}

static gboolean parse_number(json_object *object, const char *key, double *result) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) ||
        (!json_object_is_type(value, json_type_double) && !json_object_is_type(value, json_type_int))) {
        return FALSE;
    }
    *result = json_object_get_double(value);
    return isfinite(*result);
}

static gboolean parse_integer(json_object *object, const char *key, gint64 *result) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value)) return FALSE;
    if (json_object_is_type(value, json_type_int)) {
        *result = json_object_get_int64(value);
        return TRUE;
    }
    if (!json_object_is_type(value, json_type_double)) return FALSE;
    double number = json_object_get_double(value);
    if (!isfinite(number) || number < (double)G_MININT64 || number >= (double)G_MAXINT64 ||
        trunc(number) != number) {
        return FALSE;
    }
    *result = (gint64)number;
    return TRUE;
}

static gboolean parse_boolean(json_object *object, const char *key, gboolean *result) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || !json_object_is_type(value, json_type_boolean)) {
        return FALSE;
    }
    *result = json_object_get_boolean(value);
    return TRUE;
}

static gboolean parse_timestamp_ms(json_object *object, const char *key, gint64 *result) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) {
        return FALSE;
    }
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        double timestamp = json_object_get_double(value);
        if (!isfinite(timestamp)) {
            return FALSE;
        }
        if (fabs(timestamp) < 100000000000.0) {
            timestamp *= 1000.0;
        }
        if (timestamp < (double)G_MININT64 || timestamp >= (double)G_MAXINT64) {
            return FALSE;
        }
        *result = (gint64)timestamp;
        return TRUE;
    }
    if (!json_object_is_type(value, json_type_string)) {
        return FALSE;
    }
    GDateTime *time = g_date_time_new_from_iso8601(json_object_get_string(value), NULL);
    if (!time) {
        return FALSE;
    }
    *result = g_date_time_to_unix(time) * 1000 + g_date_time_get_microsecond(time) / 1000;
    g_date_time_unref(time);
    return TRUE;
}

CodexBarQuotaWindow *codexbar_quota_window_new(const char *id, const char *title) {
    g_return_val_if_fail(id != NULL, NULL);
    g_return_val_if_fail(title != NULL, NULL);
    CodexBarQuotaWindow *window = g_new0(CodexBarQuotaWindow, 1);
    window->id = g_strdup(id);
    window->title = g_strdup(title);
    return window;
}

void codexbar_pace_free(CodexBarPace *pace) {
    if (!pace) return;
    g_free(pace->summary);
    g_free(pace);
}

void codexbar_quota_window_free(CodexBarQuotaWindow *window) {
    if (!window) return;
    g_free(window->id);
    g_free(window->output_id);
    g_free(window->title);
    g_free(window->detail);
    g_free(window->reset_description);
    codexbar_pace_free(window->pace);
    g_free(window);
}

CodexBarBalance *codexbar_balance_new(const char *id, const char *title, double remaining, const char *unit) {
    g_return_val_if_fail(id != NULL, NULL);
    g_return_val_if_fail(title != NULL, NULL);
    g_return_val_if_fail(unit != NULL, NULL);
    CodexBarBalance *balance = g_new0(CodexBarBalance, 1);
    balance->id = g_strdup(id);
    balance->title = g_strdup(title);
    balance->remaining = remaining;
    balance->unit = g_strdup(unit);
    return balance;
}

void codexbar_balance_free(CodexBarBalance *balance) {
    if (!balance) return;
    g_free(balance->id);
    g_free(balance->title);
    g_free(balance->unit);
    g_free(balance);
}

CodexBarProvider *codexbar_provider_new(void) {
    CodexBarProvider *provider = g_new0(CodexBarProvider, 1);
    provider->quota_windows = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_quota_window_free);
    provider->balances = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_balance_free);
    return provider;
}

void codexbar_provider_add_quota_window(CodexBarProvider *provider, CodexBarQuotaWindow *window) {
    g_return_if_fail(provider != NULL);
    g_return_if_fail(window != NULL);
    for (guint index = 0; index < provider->quota_windows->len; index++) {
        CodexBarQuotaWindow *existing = g_ptr_array_index(provider->quota_windows, index);
        if (g_str_equal(existing->id, window->id)) {
            provider->quota_windows->pdata[index] = window;
            codexbar_quota_window_free(existing);
            return;
        }
    }
    g_ptr_array_add(provider->quota_windows, window);
}

CodexBarQuotaWindow *codexbar_provider_quota_window(const CodexBarProvider *provider, guint index) {
    g_return_val_if_fail(provider != NULL, NULL);
    return index < provider->quota_windows->len ? g_ptr_array_index(provider->quota_windows, index) : NULL;
}

void codexbar_provider_add_balance(CodexBarProvider *provider, CodexBarBalance *balance) {
    g_return_if_fail(provider != NULL);
    g_return_if_fail(balance != NULL);
    for (guint index = 0; index < provider->balances->len; index++) {
        CodexBarBalance *existing = g_ptr_array_index(provider->balances, index);
        if (g_str_equal(existing->id, balance->id)) {
            provider->balances->pdata[index] = balance;
            codexbar_balance_free(existing);
            return;
        }
    }
    g_ptr_array_add(provider->balances, balance);
}

CodexBarBalance *codexbar_provider_balance(const CodexBarProvider *provider, guint index) {
    g_return_val_if_fail(provider != NULL, NULL);
    return index < provider->balances->len ? g_ptr_array_index(provider->balances, index) : NULL;
}

static CodexBarQuotaWindow *parse_quota_window(json_object *object, const char *fallback_id, const char *fallback_title) {
    if (!object || !json_object_is_type(object, json_type_object)) return NULL;
    char *id = duplicate_json_string(object, "id");
    char *title = duplicate_json_string(object, "title");
    if (!title) title = duplicate_json_string(object, "label");
    if ((!id && !fallback_id) || (!title && !fallback_title)) {
        g_free(id);
        g_free(title);
        return NULL;
    }
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id ? id : fallback_id, title ? title : fallback_title);
    g_free(id);
    g_free(title);
    if (!window) return NULL;

    json_object *usage_known = NULL;
    double used_percent = 0.0;
    gboolean has_usage = parse_number(object, "usedPercent", &used_percent);
    window->usage_known = has_usage;
    if (json_object_object_get_ex(object, "usageKnown", &usage_known) &&
        json_object_is_type(usage_known, json_type_boolean)) {
        window->usage_known = has_usage && json_object_get_boolean(usage_known);
    }
    if (has_usage) window->used_percent = codexbar_usage_percent_from_raw(used_percent).raw;

    double window_minutes = 0.0;
    if (parse_number(object, "windowMinutes", &window_minutes) ||
        parse_number(object, "windowDurationMins", &window_minutes)) {
        if (window_minutes >= 0.0 && window_minutes < (double)G_MAXINT64) {
            window->has_window_minutes = TRUE;
            window->window_minutes = (gint64)window_minutes;
        }
    }
    window->has_resets_at = parse_timestamp_ms(object, "resetsAt", &window->resets_at_ms);
    window->detail = duplicate_json_string(object, "detail");
    if (!window->detail) window->detail = duplicate_json_string(object, "resetDescription");
    if (!window->has_resets_at) {
        json_object *reset = NULL;
        if (json_object_object_get_ex(object, "resetsAt", &reset) && json_object_is_type(reset, json_type_string)) {
            window->reset_description = g_strdup(json_object_get_string(reset));
        }
    }
    char *description = duplicate_json_string(object, "resetDescription");
    if (description && window->detail && !g_str_equal(description, window->detail)) {
        window->reset_description = description;
    } else {
        g_free(description);
    }
    return window;
}

static CodexBarQuotaWindow *parse_named_rate_window(json_object *object) {
    if (!object || !json_object_is_type(object, json_type_object)) return NULL;
    char *id = duplicate_json_string(object, "id");
    char *title = duplicate_json_string(object, "title");
    json_object *inner = NULL;
    if (!id || !title || !json_object_object_get_ex(object, "window", &inner)) {
        g_free(id);
        g_free(title);
        return NULL;
    }
    CodexBarQuotaWindow *window = parse_quota_window(inner, id, title);
    g_free(id);
    g_free(title);
    if (!window) return NULL;
    json_object *usage_known = NULL;
    if (json_object_object_get_ex(object, "usageKnown", &usage_known) &&
        json_object_is_type(usage_known, json_type_boolean) && !json_object_get_boolean(usage_known)) {
        window->usage_known = FALSE;
    }
    return window;
}

static CodexBarBalance *parse_balance(json_object *object, const char *fallback_id, const char *fallback_title) {
    double remaining = 0.0;
    if (!object || !json_object_is_type(object, json_type_object) || !parse_number(object, "remaining", &remaining)) {
        return NULL;
    }
    char *id = duplicate_json_string(object, "id");
    char *title = duplicate_json_string(object, "title");
    if (!title) title = duplicate_json_string(object, "label");
    char *unit = duplicate_json_string(object, "unit");
    if ((!id && !fallback_id) || (!title && !fallback_title)) {
        g_free(id);
        g_free(title);
        g_free(unit);
        return NULL;
    }
    CodexBarBalance *balance = codexbar_balance_new(
        id ? id : fallback_id, title ? title : fallback_title, remaining, unit ? unit : "credits");
    g_free(id);
    g_free(title);
    g_free(unit);
    if (!balance) return NULL;
    balance->has_used = parse_number(object, "used", &balance->used);
    balance->has_limit = parse_number(object, "limit", &balance->limit);
    balance->has_expiry = parse_timestamp_ms(object, "expiry", &balance->expiry_ms) ||
                           parse_timestamp_ms(object, "expiresAt", &balance->expiry_ms);
    balance->has_resets_at = parse_timestamp_ms(object, "resetsAt", &balance->resets_at_ms);
    balance->has_updated_at = parse_timestamp_ms(object, "updatedAt", &balance->updated_at_ms);
    balance->has_remaining_percent = parse_number(object, "remainingPercent", &balance->remaining_percent);
    return balance;
}

static char *parse_error(json_object *payload) {
    json_object *error = NULL;
    if (!json_object_object_get_ex(payload, "error", &error) || !json_object_is_type(error, json_type_object)) {
        return NULL;
    }
    return duplicate_json_string(error, "message");
}

static void parse_error_metadata(json_object *payload, CodexBarProvider *provider) {
    json_object *error = NULL;
    if (!json_object_object_get_ex(payload, "error", &error) || !json_object_is_type(error, json_type_object)) return;
    json_object *code = NULL;
    if (json_object_object_get_ex(error, "code", &code) && json_object_is_type(code, json_type_int)) {
        provider->error_code = json_object_get_int(code);
    }
    provider->error_kind = duplicate_json_string(error, "kind");
}

static CodexBarServiceStatusIndicator parse_status_indicator(const char *indicator) {
    if (g_strcmp0(indicator, "none") == 0) return CODEXBAR_STATUS_NONE;
    if (g_strcmp0(indicator, "minor") == 0) return CODEXBAR_STATUS_MINOR;
    if (g_strcmp0(indicator, "major") == 0) return CODEXBAR_STATUS_MAJOR;
    if (g_strcmp0(indicator, "critical") == 0) return CODEXBAR_STATUS_CRITICAL;
    if (g_strcmp0(indicator, "maintenance") == 0) return CODEXBAR_STATUS_MAINTENANCE;
    return CODEXBAR_STATUS_UNKNOWN;
}

static CodexBarServiceStatus *parse_service_status(json_object *object) {
    if (!object || !json_object_is_type(object, json_type_object)) return NULL;
    char *indicator = duplicate_json_string(object, "indicator");
    char *url = duplicate_json_string(object, "url");
    if (!indicator || !url) {
        g_free(indicator);
        g_free(url);
        return NULL;
    }
    CodexBarServiceStatus *status = g_new0(CodexBarServiceStatus, 1);
    status->indicator = parse_status_indicator(indicator);
    status->description = duplicate_json_string(object, "description");
    status->url = url;
    status->has_updated_at = parse_timestamp_ms(object, "updatedAt", &status->updated_at_ms);
    g_free(indicator);
    return status;
}

static CodexBarProviderCost *parse_provider_cost(json_object *object) {
    if (!object || !json_object_is_type(object, json_type_object)) return NULL;
    double used = 0.0;
    double limit = 0.0;
    char *currency = duplicate_json_string(object, "currencyCode");
    if (!parse_number(object, "used", &used) || !parse_number(object, "limit", &limit) || !currency) {
        g_free(currency);
        return NULL;
    }
    CodexBarProviderCost *cost = g_new0(CodexBarProviderCost, 1);
    cost->used = used;
    cost->limit = limit;
    cost->currency = currency;
    cost->period = duplicate_json_string(object, "period");
    cost->has_resets_at = parse_timestamp_ms(object, "resetsAt", &cost->resets_at_ms);
    cost->has_next_regen = parse_number(object, "nextRegenAmount", &cost->next_regen);
    cost->has_personal_used = parse_number(object, "personalUsed", &cost->personal_used);
    cost->has_updated_at = parse_timestamp_ms(object, "updatedAt", &cost->updated_at_ms);
    return cost;
}

static CodexBarTokenCost *parse_token_cost(json_object *object) {
    if (!object || !json_object_is_type(object, json_type_object)) return NULL;
    CodexBarTokenCost *cost = g_new0(CodexBarTokenCost, 1);
    cost->has_today_tokens = parse_integer(object, "sessionTokens", &cost->today_tokens);
    cost->has_today_cost = parse_number(object, "sessionCostUSD", &cost->today_cost);
    cost->has_today_requests = parse_integer(object, "sessionRequests", &cost->today_requests);
    cost->has_last_days_tokens = parse_integer(object, "last30DaysTokens", &cost->last_days_tokens);
    cost->has_last_days_cost = parse_number(object, "last30DaysCostUSD", &cost->last_days_cost);
    cost->has_last_days_requests = parse_integer(object, "last30DaysRequests", &cost->last_days_requests);
    cost->currency = duplicate_json_string(object, "currencyCode");
    cost->history_label = duplicate_json_string(object, "historyLabel");
    cost->has_history_days = parse_integer(object, "historyDays", &cost->history_days);
    cost->has_updated_at = parse_timestamp_ms(object, "updatedAt", &cost->updated_at_ms);
    gboolean has_summary = cost->has_today_tokens || cost->has_today_cost || cost->has_today_requests ||
                           cost->has_last_days_tokens || cost->has_last_days_cost ||
                           cost->has_last_days_requests;
    if (!has_summary) {
        codexbar_token_cost_free(cost);
        return NULL;
    }
    if (!cost->currency) cost->currency = g_strdup("USD");
    return cost;
}

static CodexBarPaceStage parse_pace_stage(const char *stage) {
    if (g_strcmp0(stage, "onTrack") == 0) return CODEXBAR_PACE_ON_TRACK;
    if (g_strcmp0(stage, "slightlyAhead") == 0) return CODEXBAR_PACE_SLIGHTLY_AHEAD;
    if (g_strcmp0(stage, "ahead") == 0) return CODEXBAR_PACE_AHEAD;
    if (g_strcmp0(stage, "farAhead") == 0) return CODEXBAR_PACE_FAR_AHEAD;
    if (g_strcmp0(stage, "slightlyBehind") == 0) return CODEXBAR_PACE_SLIGHTLY_BEHIND;
    if (g_strcmp0(stage, "behind") == 0) return CODEXBAR_PACE_BEHIND;
    if (g_strcmp0(stage, "farBehind") == 0) return CODEXBAR_PACE_FAR_BEHIND;
    return CODEXBAR_PACE_UNKNOWN;
}

static CodexBarPace *parse_pace(json_object *object) {
    if (!object || !json_object_is_type(object, json_type_object)) return NULL;
    char *stage = duplicate_json_string(object, "stage");
    double delta = 0.0;
    double expected = 0.0;
    gboolean will_last = FALSE;
    if (!stage || !parse_number(object, "deltaPercent", &delta) ||
        !parse_number(object, "expectedUsedPercent", &expected) ||
        !parse_boolean(object, "willLastToReset", &will_last)) {
        g_free(stage);
        return NULL;
    }
    CodexBarPace *pace = g_new0(CodexBarPace, 1);
    pace->stage = parse_pace_stage(stage);
    pace->delta_percent = delta;
    pace->expected_used_percent = expected;
    pace->will_last = will_last;
    pace->has_eta = parse_number(object, "etaSeconds", &pace->eta_seconds);
    pace->has_runout_probability = parse_number(object, "runOutProbability", &pace->runout_probability);
    pace->summary = duplicate_json_string(object, "summary");
    g_free(stage);
    return pace;
}

static void apply_pace(CodexBarProvider *provider, json_object *object, const char *quota_id) {
    CodexBarPace *pace = parse_pace(object);
    if (!pace) return;
    for (guint index = 0; index < provider->quota_windows->len; index++) {
        CodexBarQuotaWindow *window = g_ptr_array_index(provider->quota_windows, index);
        if (g_str_equal(window->id, quota_id)) {
            codexbar_pace_free(window->pace);
            window->pace = pace;
            return;
        }
    }
    codexbar_pace_free(pace);
}

static CodexBarProviderIdentity *parse_identity(json_object *object, const char *provider_id, char **account) {
    if (!object || !json_object_is_type(object, json_type_object)) return NULL;
    char *identity_provider = duplicate_json_string(object, "providerID");
    if (!identity_provider || !g_str_equal(identity_provider, provider_id)) {
        g_free(identity_provider);
        return NULL;
    }
    g_free(identity_provider);
    if (!*account) *account = duplicate_json_string(object, "accountEmail");
    CodexBarProviderIdentity *identity = g_new0(CodexBarProviderIdentity, 1);
    identity->organization = duplicate_json_string(object, "accountOrganization");
    identity->account_id = duplicate_json_string(object, "accountID");
    identity->login_method = duplicate_json_string(object, "loginMethod");
    if (!identity->organization && !identity->account_id && !identity->login_method) {
        codexbar_provider_identity_free(identity);
        return NULL;
    }
    return identity;
}

void codexbar_service_status_free(CodexBarServiceStatus *status) {
    if (!status) return;
    g_free(status->description);
    g_free(status->url);
    g_free(status);
}

void codexbar_provider_cost_free(CodexBarProviderCost *cost) {
    if (!cost) return;
    g_free(cost->currency);
    g_free(cost->period);
    g_free(cost);
}

void codexbar_token_cost_free(CodexBarTokenCost *cost) {
    if (!cost) return;
    g_free(cost->currency);
    g_free(cost->history_label);
    g_free(cost);
}

void codexbar_provider_identity_free(CodexBarProviderIdentity *identity) {
    if (!identity) return;
    g_free(identity->organization);
    g_free(identity->account_id);
    g_free(identity->login_method);
    g_free(identity);
}

void codexbar_provider_free(CodexBarProvider *provider) {
    if (!provider) {
        return;
    }
    g_free(provider->provider);
    g_free(provider->account);
    g_free(provider->plan);
    g_free(provider->source);
    g_free(provider->note);
    g_free(provider->error);
    g_free(provider->error_kind);
    codexbar_provider_identity_free(provider->identity);
    codexbar_service_status_free(provider->status);
    codexbar_provider_cost_free(provider->provider_cost);
    codexbar_token_cost_free(provider->token_cost);
    if (provider->credit_events) json_object_put(provider->credit_events);
    if (provider->usage_extensions) json_object_put(provider->usage_extensions);
    if (provider->raw) json_object_put(provider->raw);
    g_ptr_array_unref(provider->quota_windows);
    g_ptr_array_unref(provider->balances);
    g_free(provider);
}

CodexBarSnapshot *codexbar_snapshot_parse(const char *json, GError **error) {
    g_return_val_if_fail(json != NULL, NULL);

    json_tokener *tokener = json_tokener_new();
    json_object *root = json_tokener_parse_ex(tokener, json, (int)strlen(json));
    enum json_tokener_error parse_error_code = json_tokener_get_error(tokener);
    if (parse_error_code != json_tokener_success || !root) {
        g_set_error(error, codexbar_model_error_quark(), 1, "Invalid backend JSON: %s",
                    json_tokener_error_desc(parse_error_code));
        json_tokener_free(tokener);
        if (root) {
            json_object_put(root);
        }
        return NULL;
    }
    json_tokener_free(tokener);

    if (!json_object_is_type(root, json_type_array)) {
        g_set_error_literal(error, codexbar_model_error_quark(), 2, "Backend JSON must be an array");
        json_object_put(root);
        return NULL;
    }

    CodexBarSnapshot *snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);

    size_t count = json_object_array_length(root);
    for (size_t index = 0; index < count; index++) {
        json_object *payload = json_object_array_get_idx(root, index);
        if (!json_object_is_type(payload, json_type_object)) {
            continue;
        }

        CodexBarProvider *provider = codexbar_provider_new();
        provider->raw = json_object_get(payload);
        provider->provider = duplicate_json_string(payload, "provider");
        provider->account = duplicate_json_string(payload, "account");
        provider->plan = duplicate_json_string(payload, "plan");
        provider->source = duplicate_json_string(payload, "source");
        provider->note = duplicate_json_string(payload, "note");
        provider->error = parse_error(payload);
        parse_error_metadata(payload, provider);

        json_object *status = NULL;
        if (json_object_object_get_ex(payload, "status", &status)) {
            provider->status = parse_service_status(status);
        }

        json_object *windows = NULL;
        gboolean has_canonical_windows = json_object_object_get_ex(payload, "quotaWindows", &windows) &&
                                         json_object_is_type(windows, json_type_array);
        if (has_canonical_windows) {
            size_t window_count = json_object_array_length(windows);
            for (size_t window_index = 0; window_index < window_count; window_index++) {
                CodexBarQuotaWindow *window = parse_quota_window(
                    json_object_array_get_idx(windows, window_index), NULL, NULL);
                if (window) codexbar_provider_add_quota_window(provider, window);
            }
        }

        json_object *usage = NULL;
        if (provider->quota_windows->len == 0 && json_object_object_get_ex(payload, "usage", &usage) &&
            json_object_is_type(usage, json_type_object)) {
            const char *ids[] = {"primary", "secondary", "tertiary"};
            const char *titles[] = {"session", "weekly", "extra"};
            for (size_t window_index = 0; window_index < G_N_ELEMENTS(ids); window_index++) {
                json_object *object = NULL;
                if (json_object_object_get_ex(usage, ids[window_index], &object)) {
                    CodexBarQuotaWindow *window = parse_quota_window(object, ids[window_index], titles[window_index]);
                    if (window && window->usage_known) {
                        codexbar_provider_add_quota_window(provider, window);
                    } else {
                        codexbar_quota_window_free(window);
                    }
                }
            }
            json_object *extra_windows = NULL;
            if (json_object_object_get_ex(usage, "extraRateWindows", &extra_windows) &&
                json_object_is_type(extra_windows, json_type_array)) {
                size_t extra_count = json_object_array_length(extra_windows);
                for (size_t extra_index = 0; extra_index < extra_count; extra_index++) {
                    CodexBarQuotaWindow *window = parse_named_rate_window(
                        json_object_array_get_idx(extra_windows, extra_index));
                    if (window) codexbar_provider_add_quota_window(provider, window);
                }
            }
        }

        if (!usage && json_object_object_get_ex(payload, "usage", &usage) &&
            !json_object_is_type(usage, json_type_object)) {
            usage = NULL;
        }
        if (usage) {
            provider->has_updated_at = parse_timestamp_ms(usage, "updatedAt", &provider->updated_at_ms);
            provider->has_subscription_expires_at = parse_timestamp_ms(
                usage, "subscriptionExpiresAt", &provider->subscription_expires_at_ms);
            provider->has_subscription_renews_at = parse_timestamp_ms(
                usage, "subscriptionRenewsAt", &provider->subscription_renews_at_ms);
            json_object *identity = NULL;
            if (provider->provider && json_object_object_get_ex(usage, "identity", &identity)) {
                provider->identity = parse_identity(identity, provider->provider, &provider->account);
                if (!provider->plan && provider->identity && provider->identity->login_method) {
                    provider->plan = g_strdup(provider->identity->login_method);
                }
            }
            json_object *provider_cost = NULL;
            if (json_object_object_get_ex(usage, "providerCost", &provider_cost)) {
                provider->provider_cost = parse_provider_cost(provider_cost);
            }
            json_object *data_confidence = NULL;
            if (json_object_object_get_ex(usage, "dataConfidence", &data_confidence) &&
                json_object_is_type(data_confidence, json_type_string)) {
                const char *candidate = json_object_get_string(data_confidence);
                size_t candidate_length = (size_t)json_object_get_string_len(data_confidence);
                if (!memchr(candidate, '\0', candidate_length) &&
                    (g_str_equal(candidate, "exact") || g_str_equal(candidate, "estimated") ||
                     g_str_equal(candidate, "percentOnly"))) {
                    provider->usage_extensions = json_object_new_object();
                    json_object_object_add(
                        provider->usage_extensions, "dataConfidence", json_object_new_string(candidate));
                }
            }
        }
        gint64 top_level_updated_at = 0;
        if (parse_timestamp_ms(payload, "updatedAt", &top_level_updated_at)) {
            provider->has_updated_at = TRUE;
            provider->updated_at_ms = top_level_updated_at;
        }

        json_object *pace = NULL;
        if (json_object_object_get_ex(payload, "pace", &pace) && json_object_is_type(pace, json_type_object)) {
            const char *pace_ids[] = {"primary", "secondary"};
            for (size_t pace_index = 0; pace_index < G_N_ELEMENTS(pace_ids); pace_index++) {
                json_object *pace_value = NULL;
                if (json_object_object_get_ex(pace, pace_ids[pace_index], &pace_value)) {
                    apply_pace(provider, pace_value, pace_ids[pace_index]);
                }
            }
        }

        json_object *token_cost = NULL;
        if (json_object_object_get_ex(payload, "tokenCost", &token_cost)) {
            provider->token_cost = parse_token_cost(token_cost);
        }

        json_object *balances = NULL;
        gboolean has_canonical_balances = json_object_object_get_ex(payload, "balances", &balances) &&
                                          json_object_is_type(balances, json_type_array);
        if (has_canonical_balances) {
            size_t balance_count = json_object_array_length(balances);
            for (size_t balance_index = 0; balance_index < balance_count; balance_index++) {
                CodexBarBalance *balance = parse_balance(
                    json_object_array_get_idx(balances, balance_index), NULL, NULL);
                if (balance) codexbar_provider_add_balance(provider, balance);
            }
        }

        json_object *credits = NULL;
        if (json_object_object_get_ex(payload, "credits", &credits) &&
            json_object_is_type(credits, json_type_object)) {
            provider->has_credits_updated_at = parse_timestamp_ms(
                credits, "updatedAt", &provider->credits_updated_at_ms);
            json_object *events = NULL;
            if (json_object_object_get_ex(credits, "events", &events) &&
                json_object_is_type(events, json_type_array)) {
                provider->credit_events = json_object_get(events);
            }
            if (provider->balances->len == 0) {
                CodexBarBalance *balance = parse_balance(credits, "credits", "credits");
                if (balance) codexbar_provider_add_balance(provider, balance);
                json_object *credit_limit = NULL;
                if (json_object_object_get_ex(credits, "codexCreditLimit", &credit_limit)) {
                    balance = parse_balance(credit_limit, "codex-credit-limit", "monthly credit limit");
                    if (balance) codexbar_provider_add_balance(provider, balance);
                }
            }
        }

        if (!provider->provider) {
            codexbar_provider_free(provider);
            continue;
        }
        g_ptr_array_add(snapshot->providers, provider);
    }

    json_object_put(root);
    return snapshot;
}

void codexbar_snapshot_free(CodexBarSnapshot *snapshot) {
    if (!snapshot) {
        return;
    }
    g_ptr_array_unref(snapshot->providers);
    g_free(snapshot);
}

double codexbar_provider_highest_used(const CodexBarProvider *provider) {
    g_return_val_if_fail(provider != NULL, 0.0);
    double highest = 0.0;
    for (guint index = 0; index < provider->quota_windows->len; index++) {
        const CodexBarQuotaWindow *window = g_ptr_array_index(provider->quota_windows, index);
        if (window->usage_known) highest = MAX(highest, window->used_percent);
    }
    return highest;
}

double codexbar_snapshot_highest_used(const CodexBarSnapshot *snapshot) {
    g_return_val_if_fail(snapshot != NULL, 0.0);

    double highest = 0.0;
    for (guint index = 0; index < snapshot->providers->len; index++) {
        highest = MAX(highest, codexbar_provider_highest_used(g_ptr_array_index(snapshot->providers, index)));
    }
    return highest;
}
