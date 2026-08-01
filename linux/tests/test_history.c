#include "history.h"

#include <glib/gstdio.h>
#include <string.h>

static CodexBarSnapshot *snapshot(const char *account,
                                  double session_used,
                                  gint64 session_reset,
                                  double weekly_used) {
    CodexBarSnapshot *result = g_new0(CodexBarSnapshot, 1);
    result->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("codex");
    provider->account = g_strdup(account);
    CodexBarQuotaWindow *session = codexbar_quota_window_new("primary", "5-hour");
    session->usage_known = TRUE;
    session->used_percent = session_used;
    session->has_window_minutes = TRUE;
    session->window_minutes = 298;
    session->has_resets_at = TRUE;
    session->resets_at_ms = session_reset;
    codexbar_provider_add_quota_window(provider, session);
    CodexBarQuotaWindow *weekly = codexbar_quota_window_new("secondary", "Weekly");
    weekly->usage_known = TRUE;
    weekly->used_percent = weekly_used;
    weekly->has_window_minutes = TRUE;
    weekly->window_minutes = 10075;
    codexbar_provider_add_quota_window(provider, weekly);
    g_ptr_array_add(result->providers, provider);
    return result;
}

static char *temporary_directory(void) {
    g_assert_cmpint(g_mkdir_with_parents(".tmp", 0700), ==, 0);
    char *directory = g_strdup(".tmp/codexbar-history-XXXXXX");
    g_assert_nonnull(g_mkdtemp(directory));
    return directory;
}

static void remove_directory(char *directory) {
    GDir *entries = g_dir_open(directory, 0, NULL);
    if (entries) {
        const char *name = NULL;
        while ((name = g_dir_read_name(entries))) {
            char *path = g_build_filename(directory, name, NULL);
            g_assert_cmpint(g_remove(path), ==, 0);
            g_free(path);
        }
        g_dir_close(entries);
    }
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_free(directory);
}

static json_object *account_histories(json_object *document, const char *preferred) {
    json_object *accounts = json_object_object_get(document, "accounts");
    json_object *histories = NULL;
    g_assert_true(json_object_object_get_ex(accounts, preferred, &histories));
    return histories;
}

static json_object *series(json_object *histories, const char *name) {
    for (size_t index = 0; index < json_object_array_length(histories); index++) {
        json_object *value = json_object_array_get_idx(histories, index);
        if (g_str_equal(json_object_get_string(json_object_object_get(value, "name")), name)) return value;
    }
    return NULL;
}

static void test_history_is_canonical_account_scoped_and_reset_aware(void) {
    char *directory = temporary_directory();
    CodexBarHistoryStore *store = codexbar_history_store_new(directory);
    gint64 hour = 1800000000000LL;
    CodexBarSnapshot *first = snapshot("User@Example.COM", 10, hour + 7200000, 30);
    g_assert_true(codexbar_history_store_record(store, first, hour, FALSE, NULL));
    codexbar_snapshot_free(first);
    CodexBarSnapshot *second = snapshot("user@example.com", 20, hour + 7200000, 35);
    g_assert_true(codexbar_history_store_record(store, second, hour + 600000, FALSE, NULL));
    codexbar_snapshot_free(second);
    CodexBarSnapshot *reset = snapshot("user@example.com", 5, hour + 14400000, 36);
    g_assert_true(codexbar_history_store_record(store, reset, hour + 1200000, FALSE, NULL));
    codexbar_snapshot_free(reset);

    GError *error = NULL;
    json_object *document = codexbar_history_store_load_provider(store, "codex", &error);
    g_assert_no_error(error);
    g_assert_nonnull(document);
    g_assert_cmpint(json_object_get_int(json_object_object_get(document, "version")), ==, 1);
    const char *preferred = json_object_get_string(json_object_object_get(document, "preferredAccountKey"));
    g_assert_nonnull(preferred);
    g_assert_true(g_str_has_prefix(preferred, "codex:v1:email-hash:"));
    json_object *session = series(account_histories(document, preferred), "session");
    g_assert_nonnull(session);
    g_assert_cmpint(json_object_get_int(json_object_object_get(session, "windowMinutes")), ==, 300);
    json_object *entries = json_object_object_get(session, "entries");
    g_assert_cmpuint(json_object_array_length(entries), ==, 2);
    g_assert_cmpfloat(json_object_get_double(
                          json_object_object_get(json_object_array_get_idx(entries, 0), "usedPercent")),
                      ==,
                      20);
    g_assert_cmpfloat(json_object_get_double(
                          json_object_object_get(json_object_array_get_idx(entries, 1), "usedPercent")),
                      ==,
                      5);
    json_object *weekly = series(account_histories(document, preferred), "weekly");
    g_assert_cmpint(json_object_get_int(json_object_object_get(weekly, "windowMinutes")), ==, 10080);
    json_object_put(document);

    char *path = g_build_filename(directory, "codex.json", NULL);
    char *contents = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, NULL));
    g_assert_null(strstr(contents, "user@example.com"));
    g_assert_null(strstr(contents, "sessionEquivalentWindowPairIdentities"));
    g_free(contents);
    g_free(path);
    codexbar_history_store_free(store);
    remove_directory(directory);
}

