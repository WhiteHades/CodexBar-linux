#include "cli_hooks.h"

#include "config.h"
#include "process.h"
#include "provider_registry.h"

#include <json-c/json.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

enum {
    MAX_HOOK_RULES = 32,
    MAX_HOOK_ID_BYTES = 128,
    MAX_HOOK_ARGUMENTS = 32,
    MAX_HOOK_STRING_BYTES = 4096,
    MAX_HOOK_COMMAND_BYTES = 32 * 1024,
    MAX_HOOK_PAYLOAD_BYTES = 4096,
    MAX_HOOK_OUTPUT_BYTES = 1024 * 1024,
};

static const char *const event_names[] = {
    "quota_low",
    "quota_reached",
    "quota_reset",
    "provider_unavailable",
    "provider_recovered",
    "refresh_failed",
};

static gboolean event_is_known(const char *event) {
    for (guint index = 0; index < G_N_ELEMENTS(event_names); index++) {
        if (g_strcmp0(event, event_names[index]) == 0) return TRUE;
    }
    return FALSE;
}

static gboolean option_is(const char *argument, const char *name) {
    return g_str_equal(argument, name);
}

static int argument_error(const char *message) {
    fprintf(stderr, "%s\n", message);
    return 1;
}

typedef struct {
    gboolean json;
    gboolean pretty;
    const char *provider;
    const char *event;
} HookOptions;

static gboolean parse_options(int argc, char **argv, gboolean test, HookOptions *options, const char **error) {
    for (int index = 0; index < argc; index++) {
        const char *argument = argv[index];
        if (option_is(argument, "--json") || option_is(argument, "--json-only")) {
            options->json = TRUE;
        } else if (option_is(argument, "--pretty")) {
            options->pretty = TRUE;
        } else if (option_is(argument, "--format")) {
            if (++index >= argc) {
                *error = "Missing value for --format.";
                return FALSE;
            }
            if (g_str_equal(argv[index], "json")) {
                options->json = TRUE;
            } else if (!g_str_equal(argv[index], "text")) {
                *error = "Invalid format. Use text or json.";
                return FALSE;
            }
        } else if (test && option_is(argument, "--provider")) {
            if (++index >= argc) {
                *error = "Missing value for --provider.";
                return FALSE;
            }
            options->provider = argv[index];
        } else if (test && argument[0] != '-' && !options->event) {
            options->event = argument;
        } else {
            *error = "Unknown hooks argument.";
            return FALSE;
        }
    }
    return TRUE;
}

static json_object *hooks_object(CodexBarConfig *config, gboolean create) {
    if (!config->raw && create) config->raw = json_object_new_object();
    if (!config->raw) return NULL;
    json_object *hooks = NULL;
    if (json_object_object_get_ex(config->raw, "hooks", &hooks) && json_object_is_type(hooks, json_type_object)) {
        return hooks;
    }
    if (!create) return NULL;
    hooks = json_object_new_object();
    json_object_object_add(config->raw, "hooks", hooks);
    return hooks;
}

static gboolean hooks_enabled(json_object *hooks) {
    json_object *enabled = NULL;
    return hooks && json_object_object_get_ex(hooks, "enabled", &enabled) &&
           json_object_is_type(enabled, json_type_boolean) && json_object_get_boolean(enabled);
}

static json_object *hook_events(json_object *hooks) {
    json_object *events = NULL;
    return hooks && json_object_object_get_ex(hooks, "events", &events) &&
                   json_object_is_type(events, json_type_array)
               ? events
               : NULL;
}

static json_object *normalized_hooks(json_object *hooks) {
    json_object *output = json_object_new_object();
    json_object_object_add(output, "enabled", json_object_new_boolean(hooks_enabled(hooks)));
    json_object *events = hook_events(hooks);
    json_object *copy = events ? json_tokener_parse(json_object_to_json_string_ext(events, JSON_C_TO_STRING_PLAIN))
                               : json_object_new_array();
    json_object_object_add(output, "events", copy);
    return output;
}

