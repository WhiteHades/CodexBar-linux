#include "cli_hooks.h"

#include "config.h"
#include "hooks.h"
#include "provider_registry.h"

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

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

static json_object *normalized_hooks(json_object *hooks) {
    json_object *output = json_object_new_object();
    json_object_object_add(output, "enabled", json_object_new_boolean(codexbar_hooks_enabled(hooks)));
    json_object *events = codexbar_hook_events(hooks);
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
        printf("Hooks: %s\n", codexbar_hooks_enabled(hooks) ? "enabled" : "disabled");
        json_object *events = codexbar_hook_events(hooks);
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
    if (!codexbar_hook_event_is_known(options.event)) {
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
    if (!codexbar_hooks_enabled(hooks)) {
        codexbar_config_free(config);
        return argument_error("Hooks are disabled.");
    }
    json_object *events = codexbar_hook_events(hooks);
    if (!events || json_object_array_length(events) > CODEXBAR_MAX_HOOK_RULES) {
        codexbar_config_free(config);
        return argument_error("No hook rule matches this event and provider.");
    }
    GDateTime *now = g_date_time_new_now_utc();
    char *timestamp = g_date_time_format(now, "%Y-%m-%dT%H:%M:%SZ");
    g_date_time_unref(now);
    gboolean quota_event = g_str_has_prefix(options.event, "quota_");
    gboolean reset_event = g_str_equal(options.event, "quota_reset");
    const char *status = g_str_equal(options.event, "provider_unavailable") ? "major"
                         : g_str_equal(options.event, "provider_recovered") ? "none"
                                                                            : "error";
    CodexBarHookEvent event = {
        .event = options.event,
        .provider = provider->id,
        .account = "test@example.com",
        .window = quota_event ? "session" : NULL,
        .status = quota_event ? NULL : status,
        .timestamp = timestamp,
        .has_usage_percent = quota_event,
        .usage_percent = reset_event ? 0 : 1,
        .has_used = quota_event,
        .used = reset_event ? 0 : 1,
        .has_limit = quota_event,
        .limit = 1,
        .reset_at = quota_event ? timestamp : NULL,
    };
    json_object *results = codexbar_hooks_dispatch(hooks, &event, NULL, NULL);
    g_free(timestamp);
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
