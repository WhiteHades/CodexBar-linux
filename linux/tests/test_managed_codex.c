#include "managed_codex.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <json-c/json.h>
#include <sys/stat.h>

static char *test_root;
static char *store_path;
static char *homes_path;

static char *base64url(const char *value) {
    char *encoded = g_base64_encode((const guchar *)value, strlen(value));
    for (char *cursor = encoded; *cursor; cursor++) {
        if (*cursor == '+') *cursor = '-';
        if (*cursor == '/') *cursor = '_';
    }
    char *padding = strchr(encoded, '=');
    if (padding) *padding = '\0';
    return encoded;
}

static char *auth_json(const char *email, const char *account_id) {
    char *header = base64url("{\"alg\":\"none\"}");
    char *payload_json = g_strdup_printf(
        "{\"email\":\"%s\",\"https://api.openai.com/auth\":{\"chatgpt_account_id\":\"%s\"}}",
        email,
        account_id);
    char *payload = base64url(payload_json);
    char *token = g_strdup_printf("%s.%s.", header, payload);
    json_object *root = json_object_new_object();
    json_object *tokens = json_object_new_object();
    json_object_object_add(tokens, "access_token", json_object_new_string("access"));
    json_object_object_add(tokens, "refresh_token", json_object_new_string("refresh"));
    json_object_object_add(tokens, "id_token", json_object_new_string(token));
    json_object_object_add(root, "tokens", tokens);
    char *result = g_strdup(json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
    json_object_put(root);
    g_free(token);
    g_free(payload);
    g_free(payload_json);
    g_free(header);
    return result;
}

static char *write_auth(const char *name, const char *email, const char *account_id) {
    char *path = g_build_filename(test_root, name, NULL);
    char *contents = auth_json(email, account_id);
    g_assert_true(g_file_set_contents(path, contents, -1, NULL));
    g_free(contents);
    return path;
}

static void test_import_round_trip(void) {
    char *auth = write_auth("auth.json", "  USER@Example.COM ", "Workspace-Team");
    GError *error = NULL;
    CodexBarManagedCodexStore *store = codexbar_managed_codex_store_load(TRUE, &error);
    g_assert_no_error(error);
    CodexBarManagedCodexAccount *account = codexbar_managed_codex_import(store, auth, &error);
    g_assert_no_error(error);
    g_assert_nonnull(account);
    g_assert_cmpstr(account->email, ==, "user@example.com");
    g_assert_cmpstr(account->provider_account_id, ==, "workspace-team");
    g_assert_true(g_str_has_prefix(account->managed_home_path, homes_path));
    g_assert_true(g_file_test(account->managed_home_path, G_FILE_TEST_IS_DIR));
    char *first_id = g_strdup(account->id);
    char *first_home = g_strdup(account->managed_home_path);
    codexbar_managed_codex_store_free(store);

    GStatBuf status;
    g_assert_cmpint(g_stat(store_path, &status), ==, 0);
    g_assert_cmpuint(status.st_mode & 0777, ==, 0600);
    store = codexbar_managed_codex_store_load(FALSE, &error);
    g_assert_no_error(error);
    GPtrArray *accounts = codexbar_managed_codex_accounts(store);
    g_assert_cmpuint(accounts->len, ==, 1);
    account = g_ptr_array_index(accounts, 0);
    g_assert_cmpstr(account->id, ==, first_id);
    g_assert_cmpuint(strlen(account->auth_fingerprint), ==, 64);
    codexbar_managed_codex_store_free(store);

    store = codexbar_managed_codex_store_load(TRUE, &error);
    account = codexbar_managed_codex_import(store, auth, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(account->id, ==, first_id);
    g_assert_cmpstr(account->managed_home_path, !=, first_home);
    g_assert_false(g_file_test(first_home, G_FILE_TEST_EXISTS));
    codexbar_managed_codex_store_free(store);
    g_free(first_home);
    g_free(first_id);
    g_free(auth);
}

static void test_remove_does_not_delete_unmanaged_path(void) {
    char *outside = g_build_filename(test_root, "outside", NULL);
    g_assert_cmpint(g_mkdir_with_parents(outside, 0700), ==, 0);
    char *marker = g_build_filename(outside, "marker", NULL);
    g_assert_true(g_file_set_contents(marker, "keep", -1, NULL));
    char *json = g_strdup_printf(
        "{\"version\":3,\"accounts\":[{\"id\":\"11111111-1111-1111-1111-111111111111\","
        "\"email\":\"outside@example.com\",\"managedHomePath\":\"%s\",\"createdAt\":1,\"updatedAt\":1}]}",
        outside);
    g_assert_true(g_file_set_contents(store_path, json, -1, NULL));
    g_free(json);
    GError *error = NULL;
    CodexBarManagedCodexStore *store = codexbar_managed_codex_store_load(TRUE, &error);
    g_assert_no_error(error);
    g_assert_true(codexbar_managed_codex_remove(store, "outside@example.com", &error));
    g_assert_no_error(error);
    g_assert_true(g_file_test(marker, G_FILE_TEST_EXISTS));
    codexbar_managed_codex_store_free(store);
    g_free(marker);
    g_free(outside);
}

static void test_rejects_future_version(void) {
    g_assert_true(g_file_set_contents(store_path, "{\"version\":999,\"accounts\":[]}", -1, NULL));
    GError *error = NULL;
    CodexBarManagedCodexStore *store = codexbar_managed_codex_store_load(FALSE, &error);
    g_assert_null(store);
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "Unsupported"));
    g_clear_error(&error);
}

