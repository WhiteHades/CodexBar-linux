#include "cli_managed_codex.h"

#include "managed_codex.h"
#include "process.h"

#include <errno.h>
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int print_error(GError *error) {
    fprintf(stderr, "Error: %s\n", error ? error->message : "Managed Codex account operation failed");
    g_clear_error(&error);
    return 1;
}

static json_object *account_json(const CodexBarManagedCodexAccount *account) {
    json_object *object = json_object_new_object();
    json_object_object_add(object, "id", json_object_new_string(account->id));
    json_object_object_add(object, "email", json_object_new_string(account->email));
    json_object_object_add(object,
                           "providerAccountID",
                           account->provider_account_id ? json_object_new_string(account->provider_account_id) : NULL);
    json_object_object_add(object,
                           "workspaceLabel",
                           account->workspace_label ? json_object_new_string(account->workspace_label) : NULL);
    json_object_object_add(object,
                           "workspaceAccountID",
                           account->workspace_account_id
                               ? json_object_new_string(account->workspace_account_id)
                               : NULL);
    json_object_object_add(object, "managedHomePath", json_object_new_string(account->managed_home_path));
    json_object_object_add(object, "createdAt", json_object_new_double(account->created_at));
    json_object_object_add(object, "updatedAt", json_object_new_double(account->updated_at));
    return object;
}

