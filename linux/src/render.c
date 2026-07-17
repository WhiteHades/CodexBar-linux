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
        if (provider->note) {
            g_string_append_printf(tooltip, "\n  %s", provider->note);
        }
        for (guint window_index = 0; window_index < provider->quota_windows->len; window_index++) {
            append_window(tooltip, codexbar_provider_quota_window(provider, window_index));
        }
        for (guint balance_index = 0; balance_index < provider->balances->len; balance_index++) {
            append_balance(tooltip, codexbar_provider_balance(provider, balance_index));
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
