#define _POSIX_C_SOURCE 200809L

#include "cli_cache.h"

#include "provider_registry.h"

#include <errno.h>
#include <glib.h>
#include <glib/gstdio.h>
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    const char *cache;
    const char *provider;
    guint cleared;
    char *error;
} ClearResult;

static const char *option_value(int argc, char **argv, int *index) {
    if (*index + 1 >= argc || argv[*index + 1][0] == '-') return NULL;
    (*index)++;
    return argv[*index];
}

static void print_help(void) {
    puts("Usage: codexbar-linux cache clear <--cookies|--cost|--all>\n"
         "                                  [--provider <name>]\n"
         "                                  [--format text|json] [--json] [--pretty]\n\n"
         "--provider applies only to --cookies.");
}

static gboolean remove_tree(const char *path, gboolean *existed, char **error) {
    GStatBuf status;
    if (g_lstat(path, &status) != 0) {
        if (errno == ENOENT) return TRUE;
        *error = g_strdup_printf("%s: %s", path, g_strerror(errno));
        return FALSE;
    }
    *existed = TRUE;
    if (!S_ISDIR(status.st_mode)) {
        if (g_remove(path) == 0) return TRUE;
        *error = g_strdup_printf("%s: %s", path, g_strerror(errno));
        return FALSE;
    }

    GError *directory_error = NULL;
    GDir *directory = g_dir_open(path, 0, &directory_error);
    if (!directory) {
        *error = g_strdup(directory_error->message);
        g_error_free(directory_error);
        return FALSE;
    }
    const char *name;
    while ((name = g_dir_read_name(directory))) {
        char *child = g_build_filename(path, name, NULL);
        gboolean child_existed = FALSE;
        gboolean removed = remove_tree(child, &child_existed, error);
        g_free(child);
        if (!removed) {
            g_dir_close(directory);
            return FALSE;
        }
    }
    g_dir_close(directory);
    if (g_rmdir(path) == 0) return TRUE;
    *error = g_strdup_printf("%s: %s", path, g_strerror(errno));
    return FALSE;
}

static char *cookie_cache_directory(void) {
    return g_build_filename(g_get_user_data_dir(), "CodexBar", NULL);
}

static char *cost_cache_directory(void) {
    return g_build_filename(g_get_user_cache_dir(), "CodexBar", "cost-usage", NULL);
}

static ClearResult clear_cost_cache(void) {
    ClearResult result = {.cache = "cost"};
    char *path = cost_cache_directory();
    gboolean existed = FALSE;
    if (remove_tree(path, &existed, &result.error) && existed) result.cleared = 1;
    g_free(path);
    return result;
}

static ClearResult clear_provider_cookie_cache(const char *provider) {
    ClearResult result = {.cache = "cookies", .provider = provider};
    char *directory = cookie_cache_directory();
    char *filename = g_strdup_printf("%s-cookie.json", provider);
    char *path = g_build_filename(directory, filename, NULL);
    gboolean existed = FALSE;
    if (remove_tree(path, &existed, &result.error) && existed) result.cleared = 1;
    g_free(path);
    g_free(filename);
    g_free(directory);
    return result;
}

