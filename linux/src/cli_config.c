#include "cli_config.h"

#include "config.h"
#include "provider_registry.h"
#include "token_accounts.h"

#include <gio/gio.h>
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

static CodexBarConfig *load_config_for_update(GError **error) {
    return codexbar_config_load_for_update(error);
}

static void redact_string_member(json_object *object, const char *key) {
    json_object *value = NULL;
    if (json_object_object_get_ex(object, key, &value) && !json_object_is_type(value, json_type_null)) {
        json_object_object_add(object, key, json_object_new_string("[REDACTED]"));
    }
}

static void redact_provider_secrets(json_object *provider) {
    redact_string_member(provider, "apiKey");
    redact_string_member(provider, "secretKey");
    redact_string_member(provider, "cookieHeader");
    redact_string_member(provider, "oauthToken");
    redact_string_member(provider, "bearerToken");

    json_object *token_accounts = NULL;
    json_object *accounts = NULL;
    if (!json_object_object_get_ex(provider, "tokenAccounts", &token_accounts) ||
        !json_object_is_type(token_accounts, json_type_object) ||
        !json_object_object_get_ex(token_accounts, "accounts", &accounts) ||
        !json_object_is_type(accounts, json_type_array)) {
        return;
    }
    for (size_t index = 0; index < json_object_array_length(accounts); index++) {
        json_object *account = json_object_array_get_idx(accounts, index);
        if (json_object_is_type(account, json_type_object)) redact_string_member(account, "token");
    }
}

