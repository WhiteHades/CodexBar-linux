#include "model.h"

#include <json-c/json.h>

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

static void parse_rate_window(json_object *usage, const char *key, CodexBarRateWindow *window) {
    json_object *object = NULL;
    json_object *used_percent = NULL;
    if (!usage || !json_object_object_get_ex(usage, key, &object) || !json_object_is_type(object, json_type_object)) {
        return;
    }
    if (!json_object_object_get_ex(object, "usedPercent", &used_percent) ||
        (!json_object_is_type(used_percent, json_type_double) &&
         !json_object_is_type(used_percent, json_type_int))) {
        return;
    }

    window->available = TRUE;
    window->label = duplicate_json_string(object, "label");
    window->used_percent = CLAMP(json_object_get_double(used_percent), 0.0, 100.0);
    window->reset_description = duplicate_json_string(object, "resetDescription");
    window->resets_at = duplicate_json_string(object, "resetsAt");
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
    g_free(provider->primary.label);
    g_free(provider->primary.reset_description);
    g_free(provider->primary.resets_at);
    g_free(provider->secondary.label);
    g_free(provider->secondary.reset_description);
    g_free(provider->secondary.resets_at);
    g_free(provider->tertiary.label);
    g_free(provider->tertiary.reset_description);
    g_free(provider->tertiary.resets_at);
    g_free(provider->credits_label);
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

        CodexBarProvider *provider = g_new0(CodexBarProvider, 1);
        provider->provider = duplicate_json_string(payload, "provider");
        provider->account = duplicate_json_string(payload, "account");
        provider->plan = duplicate_json_string(payload, "plan");
        provider->source = duplicate_json_string(payload, "source");
        provider->note = duplicate_json_string(payload, "note");
        provider->error = parse_error(payload);

        json_object *usage = NULL;
        if (json_object_object_get_ex(payload, "usage", &usage) && json_object_is_type(usage, json_type_object)) {
            parse_rate_window(usage, "primary", &provider->primary);
            parse_rate_window(usage, "secondary", &provider->secondary);
            parse_rate_window(usage, "tertiary", &provider->tertiary);
        }

        json_object *credits = NULL;
        json_object *remaining = NULL;
        if (json_object_object_get_ex(payload, "credits", &credits) &&
            json_object_is_type(credits, json_type_object) &&
            json_object_object_get_ex(credits, "remaining", &remaining) &&
            (json_object_is_type(remaining, json_type_double) || json_object_is_type(remaining, json_type_int))) {
            provider->has_credits = TRUE;
            provider->credits_label = duplicate_json_string(credits, "label");
            provider->credits_remaining = json_object_get_double(remaining);
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
        const CodexBarRateWindow *windows[] = {&provider->primary, &provider->secondary, &provider->tertiary};
        for (size_t window_index = 0; window_index < G_N_ELEMENTS(windows); window_index++) {
            if (windows[window_index]->available) {
                highest = MAX(highest, windows[window_index]->used_percent);
            }
        }
    }
    return highest;
}
