#include "render.h"

#include <json-c/json.h>

static char *format_timestamp(gint64 timestamp_ms, const char *prefix) {
    GDateTime *utc = g_date_time_new_from_unix_utc(timestamp_ms / 1000);
    if (!utc) return NULL;
    GDateTime *local = g_date_time_to_local(utc);
    char *date = g_date_time_format(local, "%a, %b %d %Y at %H:%M");
    char *result = g_strdup_printf("%s %s", prefix, date);
    g_free(date);
    g_date_time_unref(local);
    g_date_time_unref(utc);
    return result;
}

static char *format_freshness(gint64 timestamp_ms) {
    gint64 delta_seconds = (g_get_real_time() / 1000 - timestamp_ms) / 1000;
    if (delta_seconds >= -60 && delta_seconds < 60) return g_strdup("updated just now");
    if (delta_seconds < 0) return format_timestamp(timestamp_ms, "updated");
    if (delta_seconds < 3600) return g_strdup_printf("updated %" G_GINT64_FORMAT "m ago", delta_seconds / 60);
    if (delta_seconds < 86400) return g_strdup_printf("updated %" G_GINT64_FORMAT "h ago", delta_seconds / 3600);
    return format_timestamp(timestamp_ms, "updated");
}

static char *format_count(gint64 value) {
    if (value >= 1000000000) return g_strdup_printf("%.2fB", value / 1000000000.0);
    if (value >= 1000000) return g_strdup_printf("%.2fM", value / 1000000.0);
    if (value >= 1000) return g_strdup_printf("%.0fK", value / 1000.0);
    return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

static char *format_money(double value, const char *currency) {
    return g_ascii_strcasecmp(currency, "USD") == 0 ? g_strdup_printf("$%.2f", value)
                                                     : g_strdup_printf("%.2f %s", value, currency);
}

static const char *status_label(CodexBarServiceStatusIndicator indicator) {
    switch (indicator) {
    case CODEXBAR_STATUS_NONE:
        return "Operational";
    case CODEXBAR_STATUS_MINOR:
        return "Partial outage";
    case CODEXBAR_STATUS_MAJOR:
        return "Major outage";
    case CODEXBAR_STATUS_CRITICAL:
        return "Critical issue";
    case CODEXBAR_STATUS_MAINTENANCE:
        return "Maintenance";
    case CODEXBAR_STATUS_UNKNOWN:
        return "Status unknown";
    }
    return "Status unknown";
}

static void append_window(GString *tooltip, const CodexBarQuotaWindow *window) {
    char *clipped_label = g_utf8_substring(window->title, 0, MIN(8, g_utf8_strlen(window->title, -1)));
    g_string_append_printf(tooltip, "\n  %-8s ", clipped_label);
    g_free(clipped_label);
    if (window->usage_known) {
        double used_percent =
            codexbar_usage_percent_display(codexbar_usage_percent_from_raw(window->used_percent));
        int filled = (int)((used_percent / 100.0) * 10.0 + 0.5);
        for (int index = 0; index < 10; index++) {
            g_string_append(tooltip, index < filled ? "█" : "░");
        }
        g_string_append_printf(tooltip,
                               "  %.0f%% used · %.0f%% left",
                               used_percent,
                               100.0 - used_percent);
    }
    if (window->detail) {
        g_string_append_printf(tooltip, "\n  %-8s %s", "", window->detail);
    }
    if (window->reset_description) {
        g_string_append_printf(tooltip, "\n  %-8s %s", "", window->reset_description);
    }
    if (window->has_resets_at) {
        char *reset = format_timestamp(window->resets_at_ms, "resets");
        if (reset) g_string_append_printf(tooltip, "\n  %-8s %s", "", reset);
        g_free(reset);
    }
    if (window->pace && window->pace->summary) {
        g_string_append_printf(tooltip, "\n  %-8s Pace: %s", "", window->pace->summary);
    }
}

static void append_balance(GString *tooltip, const CodexBarBalance *balance) {
    char *clipped_title = g_utf8_substring(balance->title, 0, MIN(8, g_utf8_strlen(balance->title, -1)));
    g_string_append_printf(
        tooltip, "\n  %-8s %.2f %s left", clipped_title, balance->remaining, balance->unit);
    g_free(clipped_title);
    if (balance->has_used || balance->has_limit) {
        g_string_append_printf(tooltip, "\n  %-8s ", "");
        if (balance->has_used) g_string_append_printf(tooltip, "%.2f used", balance->used);
        if (balance->has_used && balance->has_limit) g_string_append(tooltip, " · ");
        if (balance->has_limit) g_string_append_printf(tooltip, "%.2f limit", balance->limit);
    }
    if (balance->has_expiry) {
        char *expiry = format_timestamp(balance->expiry_ms, "expires");
        if (expiry) g_string_append_printf(tooltip, "\n  %-8s %s", "", expiry);
        g_free(expiry);
    }
    if (balance->has_resets_at) {
        char *reset = format_timestamp(balance->resets_at_ms, "resets");
        if (reset) g_string_append_printf(tooltip, "\n  %-8s %s", "", reset);
        g_free(reset);
    }
}

static void append_provider_cost(GString *tooltip, const CodexBarProviderCost *cost) {
    char *used = format_money(cost->used, cost->currency);
    char *limit = format_money(cost->limit, cost->currency);
    g_string_append_printf(tooltip, "\n  %s  %s", cost->limit > 0 ? "Extra usage" : "API spend", used);
    if (cost->limit > 0) g_string_append_printf(tooltip, " / %s", limit);
    g_free(used);
    g_free(limit);
    if (cost->period) g_string_append_printf(tooltip, " · %s", cost->period);
    if (cost->has_personal_used) {
        char *personal = format_money(cost->personal_used, cost->currency);
        g_string_append_printf(tooltip, "\n              %s personal used", personal);
        g_free(personal);
    }
    if (cost->has_next_regen) {
        char *regen = format_money(cost->next_regen, cost->currency);
        g_string_append_printf(tooltip, "\n              %s next regeneration", regen);
        g_free(regen);
    }
    if (cost->has_resets_at) {
        char *reset = format_timestamp(cost->resets_at_ms, "resets");
        if (reset) g_string_append_printf(tooltip, "\n              %s", reset);
        g_free(reset);
    }
}

static void append_token_cost_line(GString *tooltip,
                                   const char *label,
                                   gboolean has_tokens,
                                   gint64 tokens,
                                   gboolean has_cost,
                                   double cost,
                                   gboolean has_requests,
                                   gint64 requests,
                                   const char *currency) {
    g_string_append_printf(tooltip, "\n    %-12s", label);
    gboolean has_value = FALSE;
    if (has_cost) {
        char *money = format_money(cost, currency);
        g_string_append(tooltip, money);
        g_free(money);
        has_value = TRUE;
    }
    if (has_tokens) {
        char *count = format_count(tokens);
        g_string_append_printf(tooltip, "%s%s tokens", has_value ? " · " : "", count);
        g_free(count);
        has_value = TRUE;
    }
    if (has_requests) {
        g_string_append_printf(tooltip, "%s%" G_GINT64_FORMAT " requests", has_value ? " · " : "", requests);
    }
}

static void append_token_cost(GString *tooltip, const CodexBarTokenCost *cost) {
    g_string_append(tooltip, "\n  Cost");
    if (cost->has_today_tokens || cost->has_today_cost || cost->has_today_requests) {
        append_token_cost_line(tooltip,
                               "Today",
                               cost->has_today_tokens,
                               cost->today_tokens,
                               cost->has_today_cost,
                               cost->today_cost,
                               cost->has_today_requests,
                               cost->today_requests,
                               cost->currency);
    }
    char *fallback_label = cost->has_history_days
                               ? g_strdup_printf("Last %" G_GINT64_FORMAT " days", cost->history_days)
                               : g_strdup("Last 30 days");
    const char *history_label = cost->history_label ? cost->history_label : fallback_label;
    if (cost->has_last_days_tokens || cost->has_last_days_cost || cost->has_last_days_requests) {
        append_token_cost_line(tooltip,
                               history_label,
                               cost->has_last_days_tokens,
                               cost->last_days_tokens,
                               cost->has_last_days_cost,
                               cost->last_days_cost,
                               cost->has_last_days_requests,
                               cost->last_days_requests,
                               cost->currency);
    }
    g_free(fallback_label);
}

static char *serialize_waybar(const char *text, const char *tooltip, const char *class_name, int percentage) {
    json_object *object = json_object_new_object();
    json_object_object_add(object, "text", json_object_new_string(text));
    json_object_object_add(object, "tooltip", json_object_new_string(tooltip));
    json_object_object_add(object, "class", json_object_new_string(class_name));
    json_object_object_add(object, "percentage", json_object_new_int(percentage));
    char *result = g_strdup(json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN));
    json_object_put(object);
    return result;
}