static ClearResult clear_all_cookie_caches(void) {
    ClearResult result = {.cache = "cookies"};
    char *path = cookie_cache_directory();
    GError *directory_error = NULL;
    GDir *directory = g_dir_open(path, 0, &directory_error);
    if (!directory) {
        if (!g_error_matches(directory_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            result.error = g_strdup(directory_error->message);
        }
        g_error_free(directory_error);
        g_free(path);
        return result;
    }
    const char *name;
    while ((name = g_dir_read_name(directory))) {
        if (!g_str_has_suffix(name, "-cookie.json")) continue;
        char *entry = g_build_filename(path, name, NULL);
        gboolean existed = FALSE;
        char *error = NULL;
        if (remove_tree(entry, &existed, &error) && existed) {
            result.cleared++;
        } else if (error && !result.error) {
            result.error = error;
            error = NULL;
        }
        g_free(error);
        g_free(entry);
    }
    g_dir_close(directory);
    g_free(path);
    return result;
}

static void print_text_result(const ClearResult *result) {
    const char *scope = result->provider ? result->provider : "all providers";
    if (result->error) {
        printf("%s: failed to clear (%s) - %s\n", result->cache, scope, result->error);
    } else if (result->cleared > 0) {
        printf("%s: cleared (%s)\n", result->cache, scope);
    } else {
        printf("%s: nothing to clear (%s)\n", result->cache, scope);
    }
}

static json_object *result_json(const ClearResult *result) {
    json_object *object = json_object_new_object();
    json_object_object_add(object, "cache", json_object_new_string(result->cache));
    if (result->provider) json_object_object_add(object, "provider", json_object_new_string(result->provider));
    json_object_object_add(object, "cleared", json_object_new_int64(result->cleared));
    if (result->error) json_object_object_add(object, "error", json_object_new_string(result->error));
    return object;
}

int codexbar_cli_cache_run(int argc, char **argv) {
    if (argc == 0 || g_str_equal(argv[0], "--help") || g_str_equal(argv[0], "-h")) {
        print_help();
        return argc == 0 ? 1 : 0;
    }
    if (!g_str_equal(argv[0], "clear")) {
        fprintf(stderr, "Error: Unknown cache command: %s\n", argv[0]);
        return 1;
    }

    gboolean cookies = FALSE;
    gboolean cost = FALSE;
    gboolean all = FALSE;
    gboolean format_json = FALSE;
    gboolean json_shortcut = FALSE;
    gboolean pretty = FALSE;
    const char *provider_name = NULL;
    for (int index = 1; index < argc; index++) {
        const char *argument = argv[index];
        if (g_str_equal(argument, "--cookies")) {
            cookies = TRUE;
        } else if (g_str_equal(argument, "--cost")) {
            cost = TRUE;
        } else if (g_str_equal(argument, "--all")) {
            all = TRUE;
        } else if (g_str_equal(argument, "--provider")) {
            provider_name = option_value(argc, argv, &index);
            if (!provider_name) {
                fputs("Error: Missing value for --provider.\n", stderr);
                return 1;
            }
        } else if (g_str_equal(argument, "--format")) {
            const char *format = option_value(argc, argv, &index);
            if (!format || (!g_str_equal(format, "text") && !g_str_equal(format, "json"))) {
                fputs("Error: --format must be text or json.\n", stderr);
                return 1;
            }
            format_json = g_str_equal(format, "json");
        } else if (g_str_equal(argument, "--json") || g_str_equal(argument, "--json-only")) {
            json_shortcut = TRUE;
        } else if (g_str_equal(argument, "--pretty")) {
            pretty = TRUE;
        } else if (g_str_equal(argument, "--log-level")) {
            if (!option_value(argc, argv, &index)) {
                fputs("Error: Missing value for --log-level.\n", stderr);
                return 1;
            }
        } else if (g_str_equal(argument, "--json-output") || g_str_equal(argument, "--verbose") ||
                   g_str_equal(argument, "-v")) {
            continue;
        } else if (g_str_equal(argument, "--help") || g_str_equal(argument, "-h")) {
            print_help();
            return 0;
        } else {
            fprintf(stderr, "Error: Unknown argument: %s\n", argument);
            return 1;
        }
    }

    gboolean clear_cookies = cookies || all;
    gboolean clear_cost = cost || all;
    if (!clear_cookies && !clear_cost) {
        fputs("Error: Specify --cookies, --cost, or --all.\n", stderr);
        return 1;
    }
    if (provider_name && clear_cost) {
        fputs("Error: --provider only scopes cookie caches. Use --cookies --provider <name>, or omit --provider.\n",
              stderr);
        return 1;
    }

    const char *provider = NULL;
    if (provider_name) {
        char *lower = g_ascii_strdown(provider_name, -1);
        const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(lower);
        g_free(lower);
        if (!descriptor) {
            fprintf(stderr, "Error: Unknown provider: %s\n", provider_name);
            return 1;
        }
        provider = descriptor->id;
    }

    ClearResult results[2] = {0};
    guint count = 0;
    if (clear_cookies) {
        results[count++] = provider ? clear_provider_cookie_cache(provider) : clear_all_cookie_caches();
    }
    if (clear_cost) results[count++] = clear_cost_cache();

    if (format_json || json_shortcut) {
        json_object *array = json_object_new_array();
        for (guint index = 0; index < count; index++) json_object_array_add(array, result_json(&results[index]));
        puts(json_object_to_json_string_ext(array, pretty ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(array);
    } else {
        for (guint index = 0; index < count; index++) print_text_result(&results[index]);
    }

    int exit_code = 0;
    for (guint index = 0; index < count; index++) {
        if (results[index].error) exit_code = 1;
        g_free(results[index].error);
    }
    return exit_code;
}
