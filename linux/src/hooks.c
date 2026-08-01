#include "hooks.h"

#include "provider_registry.h"

#include <math.h>
#include <string.h>

static const char *const event_names[] = {
    "quota_low",
    "quota_reached",
    "quota_reset",
    "provider_unavailable",
    "provider_recovered",
    "refresh_failed",
};

gboolean codexbar_hook_event_is_known(const char *event) {
    for (guint index = 0; index < G_N_ELEMENTS(event_names); index++) {
        if (g_strcmp0(event, event_names[index]) == 0) return TRUE;
    }
    return FALSE;
}

gboolean codexbar_hooks_enabled(json_object *hooks) {
    json_object *enabled = NULL;
    return hooks && json_object_object_get_ex(hooks, "enabled", &enabled) &&
           json_object_is_type(enabled, json_type_boolean) && json_object_get_boolean(enabled);
}

json_object *codexbar_hook_events(json_object *hooks) {
    json_object *events = NULL;
    return hooks && json_object_object_get_ex(hooks, "events", &events) &&
                   json_object_is_type(events, json_type_array)
               ? events
               : NULL;
}

static gboolean json_string_value(json_object *object, const char *key, const char **value, size_t *length) {
    json_object *entry = NULL;
    if (!json_object_object_get_ex(object, key, &entry) || !json_object_is_type(entry, json_type_string)) return FALSE;
    *value = json_object_get_string(entry);
    *length = (size_t)json_object_get_string_len(entry);
    return strlen(*value) == *length;
}

static gboolean json_boolean_default(json_object *object, const char *key, gboolean fallback) {
    json_object *entry = NULL;
    return json_object_object_get_ex(object, key, &entry) && json_object_is_type(entry, json_type_boolean)
               ? json_object_get_boolean(entry)
               : fallback;
}

static double json_double_default(json_object *object, const char *key, double fallback) {
    json_object *entry = NULL;
    return json_object_object_get_ex(object, key, &entry) &&
                   (json_object_is_type(entry, json_type_double) || json_object_is_type(entry, json_type_int))
               ? json_object_get_double(entry)
               : fallback;
}

static gboolean rule_is_valid(json_object *rule, const char *event, const char *provider) {
    if (!json_object_is_type(rule, json_type_object) || !json_boolean_default(rule, "enabled", TRUE)) return FALSE;
    const char *rule_event = NULL;
    size_t event_length = 0;
    const char *executable = NULL;
    size_t executable_length = 0;
    if (!json_string_value(rule, "event", &rule_event, &event_length) ||
        !codexbar_hook_event_is_known(rule_event) || !g_str_equal(rule_event, event) ||
        !json_string_value(rule, "executable", &executable, &executable_length) || executable_length == 0 ||
        executable_length > CODEXBAR_MAX_HOOK_STRING_BYTES || !g_path_is_absolute(executable)) {
        return FALSE;
    }
    json_object *rule_provider = NULL;
    if (json_object_object_get_ex(rule, "provider", &rule_provider) &&
        !json_object_is_type(rule_provider, json_type_null)) {
        if (!json_object_is_type(rule_provider, json_type_string)) return FALSE;
        const char *raw_provider = json_object_get_string(rule_provider);
        const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(raw_provider);
        if (!descriptor || !g_str_equal(descriptor->id, raw_provider) || !g_str_equal(raw_provider, provider)) return FALSE;
    }
    double threshold = json_double_default(rule, "threshold", -1);
    if (threshold != -1 && (!isfinite(threshold) || threshold <= 0 || threshold > 1)) return FALSE;
    if (g_str_equal(event, "quota_low") && threshold != -1 && 1 < threshold) return FALSE;
    double timeout = json_double_default(rule, "timeoutSeconds", 10);
    if (!isfinite(timeout) || timeout < 0.1 || timeout > 300) return FALSE;
    json_object *id = NULL;
    if (json_object_object_get_ex(rule, "id", &id)) {
        if (!json_object_is_type(id, json_type_string) || json_object_get_string_len(id) == 0 ||
            json_object_get_string_len(id) > CODEXBAR_MAX_HOOK_ID_BYTES) {
            return FALSE;
        }
    }
    json_object *arguments = NULL;
    size_t command_bytes = executable_length;
    if (json_object_object_get_ex(rule, "arguments", &arguments)) {
        if (!json_object_is_type(arguments, json_type_array) ||
            json_object_array_length(arguments) > CODEXBAR_MAX_HOOK_ARGUMENTS) {
            return FALSE;
        }
        for (size_t index = 0; index < json_object_array_length(arguments); index++) {
            json_object *argument = json_object_array_get_idx(arguments, index);
            if (!json_object_is_type(argument, json_type_string)) return FALSE;
            size_t length = (size_t)json_object_get_string_len(argument);
            if (length > CODEXBAR_MAX_HOOK_STRING_BYTES || strlen(json_object_get_string(argument)) != length ||
                length > CODEXBAR_MAX_HOOK_COMMAND_BYTES - MIN((size_t)CODEXBAR_MAX_HOOK_COMMAND_BYTES, command_bytes)) {
                return FALSE;
            }
            command_bytes += length;
        }
    }
    return command_bytes <= CODEXBAR_MAX_HOOK_COMMAND_BYTES;
}

