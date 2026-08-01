#include "cli_history.h"

#include "history.h"
#include "provider_registry.h"

#include <json-c/json.h>
#include <stdio.h>

static void print_help(void) {
    puts("Usage: codexbar-linux history [--provider <name>] [--format text|json] [--pretty]");
}

static const char *option_value(int argc, char **argv, int *index) {
    if (*index + 1 >= argc || argv[*index + 1][0] == '-') return NULL;
    (*index)++;
    return argv[*index];
}

static json_object *filtered_output(json_object *all, const char *provider) {
    if (!provider) return json_object_get(all);
    json_object *output = json_object_new_object();
    json_object *providers = json_object_new_object();
    json_object *stored = NULL;
    json_object *all_providers = json_object_object_get(all, "providers");
    if (json_object_object_get_ex(all_providers, provider, &stored)) {
        json_object_object_add(providers, provider, json_object_get(stored));
    }
    json_object_object_add(output, "version", json_object_new_int(1));
    json_object_object_add(output, "providers", providers);
    return output;
}

static guint history_entry_count(json_object *histories) {
    guint count = 0;
    for (size_t index = 0; index < json_object_array_length(histories); index++) {
        json_object *entries = json_object_object_get(json_object_array_get_idx(histories, index), "entries");
        if (entries && json_object_is_type(entries, json_type_array)) count += json_object_array_length(entries);
    }
    return count;
}

static void print_text(json_object *output) {
    json_object *providers = json_object_object_get(output, "providers");
    if (json_object_object_length(providers) == 0) {
        puts("No plan-utilization history recorded.");
        return;
    }
    json_object_object_foreach(providers, provider, document) {
        guint series_count = 0;
        guint entry_count = 0;
        json_object *unscoped = json_object_object_get(document, "unscoped");
        series_count += json_object_array_length(unscoped);
        entry_count += history_entry_count(unscoped);
        json_object *accounts = json_object_object_get(document, "accounts");
        json_object_object_foreach(accounts, account, histories) {
            (void)account;
            series_count += json_object_array_length(histories);
            entry_count += history_entry_count(histories);
        }
        printf("%s: %u series, %u samples\n", provider, series_count, entry_count);
    }
}

int codexbar_cli_history_run(int argc, char **argv) {
    const char *provider = NULL;
    gboolean json = FALSE;
    gboolean pretty = FALSE;
    for (int index = 0; index < argc; index++) {
        if (g_str_equal(argv[index], "--provider")) {
            const char *value = option_value(argc, argv, &index);
            if (!value) {
                fputs("Error: Missing value for --provider.\n", stderr);
                return 1;
            }
            const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(value);
            if (!descriptor) {
                fprintf(stderr, "Error: Unknown provider: %s\n", value);
                return 1;
            }
            provider = descriptor->id;
        } else if (g_str_equal(argv[index], "--format")) {
            const char *value = option_value(argc, argv, &index);
            if (!value || (!g_str_equal(value, "text") && !g_str_equal(value, "json"))) {
                fputs("Error: --format must be text or json.\n", stderr);
                return 1;
            }
            json = g_str_equal(value, "json");
        } else if (g_str_equal(argv[index], "--json")) {
            json = TRUE;
        } else if (g_str_equal(argv[index], "--pretty")) {
            pretty = TRUE;
        } else if (g_str_equal(argv[index], "--help") || g_str_equal(argv[index], "-h")) {
            print_help();
            return 0;
        } else {
            fprintf(stderr, "Error: Unknown argument: %s\n", argv[index]);
            return 1;
        }
    }
    CodexBarHistoryStore *store = codexbar_history_store_new(NULL);
    json_object *all = codexbar_history_store_load_all(store);
    json_object *output = filtered_output(all, provider);
    if (json) {
        puts(json_object_to_json_string_ext(output,
                                            pretty ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
    } else {
        print_text(output);
    }
    json_object_put(output);
    json_object_put(all);
    codexbar_history_store_free(store);
    return 0;
}