char *codexbar_render_waybar(const CodexBarSnapshot *snapshot) {
    g_return_val_if_fail(snapshot != NULL, NULL);

    double highest = codexbar_usage_percent_display(
        codexbar_usage_percent_from_raw(codexbar_snapshot_highest_used(snapshot)));
    int percentage = (int)(highest + 0.5);
    gboolean has_error = FALSE;
    gboolean has_usage = FALSE;
    for (guint index = 0; index < snapshot->providers->len; index++) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        has_error = has_error || provider->error != NULL;
        has_usage = has_usage || provider->quota_windows->len > 0 || provider->balances->len > 0 ||
                    provider->provider_cost != NULL || provider->token_cost != NULL;
    }
    const char *class_name = has_error ? (has_usage ? "stale" : "error")
                                       : percentage >= 90 ? "critical"
                                                          : percentage >= 70 ? "warning" : "ok";
    char *text = has_error && !has_usage ? g_strdup("󰚩 !") : g_strdup_printf("󰚩 %d%%", percentage);
    GString *tooltip = g_string_new(NULL);

    if (snapshot->providers->len == 0) {
        g_string_append(tooltip, "No enabled provider returned data.");
    }

    for (guint index = 0; index < snapshot->providers->len; index++) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        if (index > 0) {
            g_string_append(tooltip, "\n\n");
        }
        g_string_append(tooltip, provider->provider);
        if (provider->account) {
            g_string_append_printf(tooltip, " · %s", provider->account);
        }
        if (provider->plan || provider->source) {
            g_string_append(tooltip, "\n  ");
            if (provider->plan) {
                g_string_append(tooltip, provider->plan);
            }
            if (provider->plan && provider->source) {
                g_string_append(tooltip, " · ");
            }
            if (provider->source) {
                g_string_append(tooltip, provider->source);
            }
        }
        if (provider->identity) {
            const char *values[] = {
                provider->identity->organization,
                provider->identity->account_id,
                provider->plan && g_strcmp0(provider->plan, provider->identity->login_method) == 0
                    ? NULL
                    : provider->identity->login_method,
            };
            for (size_t value_index = 0; value_index < G_N_ELEMENTS(values); value_index++) {
                if (values[value_index]) g_string_append_printf(tooltip, "\n  %s", values[value_index]);
            }
        }
        if (provider->has_updated_at) {
            char *updated = format_freshness(provider->updated_at_ms);
            if (updated) g_string_append_printf(tooltip, "\n  %s", updated);
            g_free(updated);
        }
        if (provider->has_subscription_expires_at) {
            char *expires = format_timestamp(provider->subscription_expires_at_ms, "subscription expires");
            if (expires) g_string_append_printf(tooltip, "\n  %s", expires);
            g_free(expires);
        }
        if (provider->has_subscription_renews_at) {
            char *renews = format_timestamp(provider->subscription_renews_at_ms, "subscription renews");
            if (renews) g_string_append_printf(tooltip, "\n  %s", renews);
            g_free(renews);
        }
        if (provider->note) {
            g_string_append_printf(tooltip, "\n  %s", provider->note);
        }
        for (guint window_index = 0; window_index < provider->quota_windows->len; window_index++) {
            append_window(tooltip, codexbar_provider_quota_window(provider, window_index));
        }
        for (guint balance_index = 0; balance_index < provider->balances->len; balance_index++) {
            append_balance(tooltip, codexbar_provider_balance(provider, balance_index));
        }
        if (provider->provider_cost) append_provider_cost(tooltip, provider->provider_cost);
        if (provider->token_cost) append_token_cost(tooltip, provider->token_cost);
        if (provider->status && provider->status->indicator != CODEXBAR_STATUS_NONE) {
            g_string_append_printf(tooltip, "\n  status    %s", status_label(provider->status->indicator));
            if (provider->status->description) {
                g_string_append_printf(tooltip, " · %s", provider->status->description);
            }
            g_string_append_printf(tooltip, "\n            %s", provider->status->url);
            if (provider->status->has_updated_at) {
                char *updated = format_freshness(provider->status->updated_at_ms);
                if (updated) g_string_append_printf(tooltip, "\n            %s", updated);
                g_free(updated);
            }
        }
        if (provider->error) {
            g_string_append_printf(tooltip, "\n  error    %s", provider->error);
        }
    }

    char *result = serialize_waybar(text, tooltip->str, class_name, percentage);
    g_free(text);
    g_string_free(tooltip, TRUE);
    return result;
}