static char *iso8601_now(void) {
    GDateTime *now = g_date_time_new_now_utc();
    char *timestamp = g_date_time_format(now, "%Y-%m-%dT%H:%M:%SZ");
    g_date_time_unref(now);
    return timestamp;
}

static json_object *event_payload(const CodexBarHookEvent *event, const char *timestamp) {
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "event", json_object_new_string(event->event));
    json_object_object_add(payload, "provider", json_object_new_string(event->provider));
    if (event->account) json_object_object_add(payload, "account", json_object_new_string(event->account));
    if (event->has_usage_percent) {
        json_object_object_add(payload, "usagePercent", json_object_new_double(event->usage_percent));
    }
    if (event->window) json_object_object_add(payload, "window", json_object_new_string(event->window));
    if (event->has_used) json_object_object_add(payload, "used", json_object_new_double(event->used));
    if (event->has_limit) json_object_object_add(payload, "limit", json_object_new_double(event->limit));
    if (event->reset_at) json_object_object_add(payload, "resetAt", json_object_new_string(event->reset_at));
    if (event->status) json_object_object_add(payload, "status", json_object_new_string(event->status));
    json_object_object_add(payload, "timestamp", json_object_new_string(timestamp));
    return payload;
}

static char **hook_environment(const CodexBarHookEvent *event, json_object *payload, const char *timestamp) {
    const char *forwarded[] = {
        "PATH", "HOME", "USER", "LOGNAME", "SHELL", "LANG", "LC_ALL", "LC_CTYPE", "TERM", "TMPDIR"};
    GPtrArray *environment = g_ptr_array_new_with_free_func(g_free);
    for (guint index = 0; index < G_N_ELEMENTS(forwarded); index++) {
        const char *value = g_getenv(forwarded[index]);
        if (value) g_ptr_array_add(environment, g_strdup_printf("%s=%s", forwarded[index], value));
    }
    g_ptr_array_add(environment, g_strdup_printf("CODEXBAR_EVENT=%s", event->event));
    g_ptr_array_add(environment, g_strdup_printf("CODEXBAR_PROVIDER=%s", event->provider));
    g_ptr_array_add(environment, g_strdup_printf("CODEXBAR_TIMESTAMP=%s", timestamp));
    json_object *value = NULL;
    if (json_object_object_get_ex(payload, "account", &value)) {
        g_ptr_array_add(environment, g_strdup_printf("CODEXBAR_ACCOUNT=%s", json_object_get_string(value)));
    }
    if (json_object_object_get_ex(payload, "window", &value)) {
        g_ptr_array_add(environment, g_strdup_printf("CODEXBAR_WINDOW=%s", json_object_get_string(value)));
    }
    if (json_object_object_get_ex(payload, "usagePercent", &value)) {
        g_ptr_array_add(environment,
                        g_strdup_printf("CODEXBAR_USAGE_PERCENT=%g", json_object_get_double(value)));
    }
    if (json_object_object_get_ex(payload, "used", &value)) {
        g_ptr_array_add(environment, g_strdup_printf("CODEXBAR_USED=%g", json_object_get_double(value)));
    }
    if (json_object_object_get_ex(payload, "limit", &value)) {
        g_ptr_array_add(environment, g_strdup_printf("CODEXBAR_LIMIT=%g", json_object_get_double(value)));
    }
    if (json_object_object_get_ex(payload, "resetAt", &value)) {
        g_ptr_array_add(environment, g_strdup_printf("CODEXBAR_RESET_AT=%s", json_object_get_string(value)));
    }
    if (json_object_object_get_ex(payload, "status", &value)) {
        g_ptr_array_add(environment, g_strdup_printf("CODEXBAR_STATUS=%s", json_object_get_string(value)));
    }
    g_ptr_array_add(environment, NULL);
    return (char **)g_ptr_array_free(environment, FALSE);
}

static void free_string_vector(char **values) {
    if (!values) return;
    for (guint index = 0; values[index]; index++) g_free(values[index]);
    g_free(values);
}

static char **rule_arguments(json_object *rule) {
    const char *executable = json_object_get_string(json_object_object_get(rule, "executable"));
    json_object *configured = NULL;
    size_t count = json_object_object_get_ex(rule, "arguments", &configured) ? json_object_array_length(configured) : 0;
    char **arguments = g_new0(char *, count + 2);
    arguments[0] = g_strdup(executable);
    for (size_t index = 0; index < count; index++) {
        arguments[index + 1] = g_strdup(json_object_get_string(json_object_array_get_idx(configured, index)));
    }
    return arguments;
}

