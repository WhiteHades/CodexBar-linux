#include "render.h"

#include <json-c/json.h>

static void append_window(GString *tooltip, const char *label, const CodexBarRateWindow *window) {
    if (!window->available) {
        return;
    }
    g_string_append_printf(tooltip, "\n  %-8s %3.0f%% used", label, window->used_percent);
    if (window->reset_description) {
        g_string_append_printf(tooltip, " · %s", window->reset_description);
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
        has_usage = has_usage || provider->primary.available || provider->secondary.available ||
                    provider->tertiary.available || provider->has_credits;
    }
    const char *class_name = has_error ? (has_usage ? "stale" : "error")
                                       : percentage >= 90 ? "critical"
                                                          : percentage >= 70 ? "warning" : "ok";
    char *text = has_error && !has_usage ? g_strdup("󰚩 !") : g_strdup_printf("󰚩 %d%%", percentage);
    GString *tooltip = g_string_new("CODEXBAR // USAGE");

    if (snapshot->providers->len == 0) {
        g_string_append(tooltip, "\n\nNo enabled provider returned data.");
    }

    for (guint index = 0; index < snapshot->providers->len; index++) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        g_string_append_printf(tooltip, "\n\n%s", provider->provider);
        if (provider->account) {
            g_string_append_printf(tooltip, " // %s", provider->account);
        }
        append_window(tooltip, "session", &provider->primary);
        append_window(tooltip, "weekly", &provider->secondary);
        append_window(tooltip, "extra", &provider->tertiary);
        if (provider->has_credits) {
            g_string_append_printf(tooltip, "\n  credits  %.2f", provider->credits_remaining);
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