char *codexbar_render_waybar_error(const char *message) {
    return serialize_waybar("󰚩 !", message ? message : "CodexBar backend failed", "error", 0);
}

static json_object *timestamp_json(gint64 timestamp_ms) {
    GDateTime *time = g_date_time_new_from_unix_utc(timestamp_ms / 1000);
    if (!time) return NULL;
    char *iso = g_date_time_format_iso8601(time);
    json_object *value = json_object_new_string(iso);
    g_free(iso);
    g_date_time_unref(time);
    return value;
}

static json_object *window_json(const CodexBarQuotaWindow *window) {
    json_object *object = json_object_new_object();
    json_object_object_add(object, "usedPercent", json_object_new_double(window->used_percent));
    if (window->has_window_minutes) {
        json_object_object_add(object, "windowMinutes", json_object_new_int64(window->window_minutes));
    }
    if (window->has_resets_at) json_object_object_add(object, "resetsAt", timestamp_json(window->resets_at_ms));
    const char *description = window->detail ? window->detail : window->reset_description;
    if (description) {
        json_object_object_add(object, "resetDescription", json_object_new_string(description));
    }
    return object;
}

static json_object *pace_json(const CodexBarPace *pace) {
    json_object *object = json_object_new_object();
    const char *stage = "unknown";
    switch (pace->stage) {
    case CODEXBAR_PACE_ON_TRACK: stage = "onTrack"; break;
    case CODEXBAR_PACE_SLIGHTLY_AHEAD: stage = "slightlyAhead"; break;
    case CODEXBAR_PACE_AHEAD: stage = "ahead"; break;
    case CODEXBAR_PACE_FAR_AHEAD: stage = "farAhead"; break;
    case CODEXBAR_PACE_SLIGHTLY_BEHIND: stage = "slightlyBehind"; break;
    case CODEXBAR_PACE_BEHIND: stage = "behind"; break;
    case CODEXBAR_PACE_FAR_BEHIND: stage = "farBehind"; break;
    case CODEXBAR_PACE_UNKNOWN: break;
    }
    json_object_object_add(object, "stage", json_object_new_string(stage));
    json_object_object_add(object, "deltaPercent", json_object_new_double(pace->delta_percent));
    json_object_object_add(object, "expectedUsedPercent", json_object_new_double(pace->expected_used_percent));
    json_object_object_add(object, "willLastToReset", json_object_new_boolean(pace->will_last));
    if (pace->has_eta) json_object_object_add(object, "etaSeconds", json_object_new_double(pace->eta_seconds));
    if (pace->has_runout_probability) {
        json_object_object_add(object, "runOutProbability", json_object_new_double(pace->runout_probability));
    }
    if (pace->summary) json_object_object_add(object, "summary", json_object_new_string(pace->summary));
    return object;
}

