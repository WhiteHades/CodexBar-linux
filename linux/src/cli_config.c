#include "cli_config.h"

#include "config.h"
#include "provider_registry.h"

#include <json-c/json.h>
#include <stdio.h>
#include <string.h>

static const char *option_value(int argc, char **argv, const char *name) {
    for (int index = 0; index + 1 < argc; index++) {
        if (g_str_equal(argv[index], name)) return argv[index + 1][0] == '-' ? NULL : argv[index + 1];
    }
    return NULL;
}

static gboolean has_flag(int argc, char **argv, const char *name) {
    for (int index = 0; index < argc; index++) {
        if (g_str_equal(argv[index], name)) return TRUE;
    }
    return FALSE;
}

static gboolean json_output(int argc, char **argv) {
    const char *format = option_value(argc, argv, "--format");
    return has_flag(argc, argv, "--json") || has_flag(argc, argv, "--json-only") ||
           (format && g_str_equal(format, "json"));
}

static int print_message_error(int argc, char **argv, const char *message) {
    if (json_output(argc, argv)) {
        json_object *object = json_object_new_object();
        json_object_object_add(object, "error", json_object_new_string(message));
        puts(json_object_to_json_string_ext(
            object, has_flag(argc, argv, "--pretty") ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(object);
    } else {
        fprintf(stderr, "%s\n", message);
    }
    return 1;
}

static int print_error(int argc, char **argv, GError *error) {
    int result = print_message_error(argc, argv, error ? error->message : "Config operation failed");
    g_clear_error(&error);
    return result;
}

static gboolean name_in(const char *name, const char *const *names, guint count) {
    for (guint index = 0; index < count; index++) {
        if (g_str_equal(name, names[index])) return TRUE;
    }
    return FALSE;
}

static char *validate_arguments(int argc,
                                char **argv,
                                const char *const *value_options,
                                guint value_count,
                                const char *const *flags,
                                guint flag_count) {
    for (int index = 0; index < argc; index++) {
        const char *argument = argv[index];
        if (name_in(argument, value_options, value_count)) {
            if (index + 1 >= argc || argv[index + 1][0] == '-') {
                return g_strdup_printf("Missing value for %s.", argument);
            }
            if (g_str_equal(argument, "--format") && !g_str_equal(argv[index + 1], "text") &&
                !g_str_equal(argv[index + 1], "json")) {
                return g_strdup("--format must be text or json.");
            }
            index++;
            continue;
        }
        if (name_in(argument, flags, flag_count)) continue;
        return g_strdup_printf("Unknown argument: %s", argument);
    }
    return NULL;
}

static CodexBarConfig *load_config(GError **error) {
    return codexbar_config_load(error);
}

static int run_validate(int argc, char **argv) {
    const char *values[] = {"--format"};
    const char *flags[] = {"--json", "--json-only", "--pretty"};
    char *argument_error = validate_arguments(argc, argv, values, G_N_ELEMENTS(values), flags, G_N_ELEMENTS(flags));
    if (argument_error) {
        int result = print_message_error(argc, argv, argument_error);
        g_free(argument_error);
        return result;
    }
    GError *error = NULL;
    CodexBarConfig *config = load_config(&error);
    if (!config) return print_error(argc, argv, error);
    GPtrArray *issues = codexbar_config_validate(config);
    gboolean has_errors = FALSE;
    if (json_output(argc, argv)) {
        json_object *array = json_object_new_array_ext((int)issues->len);
        for (guint index = 0; index < issues->len; index++) {
            const CodexBarConfigIssue *issue = g_ptr_array_index(issues, index);
            json_object *object = json_object_new_object();
            json_object_object_add(object, "severity", json_object_new_string(issue->error ? "error" : "warning"));
            if (issue->provider) json_object_object_add(object, "provider", json_object_new_string(issue->provider));
            if (issue->field) json_object_object_add(object, "field", json_object_new_string(issue->field));
            json_object_object_add(object, "code", json_object_new_string(issue->code));
            json_object_object_add(object, "message", json_object_new_string(issue->message));
            json_object_array_add(array, object);
            has_errors = has_errors || issue->error;
        }
        puts(json_object_to_json_string_ext(
            array, has_flag(argc, argv, "--pretty") ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(array);
    } else if (issues->len == 0) {
        puts("Config: OK");
    } else {
        for (guint index = 0; index < issues->len; index++) {
            const CodexBarConfigIssue *issue = g_ptr_array_index(issues, index);
            if (issue->field) {
                printf("[%s] %s (%s): %s\n",
                       issue->error ? "ERROR" : "WARNING",
                       issue->provider ? issue->provider : "config",
                       issue->field,
                       issue->message);
            } else {
                printf("[%s] %s: %s\n",
                       issue->error ? "ERROR" : "WARNING",
                       issue->provider ? issue->provider : "config",
                       issue->message);
            }
            has_errors = has_errors || issue->error;
        }
    }
    g_ptr_array_unref(issues);
    codexbar_config_free(config);
    return has_errors ? 1 : 0;
}

static int run_dump(int argc, char **argv) {
    const char *values[] = {"--format"};
    const char *flags[] = {"--json", "--json-only", "--pretty"};
    char *argument_error = validate_arguments(argc, argv, values, G_N_ELEMENTS(values), flags, G_N_ELEMENTS(flags));
    if (argument_error) {
        int result = print_message_error(argc, argv, argument_error);
        g_free(argument_error);
        return result;
    }
    GError *error = NULL;
    CodexBarConfig *config = load_config(&error);
    if (!config) return print_error(argc, argv, error);
    char *json = codexbar_config_render_json(config, has_flag(argc, argv, "--pretty"));
    puts(json);
    g_free(json);
    codexbar_config_free(config);
    return 0;
}

static int run_providers(int argc, char **argv) {
    const char *values[] = {"--format"};
    const char *flags[] = {"--json", "--json-only", "--pretty"};
    char *argument_error = validate_arguments(argc, argv, values, G_N_ELEMENTS(values), flags, G_N_ELEMENTS(flags));
    if (argument_error) {
        int result = print_message_error(argc, argv, argument_error);
        g_free(argument_error);
        return result;
    }
    GError *error = NULL;
    CodexBarConfig *config = load_config(&error);
    if (!config) return print_error(argc, argv, error);
    gboolean as_json = json_output(argc, argv);
    json_object *array = as_json ? json_object_new_array_ext((int)config->providers->len) : NULL;
    for (guint index = 0; index < config->providers->len; index++) {
        const CodexBarProviderConfig *entry = g_ptr_array_index(config->providers, index);
        const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(entry->id);
        if (as_json) {
            json_object *object = json_object_new_object();
            json_object_object_add(object, "provider", json_object_new_string(entry->id));
            json_object_object_add(object, "displayName", json_object_new_string(descriptor->display_name));
            json_object_object_add(object, "enabled", json_object_new_boolean(entry->enabled));
            json_object_object_add(object, "defaultEnabled", json_object_new_boolean(descriptor->default_enabled));
            json_object_array_add(array, object);
        } else {
            printf("%s: %s%s (%s)\n",
                   entry->id,
                   entry->enabled ? "enabled" : "disabled",
                   descriptor->default_enabled ? " default" : "",
                   descriptor->display_name);
        }
    }
    if (array) {
        puts(json_object_to_json_string_ext(
            array, has_flag(argc, argv, "--pretty") ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(array);
    }
    codexbar_config_free(config);
    return 0;
}

static const CodexBarProviderDescriptor *selected_provider(int argc, char **argv) {
    const char *raw = option_value(argc, argv, "--provider");
    if (!raw) return NULL;
    char *lowercase = g_ascii_strdown(raw, -1);
    const CodexBarProviderDescriptor *provider = codexbar_provider_registry_find(lowercase);
    g_free(lowercase);
    return provider;
}

static int run_toggle(int argc, char **argv, gboolean enabled) {
    const char *values[] = {"--format", "--provider"};
    const char *flags[] = {"--json", "--json-only", "--pretty"};
    char *argument_error = validate_arguments(argc, argv, values, G_N_ELEMENTS(values), flags, G_N_ELEMENTS(flags));
    if (argument_error) {
        int result = print_message_error(argc, argv, argument_error);
        g_free(argument_error);
        return result;
    }
    const CodexBarProviderDescriptor *provider = selected_provider(argc, argv);
    if (!provider) return print_message_error(argc, argv, "Unknown or missing provider. Use --provider <name>.");
    GError *error = NULL;
    CodexBarConfig *config = load_config(&error);
    if (!config) return print_error(argc, argv, error);
    if (!codexbar_config_set_enabled(config, provider->id, enabled, &error) ||
        !codexbar_config_save(config, &error)) {
        codexbar_config_free(config);
        return print_error(argc, argv, error);
    }
    if (json_output(argc, argv)) {
        json_object *object = json_object_new_object();
        json_object_object_add(object, "provider", json_object_new_string(provider->id));
        json_object_object_add(object, "displayName", json_object_new_string(provider->display_name));
        json_object_object_add(object, "enabled", json_object_new_boolean(enabled));
        json_object_object_add(object, "configPath", json_object_new_string(config->path));
        puts(json_object_to_json_string_ext(
            object, has_flag(argc, argv, "--pretty") ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(object);
    } else {
        printf("Config: %s %s\n", enabled ? "enabled" : "disabled", provider->display_name);
    }
    codexbar_config_free(config);
    return 0;
}

static char *read_stdin(void) {
    GString *input = g_string_new(NULL);
    char buffer[4096];
    size_t count = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), stdin)) > 0) g_string_append_len(input, buffer, count);
    return g_string_free(input, FALSE);
}

static int run_set_api_key(int argc, char **argv) {
    const char *values[] = {"--format", "--provider", "--api-key"};
    const char *flags[] = {"--json", "--json-only", "--pretty", "--stdin", "--no-enable"};
    char *argument_error = validate_arguments(argc, argv, values, G_N_ELEMENTS(values), flags, G_N_ELEMENTS(flags));
    if (argument_error) {
        int result = print_message_error(argc, argv, argument_error);
        g_free(argument_error);
        return result;
    }
    const CodexBarProviderDescriptor *provider = selected_provider(argc, argv);
    if (!provider) return print_message_error(argc, argv, "Unknown or missing provider. Use --provider <name>.");
    const char *argument = option_value(argc, argv, "--api-key");
    gboolean from_stdin = has_flag(argc, argv, "--stdin");
    if ((argument != NULL) == from_stdin) {
        return print_message_error(argc, argv, "Use exactly one of --api-key <key> or --stdin.");
    }
    char *stdin_key = from_stdin ? read_stdin() : NULL;
    const char *api_key = stdin_key ? stdin_key : argument;
    GError *error = NULL;
    CodexBarConfig *config = load_config(&error);
    if (!config) {
        g_free(stdin_key);
        return print_error(argc, argv, error);
    }
    gboolean enabled = !has_flag(argc, argv, "--no-enable");
    if (!codexbar_config_set_api_key(config, provider->id, api_key, enabled, &error) ||
        !codexbar_config_save(config, &error)) {
        codexbar_config_free(config);
        g_free(stdin_key);
        return print_error(argc, argv, error);
    }
    if (json_output(argc, argv)) {
        json_object *object = json_object_new_object();
        json_object_object_add(object, "provider", json_object_new_string(provider->id));
        json_object_object_add(object, "enabled", json_object_new_boolean(
            codexbar_config_provider(config, provider->id)->enabled));
        json_object_object_add(object, "configPath", json_object_new_string(config->path));
        puts(json_object_to_json_string_ext(
            object, has_flag(argc, argv, "--pretty") ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(object);
    } else {
        printf("Config: stored API key for %s%s\n", provider->display_name, enabled ? " and enabled" : "");
    }
    codexbar_config_free(config);
    g_free(stdin_key);
    return 0;
}

int codexbar_cli_config_run(int argc, char **argv) {
    if (argc < 1) {
        fputs("Usage: codexbar-linux config <validate|dump|providers|enable|disable|set-api-key>\n", stderr);
        return 1;
    }
    if (g_str_equal(argv[0], "validate")) return run_validate(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "dump")) return run_dump(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "providers")) return run_providers(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "enable")) return run_toggle(argc - 1, argv + 1, TRUE);
    if (g_str_equal(argv[0], "disable")) return run_toggle(argc - 1, argv + 1, FALSE);
    if (g_str_equal(argv[0], "set-api-key")) return run_set_api_key(argc - 1, argv + 1);
    fprintf(stderr, "Unknown config command: %s\n", argv[0]);
    return 1;
}
