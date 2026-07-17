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
        int filled = (int)((CLAMP(window->used_percent, 0.0, 100.0) / 100.0) * 10.0 + 0.5);
        for (int index = 0; index < 10; index++) {
            g_string_append(tooltip, index < filled ? "█" : "░");
        }
        g_string_append_printf(tooltip,
                               "  %.0f%% used · %.0f%% left",
                               window->used_percent,
                               100.0 - window->used_percent);
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

    double highest = codexbar_snapshot_highest_used(snapshot);
    int percentage = (int)(highest + 0.5);
    gboolean has_error = FALSE;
    gboolean has_usage = FALSE;
    for (guint index = 0; index < snapshot->providers->len; index++) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        has_error = has_error || provider->error != NULL;
        has_usage = has_usage || provider->quota_windows->len > 0 || provider->balances->len > 0;
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