static const char *status_indicator_id(CodexBarServiceStatusIndicator indicator) {
    switch (indicator) {
    case CODEXBAR_STATUS_NONE: return "none";
    case CODEXBAR_STATUS_MINOR: return "minor";
    case CODEXBAR_STATUS_MAJOR: return "major";
    case CODEXBAR_STATUS_CRITICAL: return "critical";
    case CODEXBAR_STATUS_MAINTENANCE: return "maintenance";
    case CODEXBAR_STATUS_UNKNOWN: return "unknown";
    }
    return "unknown";
}

static json_object *provider_json(const CodexBarProvider *provider) {
    if (provider->raw) {
        return json_tokener_parse(json_object_to_json_string_ext(provider->raw, JSON_C_TO_STRING_PLAIN));
    }
    json_object *object = json_object_new_object();
    json_object_object_add(object, "provider", json_object_new_string(provider->provider));
    if (provider->account) json_object_object_add(object, "account", json_object_new_string(provider->account));
    if (provider->source) json_object_object_add(object, "source", json_object_new_string(provider->source));
    if (provider->status) {
        json_object *status = json_object_new_object();
        json_object_object_add(
            status, "indicator", json_object_new_string(status_indicator_id(provider->status->indicator)));
        if (provider->status->description) {
            json_object_object_add(
                status, "description", json_object_new_string(provider->status->description));
        }
        if (provider->status->url) {
            json_object_object_add(status, "url", json_object_new_string(provider->status->url));
        }
        if (provider->status->has_updated_at) {
            json_object_object_add(status, "updatedAt", timestamp_json(provider->status->updated_at_ms));
        }
        json_object_object_add(object, "status", status);
    }
    if (provider->error) {
        json_object *error = json_object_new_object();
        json_object_object_add(error, "message", json_object_new_string(provider->error));
        json_object_object_add(error, "code", json_object_new_int(provider->error_code ? provider->error_code : 1));
        json_object_object_add(
            error, "kind", json_object_new_string(provider->error_kind ? provider->error_kind : "runtime"));
        json_object_object_add(object, "error", error);
        return object;
    }

    json_object *usage = json_object_new_object();
    const char *keys[] = {"primary", "secondary", "tertiary"};
    gboolean explicit_slots = provider->explicit_quota_slots;
    for (guint index = 0; index < provider->quota_windows->len; index++) {
        const CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, index);
        explicit_slots = explicit_slots || g_str_equal(window->id, "primary") ||
                         g_str_equal(window->id, "secondary") || g_str_equal(window->id, "tertiary");
    }
    for (guint index = 0; index < G_N_ELEMENTS(keys); index++) {
        const CodexBarQuotaWindow *slot = NULL;
        if (explicit_slots) {
            for (guint window_index = 0; window_index < provider->quota_windows->len; window_index++) {
                const CodexBarQuotaWindow *candidate = codexbar_provider_quota_window(provider, window_index);
                if (g_str_equal(candidate->id, keys[index])) {
                    slot = candidate;
                    break;
                }
            }
        } else if (index < provider->quota_windows->len) {
            slot = codexbar_provider_quota_window(provider, index);
        }
        json_object_object_add(usage, keys[index], slot ? window_json(slot) : NULL);
    }
    guint extra_count = 0;
    for (guint index = 0; index < provider->quota_windows->len; index++) {
        const CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, index);
        gboolean canonical = g_str_equal(window->id, "primary") || g_str_equal(window->id, "secondary") ||
                             g_str_equal(window->id, "tertiary");
        if ((explicit_slots && !canonical) || (!explicit_slots && index >= G_N_ELEMENTS(keys))) extra_count++;
    }
    if (extra_count > 0) {
        json_object *extra = json_object_new_array();
        for (guint index = 0; index < provider->quota_windows->len; index++) {
            const CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, index);
            gboolean canonical = g_str_equal(window->id, "primary") || g_str_equal(window->id, "secondary") ||
                                 g_str_equal(window->id, "tertiary");
            if ((explicit_slots && canonical) || (!explicit_slots && index < G_N_ELEMENTS(keys))) continue;
            json_object *named = json_object_new_object();
            json_object_object_add(
                named, "id", json_object_new_string(window->output_id ? window->output_id : window->id));
            json_object_object_add(named, "title", json_object_new_string(window->title));
            json_object_object_add(named, "window", window_json(window));
            if (!window->usage_known) json_object_object_add(named, "usageKnown", json_object_new_boolean(FALSE));
            json_object_array_add(extra, named);
        }
        json_object_object_add(usage, "extraRateWindows", extra);
    }
    gint64 rendered_at_ms = provider->has_updated_at ? provider->updated_at_ms : g_get_real_time() / 1000;
    json_object_object_add(usage, "updatedAt", timestamp_json(rendered_at_ms));
    if (provider->has_subscription_expires_at) {
        json_object_object_add(usage, "subscriptionExpiresAt", timestamp_json(provider->subscription_expires_at_ms));
    }
    if (provider->has_subscription_renews_at) {
        json_object_object_add(usage, "subscriptionRenewsAt", timestamp_json(provider->subscription_renews_at_ms));
    }
    if (provider->identity || provider->plan) {
        json_object *identity = json_object_new_object();
        json_object_object_add(identity, "providerID", json_object_new_string(provider->provider));
        if (provider->account) json_object_object_add(identity, "accountEmail", json_object_new_string(provider->account));
        if (provider->identity && provider->identity->organization) {
            json_object_object_add(
                identity, "accountOrganization", json_object_new_string(provider->identity->organization));
        }
        if (provider->identity && provider->identity->account_id) {
            json_object_object_add(identity, "accountID", json_object_new_string(provider->identity->account_id));
        }
        const char *login_method = provider->identity && provider->identity->login_method
                                       ? provider->identity->login_method
                                       : provider->plan;
        if (login_method) {
            json_object_object_add(identity, "loginMethod", json_object_new_string(login_method));
        }
        json_object_object_add(usage, "identity", identity);
        if (provider->account) json_object_object_add(usage, "accountEmail", json_object_new_string(provider->account));
        if (provider->identity && provider->identity->organization) {
            json_object_object_add(
                usage, "accountOrganization", json_object_new_string(provider->identity->organization));
        }
        if (login_method) {
            json_object_object_add(usage, "loginMethod", json_object_new_string(login_method));
        }
    }
    if (provider->provider_cost) {
        const CodexBarProviderCost *cost = provider->provider_cost;
        json_object *value = json_object_new_object();
        json_object_object_add(value, "used", json_object_new_double(cost->used));
        json_object_object_add(value, "limit", json_object_new_double(cost->limit));
        json_object_object_add(value, "currencyCode", json_object_new_string(cost->currency));
        if (cost->period) json_object_object_add(value, "period", json_object_new_string(cost->period));
        if (cost->has_resets_at) json_object_object_add(value, "resetsAt", timestamp_json(cost->resets_at_ms));
        if (cost->has_updated_at) json_object_object_add(value, "updatedAt", timestamp_json(cost->updated_at_ms));
        if (cost->has_next_regen) {
            json_object_object_add(value, "nextRegenAmount", json_object_new_double(cost->next_regen));
        }
        if (cost->has_personal_used) {
            json_object_object_add(value, "personalUsed", json_object_new_double(cost->personal_used));
        }
        json_object_object_add(usage, "providerCost", value);
    }
    if (provider->usage_extensions) {
        json_object_object_foreach(provider->usage_extensions, key, value) {
            json_object_object_add(usage, key, json_object_get(value));
        }
    }
    json_object_object_add(object, "usage", usage);

    if (provider->balances->len > 0 && g_str_equal(provider->provider, "codex")) {
        const CodexBarBalance *credits_balance = NULL;
        const CodexBarBalance *limit_balance = NULL;
        for (guint index = 0; index < provider->balances->len; index++) {
            const CodexBarBalance *balance = codexbar_provider_balance(provider, index);
            if (g_str_equal(balance->id, "codex-credit-limit")) {
                limit_balance = balance;
            } else if (g_str_equal(balance->id, "credits")) {
                credits_balance = balance;
            }
        }
        if (!credits_balance) credits_balance = codexbar_provider_balance(provider, 0);
        if (!limit_balance && credits_balance->has_used && credits_balance->has_limit) {
            limit_balance = credits_balance;
        }
        gint64 credits_updated_at_ms = provider->has_credits_updated_at
                                          ? provider->credits_updated_at_ms
                                          : rendered_at_ms;
        json_object *credits = json_object_new_object();
        json_object_object_add(credits, "remaining", json_object_new_double(credits_balance->remaining));
        json_object_object_add(
            credits,
            "events",
            provider->credit_events ? json_object_get(provider->credit_events) : json_object_new_array());
        json_object_object_add(credits, "updatedAt", timestamp_json(credits_updated_at_ms));
        if (limit_balance && limit_balance->has_used && limit_balance->has_limit) {
            json_object *limit = json_object_new_object();
            json_object_object_add(limit, "title", json_object_new_string(limit_balance->title));
            json_object_object_add(limit, "used", json_object_new_double(limit_balance->used));
            json_object_object_add(limit, "limit", json_object_new_double(limit_balance->limit));
            json_object_object_add(limit, "remaining", json_object_new_double(limit_balance->remaining));
            json_object_object_add(
                limit,
                "remainingPercent",
                json_object_new_double(
                    limit_balance->has_remaining_percent
                        ? limit_balance->remaining_percent
                        : (limit_balance->limit > 0.0
                               ? limit_balance->remaining / limit_balance->limit * 100.0
                               : 0.0)));
            if (limit_balance->has_resets_at) {
                json_object_object_add(limit, "resetsAt", timestamp_json(limit_balance->resets_at_ms));
            }
            json_object_object_add(
                limit,
                "updatedAt",
                timestamp_json(
                    limit_balance->has_updated_at ? limit_balance->updated_at_ms : credits_updated_at_ms));
            json_object_object_add(credits, "codexCreditLimit", limit);
        }
        json_object_object_add(object, "credits", credits);
    }
    json_object *pace = json_object_new_object();
    gboolean has_pace = FALSE;
    for (guint index = 0; index < MIN(provider->quota_windows->len, 2); index++) {
        const CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, index);
        if (!window->pace) continue;
        json_object_object_add(pace, index == 0 ? "primary" : "secondary", pace_json(window->pace));
        has_pace = TRUE;
    }
    if (has_pace) {
        json_object_object_add(object, "pace", pace);
    } else {
        json_object_put(pace);
    }
    return object;
}