static int load_failure(GError *error) {
    fprintf(stderr, "%s\n", error ? error->message : "Could not load config.");
    g_clear_error(&error);
    return 1;
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
    if (!json_string_value(rule, "event", &rule_event, &event_length) || !event_is_known(rule_event) ||
        !g_str_equal(rule_event, event) ||
        !json_string_value(rule, "executable", &executable, &executable_length) || executable_length == 0 ||
        executable_length > MAX_HOOK_STRING_BYTES || !g_path_is_absolute(executable)) {
        return FALSE;
    }
    json_object *rule_provider = NULL;
    if (json_object_object_get_ex(rule, "provider", &rule_provider) && !json_object_is_type(rule_provider, json_type_null)) {
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
            json_object_get_string_len(id) > MAX_HOOK_ID_BYTES) {
            return FALSE;
        }
    }
    json_object *arguments = NULL;
    size_t command_bytes = executable_length;
    if (json_object_object_get_ex(rule, "arguments", &arguments)) {
        if (!json_object_is_type(arguments, json_type_array) ||
            json_object_array_length(arguments) > MAX_HOOK_ARGUMENTS) {
            return FALSE;
        }
        for (size_t index = 0; index < json_object_array_length(arguments); index++) {
            json_object *argument = json_object_array_get_idx(arguments, index);
            if (!json_object_is_type(argument, json_type_string)) return FALSE;
            size_t length = (size_t)json_object_get_string_len(argument);
            if (length > MAX_HOOK_STRING_BYTES || strlen(json_object_get_string(argument)) != length ||
                length > MAX_HOOK_COMMAND_BYTES - MIN((size_t)MAX_HOOK_COMMAND_BYTES, command_bytes)) {
                return FALSE;
            }
            command_bytes += length;
        }
    }
    return command_bytes <= MAX_HOOK_COMMAND_BYTES;
}

static char *iso8601_now(void) {
    GDateTime *now = g_date_time_new_now_utc();
    char *timestamp = g_date_time_format(now, "%Y-%m-%dT%H:%M:%SZ");
    g_date_time_unref(now);
    return timestamp;
}

static json_object *sample_event(const char *event, const char *provider, const char *timestamp) {
    json_object *payload = json_object_new_object();
    json_object_object_add(payload, "event", json_object_new_string(event));
    json_object_object_add(payload, "provider", json_object_new_string(provider));
    if (g_str_has_prefix(event, "quota_")) {
        json_object_object_add(payload, "usagePercent", json_object_new_double(g_str_equal(event, "quota_reset") ? 0 : 1));
        json_object_object_add(payload, "window", json_object_new_string("session"));
    } else {
        const char *status = g_str_equal(event, "provider_unavailable") ? "major"
                             : g_str_equal(event, "provider_recovered") ? "none"
                                                                          : "error";
        json_object_object_add(payload, "status", json_object_new_string(status));
    }
    json_object_object_add(payload, "timestamp", json_object_new_string(timestamp));
    return payload;
}

