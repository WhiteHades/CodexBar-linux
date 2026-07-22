#include "cli_diagnose.h"

#include "config.h"
#include "diagnose.h"
#include "provider_registry.h"

#include <errno.h>
#include <glib.h>
#include <json-c/json.h>
#include <stdio.h>

static const char *option_value(int argc, char **argv, int *index) {
    if (*index + 1 >= argc || argv[*index + 1][0] == '-') return NULL;
    (*index)++;
    return argv[*index];
}

static void print_help(void) {
    puts("Usage: codexbar-linux diagnose --provider <name|all> --format json\n"
         "                               [--redact] [--output <path>] [--pretty]\n\n"
         "Diagnostic output is always redacted.");
}

static gboolean append_descriptor(GArray *selection, const CodexBarProviderDescriptor *descriptor) {
    if (!descriptor) return FALSE;
    for (guint index = 0; index < selection->len; index++) {
        if (g_array_index(selection, const CodexBarProviderDescriptor *, index) == descriptor) return TRUE;
    }
    g_array_append_val(selection, descriptor);
    return TRUE;
}

static GArray *provider_selection(const char *raw, CodexBarConfig *config) {
    GArray *selection = g_array_new(FALSE, FALSE, sizeof(const CodexBarProviderDescriptor *));
    if (raw && (g_ascii_strcasecmp(raw, "all") == 0)) {
        for (guint index = 0; index < codexbar_provider_registry_count(); index++) {
            append_descriptor(selection, codexbar_provider_registry_at(index));
        }
        return selection;
    }
    if (raw && g_ascii_strcasecmp(raw, "both") == 0) {
        append_descriptor(selection, codexbar_provider_registry_find("codex"));
        append_descriptor(selection, codexbar_provider_registry_find("claude"));
        return selection;
    }
    if (raw) {
        char *lower = g_ascii_strdown(raw, -1);
        append_descriptor(selection, codexbar_provider_registry_find(lower));
        g_free(lower);
        return selection;
    }
    for (guint index = 0; index < config->providers->len; index++) {
        CodexBarProviderConfig *provider = g_ptr_array_index(config->providers, index);
        if (provider->enabled) append_descriptor(selection, codexbar_provider_registry_find(provider->id));
    }
    return selection;
}

static char *batch_timestamp(void) {
    GDateTime *date = g_date_time_new_now_utc();
    char *text = g_date_time_format(date, "%Y-%m-%dT%H:%M:%SZ");
    g_date_time_unref(date);
    return text;
}

static gboolean write_export(const char *path, const char *contents, GError **error) {
    char *parent = g_path_get_dirname(path);
    if (!g_str_equal(parent, ".") && g_mkdir_with_parents(parent, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "%s: %s", parent, g_strerror(errno));
        g_free(parent);
        return FALSE;
    }
    g_free(parent);
    return g_file_set_contents(path, contents, -1, error);
}

int codexbar_cli_diagnose_run(int argc, char **argv) {
    const char *provider_name = NULL;
    const char *format = NULL;
    const char *output_path = NULL;
    gboolean pretty = FALSE;
    for (int index = 0; index < argc; index++) {
        const char *argument = argv[index];
        if (g_str_equal(argument, "--provider")) {
            provider_name = option_value(argc, argv, &index);
            if (!provider_name) {
                fputs("Error: Missing value for --provider.\n", stderr);
                return 1;
            }
        } else if (g_str_equal(argument, "--format")) {
            format = option_value(argc, argv, &index);
            if (!format) {
                fputs("Error: Missing value for --format.\n", stderr);
                return 1;
            }
        } else if (g_str_equal(argument, "--output")) {
            output_path = option_value(argc, argv, &index);
            if (!output_path) {
                fputs("Error: Missing value for --output.\n", stderr);
                return 1;
            }
        } else if (g_str_equal(argument, "--pretty")) {
            pretty = TRUE;
        } else if (g_str_equal(argument, "--log-level")) {
            if (!option_value(argc, argv, &index)) {
                fputs("Error: Missing value for --log-level.\n", stderr);
                return 1;
            }
        } else if (g_str_equal(argument, "--redact") || g_str_equal(argument, "--json-output") ||
                   g_str_equal(argument, "--verbose") || g_str_equal(argument, "-v")) {
            continue;
        } else if (g_str_equal(argument, "--help") || g_str_equal(argument, "-h")) {
            print_help();
            return 0;
        } else {
            fprintf(stderr, "Error: Unknown argument: %s\n", argument);
            return 1;
        }
    }
    if (!format || !g_str_equal(format, "json")) {
        fputs("Error: only JSON format is supported for diagnose\n", stderr);
        return 1;
    }

    GError *error = NULL;
    CodexBarConfig *config = codexbar_config_load(&error);
    if (!config) {
        fprintf(stderr, "Error: Could not load config: %s\n", error ? error->message : "unknown error");
        g_clear_error(&error);
        return 1;
    }
    GArray *selection = provider_selection(provider_name, config);
    if (provider_name && selection->len == 0) {
        fprintf(stderr, "Error: unknown provider '%s'\n", provider_name);
        g_array_unref(selection);
        codexbar_config_free(config);
        return 1;
    }

    json_object *diagnostics = json_object_new_array();
    for (guint index = 0; index < selection->len; index++) {
        const CodexBarProviderDescriptor *descriptor =
            g_array_index(selection, const CodexBarProviderDescriptor *, index);
        CodexBarProviderConfig *provider_config = codexbar_config_provider(config, descriptor->id);
        json_object_array_add(diagnostics, codexbar_diagnose_provider(descriptor, provider_config));
    }

    json_object *root;
    if (selection->len == 1) {
        root = json_object_get(json_object_array_get_idx(diagnostics, 0));
    } else {
        root = json_object_new_object();
        char *timestamp = batch_timestamp();
        json_object_object_add(root, "schemaVersion", json_object_new_string("1.0"));
        json_object_object_add(root, "timestamp", json_object_new_string(timestamp));
        g_free(timestamp);
        json_object_object_add(root, "diagnostics", json_object_get(diagnostics));
    }
    const char *contents = json_object_to_json_string_ext(
        root,
        pretty ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN);
    int exit_code = 0;
    if (output_path && output_path[0] != '\0') {
        if (!write_export(output_path, contents, &error)) {
            fprintf(stderr, "Error encoding diagnostic: %s\n", error ? error->message : "write failed");
            g_clear_error(&error);
            exit_code = 1;
        }
    } else {
        puts(contents);
    }

    json_object_put(root);
    json_object_put(diagnostics);
    g_array_unref(selection);
    codexbar_config_free(config);
    return exit_code;
}