static void test_reauthenticate_rejects_different_identity(void) {
    char *first = write_auth("first-auth.json", "first@example.com", "workspace-first");
    char *second = write_auth("second-auth.json", "second@example.com", "workspace-second");
    GError *error = NULL;
    CodexBarManagedCodexStore *store = codexbar_managed_codex_store_load(TRUE, &error);
    g_assert_no_error(error);
    CodexBarManagedCodexAccount *account = codexbar_managed_codex_import(store, first, &error);
    g_assert_no_error(error);
    g_assert_nonnull(account);
    char *id = g_strdup(account->id);
    char *home = g_strdup(account->managed_home_path);
    account = codexbar_managed_codex_reauthenticate(store, id, second, &error);
    g_assert_null(account);
    g_assert_error(error, g_quark_from_static_string("codexbar-managed-codex-error"), 10);
    g_clear_error(&error);
    account = codexbar_managed_codex_find(store, id);
    g_assert_nonnull(account);
    g_assert_cmpstr(account->managed_home_path, ==, home);
    g_assert_true(g_file_test(home, G_FILE_TEST_IS_DIR));
    codexbar_managed_codex_store_free(store);
    g_free(home);
    g_free(id);
    g_free(second);
    g_free(first);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    char *cwd = g_get_current_dir();
    char *identifier = g_uuid_string_random();
    test_root = g_build_filename(cwd, ".tmp", identifier, NULL);
    store_path = g_build_filename(test_root, "managed.json", NULL);
    homes_path = g_build_filename(test_root, "homes", NULL);
    g_assert_cmpint(g_mkdir_with_parents(test_root, 0700), ==, 0);
    g_setenv("CODEXBAR_MANAGED_CODEX_STORE", store_path, TRUE);
    g_setenv("CODEXBAR_MANAGED_CODEX_ROOT", homes_path, TRUE);
    g_test_add_func("/managed-codex/import-round-trip", test_import_round_trip);
    g_test_add_func("/managed-codex/unmanaged-path", test_remove_does_not_delete_unmanaged_path);
    g_test_add_func("/managed-codex/reauth-identity", test_reauthenticate_rejects_different_identity);
    g_test_add_func("/managed-codex/future-version", test_rejects_future_version);
    int result = g_test_run();
    char *command[] = {"rm", "-rf", test_root, NULL};
    g_spawn_sync(NULL, command, NULL, G_SPAWN_SEARCH_PATH, NULL, NULL, NULL, NULL, NULL, NULL);
    g_free(homes_path);
    g_free(store_path);
    g_free(test_root);
    g_free(identifier);
    g_free(cwd);
    return result;
}
