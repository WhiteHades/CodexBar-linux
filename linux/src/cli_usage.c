#include "cli_usage.h"

#include "backend.h"
#include "provider_registry.h"
#include "render.h"

#include <json-c/json.h>
#include <stdio.h>

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

static gboolean wants_json(int argc, char **argv) {
    const char *format = option_value(argc, argv, "--format");
    if (format) return g_ascii_strcasecmp(format, "json") == 0;
    return has_flag(argc, argv, "--json") || has_flag(argc, argv, "--json-only");
}

static int print_error_kind(int argc, char **argv, const char *message, const char *kind, int code) {
    if (wants_json(argc, argv)) {
        json_object *array = json_object_new_array();
        json_object *payload = json_object_new_object();
        json_object *error = json_object_new_object();
        json_object_object_add(error, "message", json_object_new_string(message));
        json_object_object_add(error, "code", json_object_new_int(code));
        json_object_object_add(error, "kind", json_object_new_string(kind));
        json_object_object_add(payload, "provider", json_object_new_string("cli"));
        json_object_object_add(payload, "source", json_object_new_string("cli"));
        json_object_object_add(payload, "error", error);
        json_object_array_add(array, payload);
        puts(json_object_to_json_string_ext(
            array, has_flag(argc, argv, "--pretty") ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(array);
    } else {
        fprintf(stderr, "%s\n", message);
    }
    return code;
}

static int print_error(int argc, char **argv, const char *message) {
    return print_error_kind(argc, argv, message, "runtime", 1);
}

static char *validate_arguments(int argc, char **argv) {
    for (int index = 0; index < argc; index++) {
        const char *argument = argv[index];
        if (g_str_equal(argument, "--provider") || g_str_equal(argument, "--source") ||
            g_str_equal(argument, "--format")) {
            if (index + 1 >= argc || argv[index + 1][0] == '-') {
                return g_strdup_printf("Missing value for %s.", argument);
            }
            if (g_str_equal(argument, "--format") && g_ascii_strcasecmp(argv[index + 1], "text") != 0 &&
                g_ascii_strcasecmp(argv[index + 1], "json") != 0) {
                return g_strdup("--format must be text or json.");
            }
            index++;
            continue;
        }
        if (g_str_equal(argument, "--json") || g_str_equal(argument, "--json-only") ||
            g_str_equal(argument, "--pretty")) {
            continue;
        }
        return g_strdup_printf("Unknown argument: %s", argument);
    }
    return NULL;
}

static CodexBarSnapshot *snapshot_new(void) {
    CodexBarSnapshot *snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    return snapshot;
}

static gboolean add_provider(CodexBarSnapshot *snapshot, const char *provider, const char *source, GError **error) {
    CodexBarProvider *result = codexbar_backend_fetch_one(provider, source, error);
    if (!result) return FALSE;
    g_ptr_array_add(snapshot->providers, result);
    return TRUE;
}

int codexbar_cli_usage_run(int argc, char **argv) {
    char *argument_error = validate_arguments(argc, argv);
    if (argument_error) {
        int result = print_error_kind(argc, argv, argument_error, "args", 1);
        g_free(argument_error);
        return result;
    }
    const char *provider_argument = option_value(argc, argv, "--provider");
    const char *source_argument = option_value(argc, argv, "--source");
    char *provider_name = provider_argument ? g_ascii_strdown(provider_argument, -1) : NULL;
    char *source = source_argument ? g_ascii_strdown(source_argument, -1) : NULL;
    gboolean json = wants_json(argc, argv);
    GError *error = NULL;
    CodexBarSnapshot *snapshot = NULL;
    if (!provider_name) {
        if (source) {
            g_free(provider_name);
            g_free(source);
            return print_error_kind(argc, argv, "--source requires a single provider.", "args", 1);
        }
        snapshot = codexbar_backend_fetch(&error);
    } else if (g_str_equal(provider_name, "all")) {
        if (source) {
            g_free(provider_name);
            g_free(source);
            return print_error_kind(argc, argv, "--source requires a single provider.", "args", 1);
        }
        snapshot = codexbar_backend_fetch_all(&error);
    } else {
        snapshot = snapshot_new();
        if (g_str_equal(provider_name, "both")) {
            if (source) {
                codexbar_snapshot_free(snapshot);
                g_free(provider_name);
                g_free(source);
                return print_error_kind(argc, argv, "--source requires a single provider.", "args", 1);
            }
            if (!add_provider(snapshot, "codex", NULL, &error) || !add_provider(snapshot, "claude", NULL, &error)) {
                codexbar_snapshot_free(snapshot);
                print_error(argc, argv, error ? error->message : "Usage fetch failed");
                g_clear_error(&error);
                g_free(provider_name);
                g_free(source);
                return 1;
            }
        } else {
            const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(provider_name);
            if (!descriptor) {
                char *message = g_strdup_printf("Unknown provider: %s", provider_name);
                codexbar_snapshot_free(snapshot);
                int result = print_error_kind(argc, argv, message, "args", 1);
                g_free(message);
                g_free(provider_name);
                g_free(source);
                return result;
            }
            if (!add_provider(snapshot, descriptor->id, source, &error)) {
                codexbar_snapshot_free(snapshot);
                print_error(argc, argv, error ? error->message : "Unknown provider");
                g_clear_error(&error);
                g_free(provider_name);
                g_free(source);
                return 1;
            }
        }
    }
    if (!snapshot) {
        print_error(argc, argv, error ? error->message : "Usage fetch failed");
        g_clear_error(&error);
        g_free(provider_name);
        g_free(source);
        return 1;
    }

    char *output = json ? codexbar_render_usage_json(snapshot, has_flag(argc, argv, "--pretty"))
                        : codexbar_render_usage_text(snapshot);
    puts(output);
    g_free(output);
    int exit_code = 0;
    for (guint index = 0; index < snapshot->providers->len; index++) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        if (provider->error) exit_code = MAX(exit_code, provider->error_code ? provider->error_code : 1);
    }
    codexbar_snapshot_free(snapshot);
    g_free(provider_name);
    g_free(source);
    return exit_code;
}