static char *redacted_config_json(char *json, gboolean pretty) {
    json_object *root = json_tokener_parse(json);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        return json;
    }
    json_object *providers = NULL;
    if (json_object_object_get_ex(root, "providers", &providers) &&
        json_object_is_type(providers, json_type_array)) {
        for (size_t index = 0; index < json_object_array_length(providers); index++) {
            json_object *provider = json_object_array_get_idx(providers, index);
            if (json_object_is_type(provider, json_type_object)) redact_provider_secrets(provider);
        }
    }
    char *result = g_strdup(json_object_to_json_string_ext(
        root, pretty ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
    json_object_put(root);
    g_free(json);
    return result;
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
    const char *flags[] = {"--json", "--json-only", "--pretty", "--show-secrets"};
    char *argument_error = validate_arguments(argc, argv, values, G_N_ELEMENTS(values), flags, G_N_ELEMENTS(flags));
    if (argument_error) {
        int result = print_message_error(argc, argv, argument_error);
        g_free(argument_error);
        return result;
    }
    GError *error = NULL;
    CodexBarConfig *config = load_config(&error);
    if (!config) return print_error(argc, argv, error);
    gboolean pretty = has_flag(argc, argv, "--pretty");
    char *json = codexbar_config_render_json(config, pretty);
    if (!has_flag(argc, argv, "--show-secrets")) json = redacted_config_json(json, pretty);
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
    CodexBarConfig *config = load_config_for_update(&error);
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

static char *read_stdin(size_t *length) {
    GString *input = g_string_new(NULL);
    char buffer[4096];
    size_t count = 0;
    while ((count = fread(buffer, 1, sizeof(buffer), stdin)) > 0) g_string_append_len(input, buffer, count);
    *length = input->len;
    return g_string_free(input, FALSE);
}

static json_object *account_summary(json_object *account, guint index, int active_index) {
    json_object *summary = json_object_new_object();
    json_object_object_add(summary, "index", json_object_new_int((int)index + 1));
    json_object_object_add(summary, "active", json_object_new_boolean((int)index == active_index));
    const char *keys[] = {"id", "label", "addedAt", "lastUsed", "externalIdentifier", "usageScope",
                          "organizationId", "workspaceID"};
    for (guint key = 0; key < G_N_ELEMENTS(keys); key++) {
        json_object *value = NULL;
        if (json_object_object_get_ex(account, keys[key], &value)) {
            json_object_object_add(summary, keys[key], json_object_get(value));
        }
    }
    return summary;
}

static int run_accounts_list(int argc, char **argv) {
    const char *values[] = {"--format", "--provider"};
    const char *flags[] = {"--json", "--json-only", "--pretty"};
    char *argument_error = validate_arguments(argc, argv, values, G_N_ELEMENTS(values), flags, G_N_ELEMENTS(flags));
    if (argument_error) {
        int result = print_message_error(argc, argv, argument_error);
        g_free(argument_error);
        return result;
    }
    const CodexBarProviderDescriptor *provider = selected_provider(argc, argv);
    if (!provider || !codexbar_token_accounts_supported(provider->id)) {
        return print_message_error(argc, argv, "Unknown provider or provider does not support token accounts.");
    }
    GError *error = NULL;
    CodexBarConfig *config = load_config(&error);
    if (!config) return print_error(argc, argv, error);
    CodexBarProviderConfig *entry = codexbar_config_provider(config, provider->id);
    int active_index = 0;
    json_object *accounts = codexbar_token_accounts_array(entry, &active_index);
    size_t count = accounts ? json_object_array_length(accounts) : 0;
    if (json_output(argc, argv)) {
        json_object *output = json_object_new_object();
        json_object_object_add(output, "provider", json_object_new_string(provider->id));
        json_object_object_add(output, "activeIndex", json_object_new_int(count ? CLAMP(active_index, 0, (int)count - 1) : 0));
        json_object *items = json_object_new_array_ext((int)count);
        for (guint index = 0; index < count; index++) {
            json_object_array_add(items, account_summary(json_object_array_get_idx(accounts, index), index, active_index));
        }
        json_object_object_add(output, "accounts", items);
        puts(json_object_to_json_string_ext(
            output, has_flag(argc, argv, "--pretty") ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(output);
    } else if (!count) {
        printf("No token accounts configured for %s.\n", provider->display_name);
    } else {
        for (guint index = 0; index < count; index++) {
            json_object *account = json_object_array_get_idx(accounts, index);
            char *id = codexbar_token_account_string(account, "id");
            char *label = codexbar_token_account_string(account, "label");
            printf("%c %u. %s (%s)\n", (int)index == active_index ? '*' : ' ', index + 1,
                   label ? label : "Account", id ? id : "no id");
            g_free(id);
            g_free(label);
        }
    }
    codexbar_config_free(config);
    return 0;
}

static int save_account_change(int argc,
                               char **argv,
                               CodexBarConfig *config,
                               const CodexBarProviderDescriptor *provider,
                               const char *action,
                               const char *account_id,
                               GError *error) {
    if (error || !codexbar_config_save(config, &error)) {
        codexbar_config_free(config);
        return print_error(argc, argv, error);
    }
    if (json_output(argc, argv)) {
        json_object *output = json_object_new_object();
        json_object_object_add(output, "provider", json_object_new_string(provider->id));
        json_object_object_add(output, "action", json_object_new_string(action));
        if (account_id) json_object_object_add(output, "accountId", json_object_new_string(account_id));
        json_object_object_add(output, "configPath", json_object_new_string(config->path));
        puts(json_object_to_json_string_ext(
            output, has_flag(argc, argv, "--pretty") ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(output);
    } else {
        printf("Config: %s token account for %s\n", action, provider->display_name);
    }
    codexbar_config_free(config);
    return 0;
}

static int run_accounts_add(int argc, char **argv) {
    const char *values[] = {"--format", "--provider", "--label", "--token", "--external-id", "--usage-scope",
                            "--organization-id", "--workspace-id"};
    const char *flags[] = {"--json", "--json-only", "--pretty", "--stdin"};
    char *argument_error = validate_arguments(argc, argv, values, G_N_ELEMENTS(values), flags, G_N_ELEMENTS(flags));
    if (argument_error) {
        int result = print_message_error(argc, argv, argument_error);
        g_free(argument_error);
        return result;
    }
    const CodexBarProviderDescriptor *provider = selected_provider(argc, argv);
    if (!provider || !codexbar_token_accounts_supported(provider->id)) {
        return print_message_error(argc, argv, "Unknown provider or provider does not support token accounts.");
    }
    const char *argument = option_value(argc, argv, "--token");
    gboolean from_stdin = has_flag(argc, argv, "--stdin");
    if ((argument != NULL) == from_stdin) return print_message_error(argc, argv, "Use exactly one of --token or --stdin.");
    size_t stdin_length = 0;
    char *stdin_token = from_stdin ? read_stdin(&stdin_length) : NULL;
    (void)stdin_length;
    GError *error = NULL;
    CodexBarConfig *config = load_config_for_update(&error);
    if (!config) {
        g_free(stdin_token);
        return print_error(argc, argv, error);
    }
    char *id = NULL;
    codexbar_token_accounts_add(codexbar_config_provider(config, provider->id),
                                option_value(argc, argv, "--label"), stdin_token ? stdin_token : argument,
                                option_value(argc, argv, "--external-id"), option_value(argc, argv, "--usage-scope"),
                                option_value(argc, argv, "--organization-id"), option_value(argc, argv, "--workspace-id"),
                                &id, &error);
    g_free(stdin_token);
    int result = save_account_change(argc, argv, config, provider, "added", id, error);
    g_free(id);
    return result;
}

static int run_accounts_update(int argc, char **argv) {
    const char *values[] = {"--format", "--provider", "--account", "--label", "--token", "--external-id",
                            "--usage-scope", "--organization-id", "--workspace-id"};
    const char *flags[] = {"--json", "--json-only", "--pretty", "--stdin"};
    char *argument_error = validate_arguments(argc, argv, values, G_N_ELEMENTS(values), flags, G_N_ELEMENTS(flags));
    if (argument_error) {
        int result = print_message_error(argc, argv, argument_error);
        g_free(argument_error);
        return result;
    }
    const CodexBarProviderDescriptor *provider = selected_provider(argc, argv);
    const char *selector = option_value(argc, argv, "--account");
    if (!provider || !selector || !codexbar_token_accounts_supported(provider->id)) {
        return print_message_error(argc, argv, "A supported --provider and --account are required.");
    }
    const char *token = option_value(argc, argv, "--token");
    gboolean from_stdin = has_flag(argc, argv, "--stdin");
    if (token && from_stdin) return print_message_error(argc, argv, "Use only one of --token or --stdin.");
    size_t stdin_length = 0;
    char *stdin_token = from_stdin ? read_stdin(&stdin_length) : NULL;
    (void)stdin_length;
    GError *error = NULL;
    CodexBarConfig *config = load_config_for_update(&error);
    if (!config) {
        g_free(stdin_token);
        return print_error(argc, argv, error);
    }
    codexbar_token_accounts_update(codexbar_config_provider(config, provider->id), selector,
                                   option_value(argc, argv, "--label"), stdin_token ? stdin_token : token,
                                   option_value(argc, argv, "--external-id"), option_value(argc, argv, "--usage-scope"),
                                   option_value(argc, argv, "--organization-id"), option_value(argc, argv, "--workspace-id"),
                                   &error);
    g_free(stdin_token);
    return save_account_change(argc, argv, config, provider, "updated", NULL, error);
}

static int run_accounts_simple(int argc, char **argv, const char *action) {
    const char *values[] = {"--format", "--provider", "--account", "--index"};
    const char *flags[] = {"--json", "--json-only", "--pretty"};
    char *argument_error = validate_arguments(argc, argv, values, G_N_ELEMENTS(values), flags, G_N_ELEMENTS(flags));
    if (argument_error) {
        int result = print_message_error(argc, argv, argument_error);
        g_free(argument_error);
        return result;
    }
    const CodexBarProviderDescriptor *provider = selected_provider(argc, argv);
    const char *selector = option_value(argc, argv, "--account");
    if (!provider || !selector || !codexbar_token_accounts_supported(provider->id)) {
        return print_message_error(argc, argv, "A supported --provider and --account are required.");
    }
    GError *error = NULL;
    CodexBarConfig *config = load_config_for_update(&error);
    if (!config) return print_error(argc, argv, error);
    CodexBarProviderConfig *entry = codexbar_config_provider(config, provider->id);
    if (g_str_equal(action, "removed")) codexbar_token_accounts_remove(entry, selector, &error);
    else if (g_str_equal(action, "selected")) codexbar_token_accounts_select(entry, selector, &error);
    else {
        const char *raw_index = option_value(argc, argv, "--index");
        char *end = NULL;
        guint64 index = raw_index ? g_ascii_strtoull(raw_index, &end, 10) : 0;
        if (!raw_index || !end || *end || index == 0 || index > G_MAXUINT) {
            error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                        "Move requires a positive one-based --index.");
        } else {
            codexbar_token_accounts_move(entry, selector, (guint)index - 1, &error);
        }
    }
    return save_account_change(argc, argv, config, provider, action, NULL, error);
}

static int run_accounts(int argc, char **argv) {
    if (argc < 1) {
        fputs("Usage: codexbar-linux config accounts <list|add|update|remove|select|move>\n", stderr);
        return 1;
    }
    if (g_str_equal(argv[0], "list")) return run_accounts_list(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "add")) return run_accounts_add(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "update")) return run_accounts_update(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "remove")) return run_accounts_simple(argc - 1, argv + 1, "removed");
    if (g_str_equal(argv[0], "select")) return run_accounts_simple(argc - 1, argv + 1, "selected");
    if (g_str_equal(argv[0], "move")) return run_accounts_simple(argc - 1, argv + 1, "moved");
    fprintf(stderr, "Unknown accounts command: %s\n", argv[0]);
    return 1;
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
    size_t api_key_length = 0;
    char *stdin_key = from_stdin ? read_stdin(&api_key_length) : NULL;
    const char *api_key = stdin_key ? stdin_key : argument;
    if (!from_stdin) api_key_length = strlen(api_key);
    GError *error = NULL;
    CodexBarConfig *config = load_config_for_update(&error);
    if (!config) {
        g_free(stdin_key);
        return print_error(argc, argv, error);
    }
    gboolean enabled = !has_flag(argc, argv, "--no-enable");
    if (!codexbar_config_set_api_key(config, provider->id, api_key, api_key_length, enabled, &error) ||
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

static int run_refresh(int argc, char **argv) {
    const char *values[] = {"--format"};
    const char *flags[] = {"--json", "--json-only", "--pretty"};
    int value_count = argc > 0 && argv[0][0] != '-' ? 1 : 0;
    char *argument_error = validate_arguments(
        argc - value_count, argv + value_count, values, G_N_ELEMENTS(values), flags, G_N_ELEMENTS(flags));
    if (argument_error) {
        int result = print_message_error(argc, argv, argument_error);
        g_free(argument_error);
        return result;
    }
    if (argc - value_count > 0 && argv[value_count][0] != '-') {
        return print_message_error(argc, argv, "Only one refresh frequency may be specified.");
    }

    GError *error = NULL;
    CodexBarConfig *config = value_count ? load_config_for_update(&error) : load_config(&error);
    if (!config) return print_error(argc, argv, error);
    if (value_count) {
        CodexBarRefreshFrequency parsed = codexbar_refresh_frequency_parse(argv[0], TRUE);
        if (!g_str_equal(codexbar_refresh_frequency_raw(parsed), argv[0])) {
            codexbar_config_free(config);
            return print_message_error(
                argc,
                argv,
                "Refresh frequency must be manual, oneMinute, twoMinutes, fiveMinutes, fifteenMinutes, "
                "thirtyMinutes, adaptive, or adaptiveAgentAware.");
        }
        config->refresh_frequency = parsed;
        if (!codexbar_config_save(config, &error)) {
            codexbar_config_free(config);
            return print_error(argc, argv, error);
        }
    }
    const char *raw = codexbar_refresh_frequency_raw(config->refresh_frequency);
    if (json_output(argc, argv)) {
        json_object *object = json_object_new_object();
        json_object_object_add(object, "refreshFrequency", json_object_new_string(raw));
        json_object_object_add(object, "configPath", json_object_new_string(config->path));
        puts(json_object_to_json_string_ext(
            object, has_flag(argc, argv, "--pretty") ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(object);
    } else {
        printf("Refresh frequency: %s\n", raw);
    }
    codexbar_config_free(config);
    return 0;
}

int codexbar_cli_config_run(int argc, char **argv) {
    if (argc < 1) {
        fputs("Usage: codexbar-linux config <validate|dump|providers|enable|disable|set-api-key|accounts|refresh>\n",
              stderr);
        return 1;
    }
    if (g_str_equal(argv[0], "validate")) return run_validate(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "dump")) return run_dump(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "providers")) return run_providers(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "enable")) return run_toggle(argc - 1, argv + 1, TRUE);
    if (g_str_equal(argv[0], "disable")) return run_toggle(argc - 1, argv + 1, FALSE);
    if (g_str_equal(argv[0], "set-api-key")) return run_set_api_key(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "accounts")) return run_accounts(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "refresh")) return run_refresh(argc - 1, argv + 1);
    fprintf(stderr, "Unknown config command: %s\n", argv[0]);
    return 1;
}