char *codexbar_render_usage_json(const CodexBarSnapshot *snapshot, gboolean pretty) {
    g_return_val_if_fail(snapshot != NULL, NULL);
    json_object *array = json_object_new_array_ext((int)snapshot->providers->len);
    for (guint index = 0; index < snapshot->providers->len; index++) {
        json_object_array_add(array, provider_json(g_ptr_array_index(snapshot->providers, index)));
    }
    char *result = g_strdup(json_object_to_json_string_ext(
        array, pretty ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
    json_object_put(array);
    return result;
}

char *codexbar_render_usage_text(const CodexBarSnapshot *snapshot) {
    g_return_val_if_fail(snapshot != NULL, NULL);
    GString *text = g_string_new(NULL);
    for (guint index = 0; index < snapshot->providers->len; index++) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        if (index > 0) g_string_append_c(text, '\n');
        g_string_append(text, provider->provider);
        if (provider->account) g_string_append_printf(text, " · %s", provider->account);
        if (provider->plan) g_string_append_printf(text, " · %s", provider->plan);
        if (provider->source) g_string_append_printf(text, " [%s]", provider->source);
        g_string_append_c(text, '\n');
        for (guint window_index = 0; window_index < provider->quota_windows->len; window_index++) {
            const CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, window_index);
            g_string_append_printf(text, "  %s: ", window->title);
            if (window->usage_known) {
                double display_percent =
                    codexbar_usage_percent_display(codexbar_usage_percent_from_raw(window->used_percent));
                g_string_append_printf(text, "%.0f%% used", display_percent);
            } else {
                g_string_append(text, "usage unavailable");
            }
            g_string_append_c(text, '\n');
        }
        for (guint balance_index = 0; balance_index < provider->balances->len; balance_index++) {
            const CodexBarBalance *balance = codexbar_provider_balance(provider, balance_index);
            g_string_append_printf(
                text, "  %s: %.2f %s left\n", balance->title, balance->remaining, balance->unit);
        }
        if (provider->provider_cost) {
            char *used = format_money(provider->provider_cost->used, provider->provider_cost->currency);
            g_string_append_printf(text,
                                   "  %s: %s",
                                   provider->provider_cost->limit > 0 ? "Extra usage" : "API spend",
                                   used);
            g_free(used);
            if (provider->provider_cost->limit > 0) {
                char *limit = format_money(provider->provider_cost->limit, provider->provider_cost->currency);
                g_string_append_printf(text, " / %s", limit);
                g_free(limit);
            }
            if (provider->provider_cost->period) {
                g_string_append_printf(text, " · %s", provider->provider_cost->period);
            }
            g_string_append_c(text, '\n');
        }
        if (provider->error) g_string_append_printf(text, "  error: %s\n", provider->error);
    }
    return g_string_free(text, FALSE);
}