static const char *process_failure_summary(const GError *error) {
    if (!error) return "error";
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) return "timed out";
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_MESSAGE_TOO_LARGE)) return "output too large";
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND)) return "executable not found";
    return "launch failed";
}

static char *trimmed_copy(const char *value) {
    char *copy = g_strdup(value ? value : "");
    g_strstrip(copy);
    return copy;
}

static CodexBarProcessResult *default_process_runner(const CodexBarProcessRequest *request,
                                                     gpointer user_data,
                                                     GError **error) {
    (void)user_data;
    return codexbar_process_run(request, NULL, error);
}

static json_object *run_rule(json_object *rule,
                             const CodexBarHookEvent *event,
                             json_object *payload,
                             CodexBarHookProcessRunner process_runner,
                             gpointer process_runner_data) {
    const char *payload_string = json_object_to_json_string_ext(payload, JSON_C_TO_STRING_PLAIN);
    const char *timestamp = json_object_get_string(json_object_object_get(payload, "timestamp"));
    char **environment = hook_environment(event, payload, timestamp);
    char **arguments = rule_arguments(rule);
    double timeout = json_double_default(rule, "timeoutSeconds", 10);
    CodexBarProcessRequest request = {
        .arguments = (const char *const *)arguments,
        .environment = (const char *const *)environment,
        .standard_input = payload_string,
        .standard_input_length = strlen(payload_string),
        .timeout_milliseconds = (guint)ceil(timeout * 1000),
        .termination_grace_milliseconds = 400,
        .maximum_output_bytes = CODEXBAR_MAX_HOOK_OUTPUT_BYTES,
        .new_session = TRUE,
    };
    GError *error = NULL;
    CodexBarProcessResult *result = request.standard_input_length <= CODEXBAR_MAX_HOOK_PAYLOAD_BYTES
                                        ? process_runner(&request, process_runner_data, &error)
                                        : NULL;
    json_object *output = json_object_new_object();
    json_object *id = NULL;
    char *generated_id = NULL;
    const char *rule_id = json_object_object_get_ex(rule, "id", &id) && json_object_is_type(id, json_type_string)
                              ? json_object_get_string(id)
                              : (generated_id = g_uuid_string_random());
    json_object_object_add(output, "ruleID", json_object_new_string(rule_id));
    json_object_object_add(output, "executable", json_object_new_string(arguments[0]));
    json_object_object_add(output, "event", json_object_new_string(event->event));
    json_object_object_add(output, "provider", json_object_new_string(event->provider));
    gboolean success = result && codexbar_process_result_succeeded(result);
    json_object_object_add(output, "success", json_object_new_boolean(success));
    if (success) {
        char *stdout_text = trimmed_copy(result->standard_output);
        if (stdout_text[0]) json_object_object_add(output, "stdout", json_object_new_string(stdout_text));
        else json_object_object_add(output, "stdout", NULL);
        json_object_object_add(output, "error", NULL);
        g_free(stdout_text);
    } else {
        json_object_object_add(output, "stdout", NULL);
        char *summary = NULL;
        if (result) summary = g_strdup_printf("exit %d", result->exit_status);
        json_object_object_add(output,
                               "error",
                               json_object_new_string(summary ? summary : process_failure_summary(error)));
        g_free(summary);
    }
    codexbar_process_result_free(result);
    g_clear_error(&error);
    g_free(generated_id);
    free_string_vector(arguments);
    free_string_vector(environment);
    return output;
}

json_object *codexbar_hooks_dispatch(json_object *hooks,
                                     const CodexBarHookEvent *event,
                                     CodexBarHookProcessRunner process_runner,
                                     gpointer process_runner_data) {
    json_object *results = json_object_new_array();
    json_object *events = codexbar_hook_events(hooks);
    if (!codexbar_hooks_enabled(hooks) || !event || !codexbar_hook_event_is_known(event->event) || !event->provider ||
        !events || json_object_array_length(events) > CODEXBAR_MAX_HOOK_RULES) {
        return results;
    }

    char *generated_timestamp = event->timestamp ? NULL : iso8601_now();
    const char *timestamp = event->timestamp ? event->timestamp : generated_timestamp;
    json_object *payload = event_payload(event, timestamp);
    CodexBarHookProcessRunner runner = process_runner ? process_runner : default_process_runner;
    for (size_t index = 0; index < json_object_array_length(events); index++) {
        json_object *rule = json_object_array_get_idx(events, index);
        if (!rule_is_valid(rule, event->event, event->provider)) continue;
        json_object_array_add(results, run_rule(rule, event, payload, runner, process_runner_data));
    }
    json_object_put(payload);
    g_free(generated_timestamp);
    return results;
}