static void test_history_prefers_provider_account_identity(void) {
    char *directory = temporary_directory();
    CodexBarHistoryStore *store = codexbar_history_store_new(directory);
    CodexBarSnapshot *value = snapshot("user@example.com", 10, 1800007200000LL, 20);
    CodexBarProvider *provider = g_ptr_array_index(value->providers, 0);
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->account_id = g_strdup(" acct-123 ");
    g_assert_true(codexbar_history_store_record(store, value, 1800000000000LL, FALSE, NULL));
    codexbar_snapshot_free(value);

    json_object *document = codexbar_history_store_load_provider(store, "codex", NULL);
    g_assert_cmpstr(json_object_get_string(json_object_object_get(document, "preferredAccountKey")),
                    ==,
                    "codex:v1:provider-account:acct-123");
    json_object_put(document);
    codexbar_history_store_free(store);
    remove_directory(directory);
}

static void test_history_merges_equivalent_persisted_series(void) {
    char *directory = temporary_directory();
    char *path = g_build_filename(directory, "codex.json", NULL);
    char *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, "user@example.com", -1);
    char *key = g_strdup_printf("codex:v1:email-hash:%s", digest);
    char *document = g_strdup_printf(
        "{\"version\":1,\"preferredAccountKey\":\"%s\","
        "\"unscoped\":[],\"accounts\":{\"%s\":["
        "{\"name\":\"session\",\"windowMinutes\":298,\"entries\":["
        "{\"capturedAt\":\"2027-01-15T08:00:00.000000Z\",\"usedPercent\":10,\"resetsAt\":null}]},"
        "{\"name\":\"session\",\"windowMinutes\":302,\"entries\":["
        "{\"capturedAt\":\"2027-01-15T09:00:00.000000Z\",\"usedPercent\":20,\"resetsAt\":null}]}]}}",
        key,
        key);
    g_assert_true(g_file_set_contents(path, document, -1, NULL));
    g_free(document);
    g_free(key);
    g_free(digest);

    CodexBarHistoryStore *store = codexbar_history_store_new(directory);
    CodexBarSnapshot *value = snapshot("user@example.com", 30, 1800014400000LL, 40);
    g_assert_true(codexbar_history_store_record(store, value, 1800007200000LL, FALSE, NULL));
    codexbar_snapshot_free(value);

    json_object *loaded = codexbar_history_store_load_provider(store, "codex", NULL);
    const char *preferred = json_object_get_string(json_object_object_get(loaded, "preferredAccountKey"));
    json_object *histories = account_histories(loaded, preferred);
    guint session_count = 0;
    json_object *session_value = NULL;
    for (size_t index = 0; index < json_object_array_length(histories); index++) {
        json_object *candidate = json_object_array_get_idx(histories, index);
        if (g_str_equal(json_object_get_string(json_object_object_get(candidate, "name")), "session")) {
            session_count++;
            session_value = candidate;
        }
    }
    g_assert_cmpuint(session_count, ==, 1);
    g_assert_cmpint(json_object_get_int(json_object_object_get(session_value, "windowMinutes")), ==, 300);
    g_assert_cmpuint(json_object_array_length(json_object_object_get(session_value, "entries")), ==, 3);
    json_object_put(loaded);
    codexbar_history_store_free(store);
    g_free(path);
    remove_directory(directory);
}

static void test_history_keeps_accounts_separate(void) {
    char *directory = temporary_directory();
    CodexBarHistoryStore *store = codexbar_history_store_new(directory);
    CodexBarSnapshot *first = snapshot("one@example.com", 10, 1800007200000LL, 20);
    CodexBarSnapshot *second = snapshot("two@example.com", 30, 1800010800000LL, 40);
    g_assert_true(codexbar_history_store_record(store, first, 1800000000000LL, FALSE, NULL));
    g_assert_true(codexbar_history_store_record(store, second, 1800003600000LL, FALSE, NULL));
    codexbar_snapshot_free(first);
    codexbar_snapshot_free(second);
    json_object *document = codexbar_history_store_load_provider(store, "codex", NULL);
    json_object *accounts = json_object_object_get(document, "accounts");
    g_assert_cmpuint(json_object_object_length(accounts), ==, 2);
    g_assert_cmpuint(json_object_array_length(json_object_object_get(document, "unscoped")), ==, 0);
    json_object_put(document);
    codexbar_history_store_free(store);
    remove_directory(directory);
}

static void test_history_rejects_unsupported_schema(void) {
    char *directory = temporary_directory();
    char *path = g_build_filename(directory, "codex.json", NULL);
    g_assert_true(g_file_set_contents(
        path,
        "{\"version\":2,\"unscoped\":[],\"accounts\":{}}",
        -1,
        NULL));
    CodexBarHistoryStore *store = codexbar_history_store_new(directory);
    GError *error = NULL;
    g_assert_null(codexbar_history_store_load_provider(store, "codex", &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    codexbar_history_store_free(store);
    g_free(path);
    remove_directory(directory);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/history/canonical-account-reset", test_history_is_canonical_account_scoped_and_reset_aware);
    g_test_add_func("/history/account-isolation", test_history_keeps_accounts_separate);
    g_test_add_func("/history/provider-account", test_history_prefers_provider_account_identity);
    g_test_add_func("/history/merge-equivalent-series", test_history_merges_equivalent_persisted_series);
    g_test_add_func("/history/schema", test_history_rejects_unsupported_schema);
    return g_test_run();
}