static int list_accounts(int argc, char **argv) {
    gboolean json = FALSE;
    gboolean pretty = FALSE;
    for (int index = 0; index < argc; index++) {
        if (g_str_equal(argv[index], "--json")) json = TRUE;
        else if (g_str_equal(argv[index], "--pretty")) pretty = TRUE;
        else {
            fprintf(stderr, "Unknown argument: %s\n", argv[index]);
            return 1;
        }
    }
    GError *error = NULL;
    CodexBarManagedCodexStore *store = codexbar_managed_codex_store_load(FALSE, &error);
    if (!store) return print_error(error);
    GPtrArray *accounts = codexbar_managed_codex_accounts(store);
    if (json) {
        json_object *array = json_object_new_array_ext((int)accounts->len);
        for (guint index = 0; index < accounts->len; index++) {
            json_object_array_add(array, account_json(g_ptr_array_index(accounts, index)));
        }
        puts(json_object_to_json_string_ext(array, pretty ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(array);
    } else if (accounts->len == 0) {
        puts("No managed Codex accounts.");
    } else {
        puts("EMAIL  ACCOUNT  ID");
        for (guint index = 0; index < accounts->len; index++) {
            CodexBarManagedCodexAccount *account = g_ptr_array_index(accounts, index);
            printf("%s  %s  %s\n",
                   account->email,
                   account->provider_account_id ? account->provider_account_id : "-",
                   account->id);
        }
    }
    codexbar_managed_codex_store_free(store);
    return 0;
}

static int import_account(int argc, char **argv) {
    if (argc != 1 || argv[0][0] == '\0') {
        fputs("Usage: codexbar-linux codex-accounts import <auth.json>\n", stderr);
        return 1;
    }
    char *path = g_canonicalize_filename(argv[0], NULL);
    GError *error = NULL;
    CodexBarManagedCodexStore *store = codexbar_managed_codex_store_load(TRUE, &error);
    CodexBarManagedCodexAccount *account =
        store ? codexbar_managed_codex_import(store, path, &error) : NULL;
    g_free(path);
    if (!account) {
        codexbar_managed_codex_store_free(store);
        return print_error(error);
    }
    printf("Imported managed Codex account %s (%s).\n", account->email, account->id);
    codexbar_managed_codex_store_free(store);
    return 0;
}

static int login_account(int argc, char **argv) {
    guint timeout_seconds = 120;
    if (argc == 2 && g_str_equal(argv[0], "--timeout")) {
        char *end = NULL;
        guint64 parsed = g_ascii_strtoull(argv[1], &end, 10);
        if (!end || *end != '\0' || parsed == 0 || parsed > 1800) {
            fputs("Error: --timeout must be between 1 and 1800 seconds.\n", stderr);
            return 1;
        }
        timeout_seconds = (guint)parsed;
    } else if (argc != 0) {
        fputs("Usage: codexbar-linux codex-accounts login [--timeout <seconds>]\n", stderr);
        return 1;
    }
    GError *error = NULL;
    CodexBarManagedCodexStore *store = codexbar_managed_codex_store_load(TRUE, &error);
    char *home = store ? codexbar_managed_codex_create_home(store, &error) : NULL;
    if (!home) {
        codexbar_managed_codex_store_free(store);
        return print_error(error);
    }
    const char *binary = g_getenv("CODEX_CLI_PATH");
    if (!binary || binary[0] == '\0') binary = "codex";
    const char *arguments[] = {binary, "login", NULL};
    char **environment = g_get_environ();
    environment = g_environ_setenv(environment, "CODEX_HOME", home, TRUE);
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .environment = (const char *const *)environment,
        .timeout_milliseconds = timeout_seconds * 1000,
        .termination_grace_milliseconds = 2000,
        .maximum_output_bytes = 4000,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, &error);
    g_strfreev(environment);
    char *auth_file = g_build_filename(home, "auth.json", NULL);
    CodexBarManagedCodexAccount *account = result && codexbar_process_result_succeeded(result)
                                                ? codexbar_managed_codex_import(store, auth_file, &error)
                                                : NULL;
    if (!account && result && !error) {
        g_set_error(&error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Codex login failed with status %d: %.4000s",
                    result->exit_status,
                    result->standard_error_length > 0 ? result->standard_error : result->standard_output);
    }
    codexbar_managed_codex_discard_home(store, home, NULL);
    g_free(auth_file);
    g_free(home);
    codexbar_process_result_free(result);
    if (!account) {
        codexbar_managed_codex_store_free(store);
        return print_error(error);
    }
    printf("Authenticated managed Codex account %s (%s).\n", account->email, account->id);
    codexbar_managed_codex_store_free(store);
    return 0;
}

static int remove_account(int argc, char **argv) {
    if (argc != 1 || argv[0][0] == '\0') {
        fputs("Usage: codexbar-linux codex-accounts remove <id|email>\n", stderr);
        return 1;
    }
    GError *error = NULL;
    CodexBarManagedCodexStore *store = codexbar_managed_codex_store_load(TRUE, &error);
    if (!store || !codexbar_managed_codex_remove(store, argv[0], &error)) {
        codexbar_managed_codex_store_free(store);
        return print_error(error);
    }
    puts("Removed managed Codex account.");
    codexbar_managed_codex_store_free(store);
    return 0;
}

static int execute_account(int argc, char **argv) {
    if (argc < 1 || argv[0][0] == '\0') {
        fputs("Usage: codexbar-linux codex-accounts exec <id|email> [--] [command ...]\n", stderr);
        return 1;
    }
    GError *error = NULL;
    CodexBarManagedCodexStore *store = codexbar_managed_codex_store_load(FALSE, &error);
    if (!store) return print_error(error);
    CodexBarManagedCodexAccount *account = codexbar_managed_codex_find(store, argv[0]);
    if (!account) {
        codexbar_managed_codex_store_free(store);
        fputs("Error: managed Codex account was not found or is ambiguous.\n", stderr);
        return 1;
    }
    char *home = g_strdup(account->managed_home_path);
    codexbar_managed_codex_store_free(store);
    int command_index = argc > 1 && g_str_equal(argv[1], "--") ? 2 : 1;
    const char *configured_command = g_getenv("CODEX_CLI_PATH");
    char *default_command[] = {
        (char *)(configured_command && configured_command[0] != '\0' ? configured_command : "codex"),
        NULL,
    };
    char **command = command_index < argc ? argv + command_index : default_command;
    if (!g_setenv("CODEX_HOME", home, TRUE)) {
        g_free(home);
        fputs("Error: could not set CODEX_HOME.\n", stderr);
        return 1;
    }
    g_free(home);
    execvp(command[0], command);
    fprintf(stderr, "Error: could not execute %s: %s\n", command[0], g_strerror(errno));
    return 1;
}

int codexbar_cli_managed_codex_run(int argc, char **argv) {
    if (argc < 1) {
        fputs("Usage: codexbar-linux codex-accounts <list|login|import|remove|exec>\n", stderr);
        return 1;
    }
    if (g_str_equal(argv[0], "list")) return list_accounts(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "login")) return login_account(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "import")) return import_account(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "remove")) return remove_account(argc - 1, argv + 1);
    if (g_str_equal(argv[0], "exec")) return execute_account(argc - 1, argv + 1);
    fprintf(stderr, "Unknown codex-accounts command: %s\n", argv[0]);
    return 1;
}
