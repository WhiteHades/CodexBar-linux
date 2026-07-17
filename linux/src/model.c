#include "model.h"

#include <json-c/json.h>
#include <math.h>

static GQuark codexbar_model_error_quark(void) {
    return g_quark_from_static_string("codexbar-model-error");
}

static char *duplicate_json_string(json_object *object, const char *key) {
    json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) {
        return NULL;
    }
    if (!json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    return g_strdup(json_object_get_string(value));
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

void codexbar_quota_window_free(CodexBarQuotaWindow *window) {
    if (!window) return;
    g_free(window->id);
    g_free(window->title);
    g_free(window->detail);
    g_free(window->reset_description);
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
    if (has_usage) window->used_percent = CLAMP(used_percent, 0.0, 100.0);

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
    return balance;
}

static char *parse_error(json_object *payload) {
    json_object *error = NULL;
    if (!json_object_object_get_ex(payload, "error", &error) || !json_object_is_type(error, json_type_object)) {
        return NULL;
    }
    return duplicate_json_string(error, "message");
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
        provider->provider = duplicate_json_string(payload, "provider");
        provider->account = duplicate_json_string(payload, "account");
        provider->plan = duplicate_json_string(payload, "plan");
        provider->source = duplicate_json_string(payload, "source");
        provider->note = duplicate_json_string(payload, "note");
        provider->error = parse_error(payload);

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
        if (provider->balances->len == 0 && json_object_object_get_ex(payload, "credits", &credits)) {
            json_object *credit_limit = NULL;
            CodexBarBalance *balance =
                json_object_is_type(credits, json_type_object) &&
                        json_object_object_get_ex(credits, "codexCreditLimit", &credit_limit)
                    ? parse_balance(credit_limit, "codex-credit-limit", "monthly credit limit")
                    : parse_balance(credits, "credits", "credits");
            if (balance) codexbar_provider_add_balance(provider, balance);
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

double codexbar_snapshot_highest_used(const CodexBarSnapshot *snapshot) {
    g_return_val_if_fail(snapshot != NULL, 0.0);

    double highest = 0.0;
    for (guint index = 0; index < snapshot->providers->len; index++) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        for (guint window_index = 0; window_index < provider->quota_windows->len; window_index++) {
            const CodexBarQuotaWindow *window = g_ptr_array_index(provider->quota_windows, window_index);
            if (window->usage_known) highest = MAX(highest, window->used_percent);
        }
    }
    return highest;
}