static char **hook_environment(const char *event, const char *provider, json_object *payload, const char *timestamp) {
    const char *forwarded[] = {
        "PATH", "HOME", "USER", "LOGNAME", "SHELL", "LANG", "LC_ALL", "LC_CTYPE", "TERM", "TMPDIR"};
    GPtrArray *environment = g_ptr_array_new_with_free_func(g_free);
    for (guint index = 0; index < G_N_ELEMENTS(forwarded); index++) {
        const char *value = g_getenv(forwarded[index]);
        if (value) g_ptr_array_add(environment, g_strdup_printf("%s=%s", forwarded[index], value));
    }
    g_ptr_array_add(environment, g_strdup_printf("CODEXBAR_EVENT=%s", event));
    g_ptr_array_add(environment, g_strdup_printf("CODEXBAR_PROVIDER=%s", provider));
    g_ptr_array_add(environment, g_strdup_printf("CODEXBAR_TIMESTAMP=%s", timestamp));
    json_object *value = NULL;
    if (json_object_object_get_ex(payload, "window", &value)) {
        g_ptr_array_add(environment, g_strdup_printf("CODEXBAR_WINDOW=%s", json_object_get_string(value)));
    }
    if (json_object_object_get_ex(payload, "usagePercent", &value)) {
        g_ptr_array_add(environment,
                        g_strdup_printf("CODEXBAR_USAGE_PERCENT=%g", json_object_get_double(value)));
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

static json_object *run_rule(json_object *rule, const char *event, const char *provider, json_object *payload) {
    const char *payload_string = json_object_to_json_string_ext(payload, JSON_C_TO_STRING_PLAIN);
    char *timestamp = g_strdup(json_object_get_string(json_object_object_get(payload, "timestamp")));
    char **environment = hook_environment(event, provider, payload, timestamp);
    char **arguments = rule_arguments(rule);
    double timeout = json_double_default(rule, "timeoutSeconds", 10);
    CodexBarProcessRequest request = {
        .arguments = (const char *const *)arguments,
        .environment = (const char *const *)environment,
        .standard_input = payload_string,
        .standard_input_length = strlen(payload_string),
        .timeout_milliseconds = (guint)ceil(timeout * 1000),
        .termination_grace_milliseconds = 400,
        .maximum_output_bytes = MAX_HOOK_OUTPUT_BYTES,
        .new_session = TRUE,
    };
    GError *error = NULL;
    CodexBarProcessResult *result = request.standard_input_length <= MAX_HOOK_PAYLOAD_BYTES
                                        ? codexbar_process_run(&request, NULL, &error)
                                        : NULL;
    json_object *output = json_object_new_object();
    json_object *id = NULL;
    char *generated_id = NULL;
    const char *rule_id = json_object_object_get_ex(rule, "id", &id) && json_object_is_type(id, json_type_string)
                              ? json_object_get_string(id)
                              : (generated_id = g_uuid_string_random());
    json_object_object_add(output, "ruleID", json_object_new_string(rule_id));
    json_object_object_add(output, "executable", json_object_new_string(arguments[0]));
    json_object_object_add(output, "event", json_object_new_string(event));
    json_object_object_add(output, "provider", json_object_new_string(provider));
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
    g_free(timestamp);
    return output;
}

static int run_list(int argc, char **argv) {
    HookOptions options = {0};
    const char *error = NULL;
    if (!parse_options(argc, argv, FALSE, &options, &error)) return argument_error(error);
    GError *load_error = NULL;
    CodexBarConfig *config = codexbar_config_load(&load_error);
    if (!config) return load_failure(load_error);
    json_object *hooks = hooks_object(config, FALSE);
    if (options.json) {
        json_object *output = normalized_hooks(hooks);
        puts(json_object_to_json_string_ext(
            output, options.pretty ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(output);
    } else {
        printf("Hooks: %s\n", hooks_enabled(hooks) ? "enabled" : "disabled");
        json_object *events = hook_events(hooks);
        if (!events || json_object_array_length(events) == 0) {
            puts("No rules configured.");
        } else {
            for (size_t index = 0; index < json_object_array_length(events); index++) {
                json_object *rule = json_object_array_get_idx(events, index);
                const char *event = "invalid";
                const char *executable = "";
                const char *provider = "any";
                size_t unused = 0;
                json_string_value(rule, "event", &event, &unused);
                json_string_value(rule, "executable", &executable, &unused);
                json_object *provider_value = NULL;
                if (json_object_object_get_ex(rule, "provider", &provider_value) &&
                    json_object_is_type(provider_value, json_type_string)) {
                    provider = json_object_get_string(provider_value);
                }
                printf("[%s] %s provider=%s: %s",
                       json_boolean_default(rule, "enabled", TRUE) ? "on" : "off",
                       event,
                       provider,
                       executable);
                json_object *arguments = NULL;
                if (json_object_object_get_ex(rule, "arguments", &arguments) &&
                    json_object_is_type(arguments, json_type_array)) {
                    for (size_t arg = 0; arg < json_object_array_length(arguments); arg++) {
                        json_object *value = json_object_array_get_idx(arguments, arg);
                        if (json_object_is_type(value, json_type_string)) printf(" %s", json_object_get_string(value));
                    }
                }
                putchar('\n');
            }
        }
    }
    codexbar_config_free(config);
    return 0;
}

static int run_set_enabled(int argc, char **argv, gboolean enabled) {
    HookOptions options = {0};
    const char *error = NULL;
    if (!parse_options(argc, argv, FALSE, &options, &error)) return argument_error(error);
    GError *config_error = NULL;
    CodexBarConfig *config = codexbar_config_load_for_update(&config_error);
    if (!config) return load_failure(config_error);
    json_object *hooks = hooks_object(config, TRUE);
    json_object_object_add(hooks, "enabled", json_object_new_boolean(enabled));
    if (!codexbar_config_save(config, &config_error)) {
        codexbar_config_free(config);
        return load_failure(config_error);
    }
    if (options.json) {
        json_object *output = normalized_hooks(hooks);
        puts(json_object_to_json_string_ext(
            output, options.pretty ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(output);
    } else {
        printf("Hooks: %s\n", enabled ? "enabled" : "disabled");
    }
    codexbar_config_free(config);
    return 0;
}

static int run_test(int argc, char **argv) {
    HookOptions options = {0};
    const char *option_error = NULL;
    if (!parse_options(argc, argv, TRUE, &options, &option_error)) return argument_error(option_error);
    if (!event_is_known(options.event)) {
        return argument_error(
            "Unknown or missing event. Use one of: quota_low, quota_reached, quota_reset, provider_unavailable, "
            "provider_recovered, refresh_failed.");
    }
    char *provider_name = options.provider ? g_ascii_strdown(options.provider, -1) : NULL;
    const CodexBarProviderDescriptor *provider = codexbar_provider_registry_find(provider_name);
    g_free(provider_name);
    if (!provider) return argument_error("Unknown or missing provider. Use --provider <name>.");
    GError *load_error = NULL;
    CodexBarConfig *config = codexbar_config_load(&load_error);
    if (!config) return load_failure(load_error);
    json_object *hooks = hooks_object(config, FALSE);
    if (!hooks_enabled(hooks)) {
        codexbar_config_free(config);
        return argument_error("Hooks are disabled.");
    }
    json_object *events = hook_events(hooks);
    if (!events || json_object_array_length(events) > MAX_HOOK_RULES) {
        codexbar_config_free(config);
        return argument_error("No hook rule matches this event and provider.");
    }
    char *timestamp = iso8601_now();
    json_object *payload = sample_event(options.event, provider->id, timestamp);
    g_free(timestamp);
    json_object *results = json_object_new_array();
    for (size_t index = 0; index < json_object_array_length(events); index++) {
        json_object *rule = json_object_array_get_idx(events, index);
        if (!rule_is_valid(rule, options.event, provider->id)) continue;
        json_object_array_add(results, run_rule(rule, options.event, provider->id, payload));
    }
    json_object_put(payload);
    if (json_object_array_length(results) == 0) {
        json_object_put(results);
        codexbar_config_free(config);
        char *message = g_strdup_printf("No hook rule matches %s for %s.", options.event, provider->id);
        int status = argument_error(message);
        g_free(message);
        return status;
    }
    gboolean succeeded = TRUE;
    for (size_t index = 0; index < json_object_array_length(results); index++) {
        json_object *result = json_object_array_get_idx(results, index);
        json_object *success = json_object_object_get(result, "success");
        succeeded = succeeded && json_object_get_boolean(success);
        if (!options.json) {
            const char *executable = json_object_get_string(json_object_object_get(result, "executable"));
            if (json_object_get_boolean(success)) {
                json_object *stdout_value = json_object_object_get(result, "stdout");
                printf("ran %s: OK%s%s\n",
                       executable,
                       stdout_value ? " — " : "",
                       stdout_value ? json_object_get_string(stdout_value) : "");
            } else {
                fprintf(stderr,
                        "ran %s: %s\n",
                        executable,
                        json_object_get_string(json_object_object_get(result, "error")));
            }
        }
    }
    if (options.json) {
        puts(json_object_to_json_string_ext(
            results, options.pretty ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
    }
    json_object_put(results);
    codexbar_config_free(config);
    return succeeded ? 0 : 1;
}

int codexbar_cli_hooks_run(int argc, char **argv) {
    if (argc < 1) return argument_error("Usage: codexbar-linux hooks <list|enable|disable|test>.");
    if (g_str_equal(argv[0], "list")) return run_list(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "enable")) return run_set_enabled(argc - 1, argv + 1, TRUE);
    if (g_str_equal(argv[0], "disable")) return run_set_enabled(argc - 1, argv + 1, FALSE);
    if (g_str_equal(argv[0], "test")) return run_test(argc - 1, argv + 1);
    return argument_error("Unknown hooks command.");
}
