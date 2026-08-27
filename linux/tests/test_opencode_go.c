#include "opencode_go.h"
#include "provider_registry.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-c/json.h>
#include <sqlite3.h>
#include <string.h>

typedef struct {
    char *home;
    char *directory;
    char *database_path;
    char *auth_path;
} Fixture;

static Fixture fixture_new(void) {
    GError *error = NULL;
    Fixture fixture = {0};
    fixture.home = g_dir_make_tmp("codexbar-opencode-go-XXXXXX", &error);
    g_assert_no_error(error);
    fixture.directory = g_build_filename(fixture.home, ".local", "share", "opencode", NULL);
    g_assert_cmpint(g_mkdir_with_parents(fixture.directory, 0700), ==, 0);
    fixture.database_path = g_build_filename(fixture.directory, "opencode.db", NULL);
    fixture.auth_path = g_build_filename(fixture.directory, "auth.json", NULL);
    return fixture;
}

static void fixture_free(Fixture *fixture) {
    char *wal_path = g_strconcat(fixture->database_path, "-wal", NULL);
    char *shm_path = g_strconcat(fixture->database_path, "-shm", NULL);
    g_remove(wal_path);
    g_remove(shm_path);
    g_free(shm_path);
    g_free(wal_path);
    g_remove(fixture->database_path);
    g_remove(fixture->auth_path);
    g_rmdir(fixture->directory);
    char *share = g_path_get_dirname(fixture->directory);
    char *local = g_path_get_dirname(share);
    g_rmdir(share);
    g_rmdir(local);
    g_rmdir(fixture->home);
    g_free(local);
    g_free(share);
    g_free(fixture->auth_path);
    g_free(fixture->database_path);
    g_free(fixture->directory);
    g_free(fixture->home);
}

static sqlite3 *open_database(const Fixture *fixture) {
    sqlite3 *database = NULL;
    g_assert_cmpint(sqlite3_open(fixture->database_path, &database), ==, SQLITE_OK);
    return database;
}

static void execute(sqlite3 *database, const char *sql) {
    char *message = NULL;
    int status = sqlite3_exec(database, sql, NULL, NULL, &message);
    if (status != SQLITE_OK) g_test_message("SQLite fixture error: %s", message ? message : "unknown");
    sqlite3_free(message);
    g_assert_cmpint(status, ==, SQLITE_OK);
}

static gint64 timestamp_ms(const char *iso8601) {
    GDateTime *date_time = g_date_time_new_from_iso8601(iso8601, NULL);
    g_assert_nonnull(date_time);
    gint64 result = g_date_time_to_unix(date_time) * 1000 + g_date_time_get_microsecond(date_time) / 1000;
    g_date_time_unref(date_time);
    return result;
}

static CodexBarQuotaWindow *window(CodexBarProvider *provider, guint index, const char *id) {
    CodexBarQuotaWindow *result = codexbar_provider_quota_window(provider, index);
    g_assert_nonnull(result);
    g_assert_cmpstr(result->id, ==, id);
    return result;
}

static guint web_call;

static CodexBarHttpResponse *web_response(const char *body) {
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = 200;
    response->body = g_strdup(body);
    response->body_length = strlen(body);
    response->headers = g_ptr_array_new();
    return response;
}

static CodexBarHttpResponse *web_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    switch (web_call++) {
    case 0:
        g_assert_cmpstr(request->method, ==, "GET");
        g_assert_nonnull(strstr(request->url, "_server?id="));
        return web_response("{}");
    case 1:
        g_assert_cmpstr(request->method, ==, "POST");
        g_assert_cmpuint(request->body_length, ==, 2);
        g_assert_cmpmem(request->body, request->body_length, "[]", 2);
        return web_response("{\"data\":[{\"id\":\"wrk_fixture1\"}]}");
    case 2:
        g_assert_cmpstr(request->method, ==, "GET");
        g_assert_nonnull(strstr(request->url, "/workspace/wrk_fixture1/go"));
        return web_response("rollingUsage:{usagePercent:25,resetInSec:3600}");
    case 3:
        g_assert_nonnull(strstr(request->url, "/workspace/wrk_fixture1"));
        g_assert_null(strstr(request->url, "/go"));
        return web_response("<p>Current balance: $42.50</p>");
    default:
        g_assert_not_reached();
    }
}

static CodexBarHttpResponse *balance_only_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    switch (web_call++) {
    case 0:
        g_assert_nonnull(strstr(request->url, "/workspace/wrk_balance/go"));
        return web_response("<html><body>subscription active</body></html>");
    case 1:
        g_assert_nonnull(strstr(request->url, "/workspace/wrk_balance"));
        return web_response("{\"currentBalanceUSD\":\"7.25\"}");
    default:
        g_assert_not_reached();
    }
}

static void create_message_table(sqlite3 *database) {
    execute(database, "CREATE TABLE message (id TEXT PRIMARY KEY, time_created INTEGER, data TEXT NOT NULL)");
}

static void insert_message(sqlite3 *database,
                           const char *id,
                           gint64 created_ms,
                           const char *provider_id,
                           const char *role,
                           const char *cost_json) {
    char *data = g_strdup_printf(
        "{\"time\":{\"created\":%" G_GINT64_FORMAT "},\"cost\":%s,\"providerID\":\"%s\",\"role\":\"%s\"}",
        created_ms,
        cost_json,
        provider_id,
        role);
    sqlite3_stmt *statement = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(database,
                                      "INSERT INTO message (id, time_created, data) VALUES (?, ?, ?)",
                                      -1,
                                      &statement,
                                      NULL),
                    ==,
                    SQLITE_OK);
    sqlite3_bind_text(statement, 1, id, -1, SQLITE_TRANSIENT);
    sqlite3_bind_int64(statement, 2, created_ms);
    sqlite3_bind_text(statement, 3, data, -1, SQLITE_TRANSIENT);
    g_assert_cmpint(sqlite3_step(statement), ==, SQLITE_DONE);
    sqlite3_finalize(statement);
    g_free(data);
}

static void test_local_database_contract(void) {
    Fixture fixture = fixture_new();
    sqlite3 *database = open_database(&fixture);
    create_message_table(database);
    gint64 now_ms = 1800000000000LL;
    insert_message(database, "message-1", now_ms - 60000, "opencode-go", "assistant", "6");
    sqlite3_close(database);

    GError *error = NULL;
    CodexBarProvider *provider = codexbar_opencode_go_fetch_from_home(fixture.home, now_ms, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->provider, ==, "opencodego");
    g_assert_cmpstr(provider->source, ==, "local");
    g_assert_true(provider->has_updated_at);
    g_assert_cmpint(provider->updated_at_ms, ==, now_ms);
    g_assert_cmpuint(provider->quota_windows->len, ==, 3);

    CodexBarQuotaWindow *rolling = window(provider, 0, "primary");
    g_assert_cmpstr(rolling->title, ==, "5-hour");
    g_assert_cmpint(rolling->window_minutes, ==, 300);
    g_assert_cmpfloat(rolling->used_percent, ==, 50);
    g_assert_cmpint(rolling->resets_at_ms, ==, now_ms + (5 * 60 * 60 - 60) * 1000);
    CodexBarQuotaWindow *weekly = window(provider, 1, "secondary");
    g_assert_cmpstr(weekly->title, ==, "Weekly");
    g_assert_cmpint(weekly->window_minutes, ==, 10080);
    g_assert_cmpfloat(weekly->used_percent, ==, 20);
    CodexBarQuotaWindow *monthly = window(provider, 2, "tertiary");
    g_assert_cmpstr(monthly->title, ==, "Monthly");
    g_assert_cmpint(monthly->window_minutes, ==, 43200);
    g_assert_cmpfloat(monthly->used_percent, ==, 10);

    codexbar_provider_free(provider);
    fixture_free(&fixture);
}

static void test_part_costs_and_message_precedence(void) {
    Fixture fixture = fixture_new();
    sqlite3 *database = open_database(&fixture);
    create_message_table(database);
    execute(database,
            "CREATE TABLE part (message_id TEXT NOT NULL, time_created INTEGER, data TEXT NOT NULL)");
    gint64 now_ms = 1800000000000LL;
    insert_message(database, "parts-only", now_ms - 60000, "opencode-go", "assistant", "null");
    insert_message(database, "message-wins", now_ms - 60000, "opencode-go", "assistant", "3");
    char *sql = g_strdup_printf(
        "INSERT INTO part VALUES "
        "('parts-only', %" G_GINT64_FORMAT ", '{\"type\":\"step-finish\",\"cost\":2}'),"
        "('parts-only', %" G_GINT64_FORMAT ", '{\"type\":\"step-finish\",\"cost\":1}'),"
        "('message-wins', %" G_GINT64_FORMAT ", '{\"type\":\"step-finish\",\"cost\":9}')",
        now_ms - 60000,
        now_ms - 60000,
        now_ms - 60000);
    execute(database, sql);
    g_free(sql);
    sqlite3_close(database);

    GError *error = NULL;
    CodexBarProvider *provider = codexbar_opencode_go_fetch_from_home(fixture.home, now_ms, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(window(provider, 0, "primary")->used_percent, ==, 50);
    codexbar_provider_free(provider);
    fixture_free(&fixture);
}

static void test_idle_wal_immutable_fallback(void) {
    Fixture fixture = fixture_new();
    sqlite3 *database = open_database(&fixture);
    create_message_table(database);
    gint64 now_ms = 1800000000000LL;
    insert_message(database, "idle-wal", now_ms - 60000, "opencode-go", "assistant", "3");
    execute(database, "PRAGMA journal_mode = WAL");
    execute(database, "PRAGMA wal_checkpoint(TRUNCATE)");
    g_assert_cmpint(sqlite3_close(database), ==, SQLITE_OK);
    char *wal_path = g_strconcat(fixture.database_path, "-wal", NULL);
    char *shm_path = g_strconcat(fixture.database_path, "-shm", NULL);
    g_remove(wal_path);
    g_remove(shm_path);
    g_assert_false(g_file_test(wal_path, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(shm_path, G_FILE_TEST_EXISTS));
    g_assert_cmpint(g_chmod(fixture.directory, 0500), ==, 0);

    GError *error = NULL;
    CodexBarProvider *provider = codexbar_opencode_go_fetch_from_home(fixture.home, now_ms, &error);
    g_assert_cmpint(g_chmod(fixture.directory, 0700), ==, 0);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpfloat(window(provider, 0, "primary")->used_percent, ==, 25);
    g_assert_false(g_file_test(wal_path, G_FILE_TEST_EXISTS));
    g_assert_false(g_file_test(shm_path, G_FILE_TEST_EXISTS));
    codexbar_provider_free(provider);
    g_free(shm_path);
    g_free(wal_path);
    fixture_free(&fixture);
}

static void test_active_wal_uses_ordinary_readonly(void) {
    Fixture fixture = fixture_new();
    sqlite3 *database = open_database(&fixture);
    create_message_table(database);
    execute(database, "PRAGMA journal_mode = WAL");
    execute(database, "PRAGMA wal_autocheckpoint = 0");
    gint64 now_ms = 1800000000000LL;
    insert_message(database, "active-wal", now_ms - 60000, "opencode-go", "assistant", "3");
    char *wal_path = g_strconcat(fixture.database_path, "-wal", NULL);
    char *shm_path = g_strconcat(fixture.database_path, "-shm", NULL);
    g_assert_true(g_file_test(wal_path, G_FILE_TEST_EXISTS));
    g_assert_true(g_file_test(shm_path, G_FILE_TEST_EXISTS));

    GError *error = NULL;
    CodexBarProvider *provider = codexbar_opencode_go_fetch_from_home(fixture.home, now_ms, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpfloat(window(provider, 0, "primary")->used_percent, ==, 25);
    codexbar_provider_free(provider);
    g_assert_cmpint(sqlite3_close(database), ==, SQLITE_OK);
    g_free(shm_path);
    g_free(wal_path);
    fixture_free(&fixture);
}

static void test_web_usage_parser(void) {
    const char *page =
        "rollingUsage:{usagePercent:25.5,resetInSec:3600},"
        "weeklyUsage:{usagePercent:40,resetInSec:7200},"
        "monthlyUsage:{usagePercent:75,resetInSec:10800}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_opencode_go_parse_web_usage(
        page, strlen(page), 1800000000000LL, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->source, ==, "web");
    g_assert_cmpuint(provider->quota_windows->len, ==, 3);
    g_assert_cmpfloat(window(provider, 0, "primary")->used_percent, ==, 25.5);
    g_assert_cmpint(window(provider, 1, "secondary")->resets_at_ms, ==, 1800007200000LL);
    g_assert_cmpfloat(window(provider, 2, "tertiary")->used_percent, ==, 75);
    codexbar_provider_free(provider);

    const char *json =
        "{\"payload\":{\"limits\":{"
        "\"five_hour\":{\"used\":3,\"limit\":12,\"resetAt\":\"2027-01-15T09:00:00Z\"},"
        "\"week\":{\"utilization\":0.4,\"reset_in_sec\":7200},"
        "\"month\":{\"percentUsed\":75,\"resetsAt\":1800000010800}}}}";
    provider = codexbar_opencode_go_parse_web_usage(
        json, strlen(json), 1800000000000LL, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 3);
    g_assert_cmpfloat(window(provider, 0, "primary")->used_percent, ==, 25);
    g_assert_cmpfloat(window(provider, 1, "secondary")->used_percent, ==, 40);
    g_assert_cmpfloat(window(provider, 2, "tertiary")->used_percent, ==, 75);
    codexbar_provider_free(provider);

    const char *api_percent_units =
        "{\"usage\":{"
        "\"rolling\":{\"usagePercent\":1,\"resetInSec\":3600},"
        "\"weekly\":{\"usagePercent\":0.5,\"resetInSec\":7200},"
        "\"monthly\":{\"usagePercent\":75,\"resetInSec\":10800}}}";
    provider = codexbar_opencode_go_parse_web_usage(
        api_percent_units, strlen(api_percent_units), 1800000000000LL, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(window(provider, 0, "primary")->used_percent, ==, 1);
    g_assert_cmpfloat(window(provider, 1, "secondary")->used_percent, ==, 0.5);
    g_assert_cmpfloat(window(provider, 2, "tertiary")->used_percent, ==, 75);
    codexbar_provider_free(provider);

    const char *oversized_reset =
        "{\"rollingUsage\":{\"usagePercent\":25,\"resetInSec\":1e100}}";
    provider = codexbar_opencode_go_parse_web_usage(
        oversized_reset, strlen(oversized_reset), 1800000000000LL, &error);
    g_assert_no_error(error);
    CodexBarQuotaWindow *primary = window(provider, 0, "primary");
    g_assert_false(primary->has_resets_at);
    codexbar_provider_free(provider);
}

static void test_web_discovery_fallback_and_zen_balance(void) {
    CodexBarProviderConfig config = {0};
    config.raw = json_object_new_object();
    json_object_object_add(config.raw, "cookieHeader", json_object_new_string("session=fixture"));
    web_call = 0;
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_opencode_go_fetch_for_source_with_transport_and_cancellable(
        &config, "web", web_transport, NULL, 1800000000000LL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(web_call, ==, 4);
    g_assert_cmpstr(provider->source, ==, "web");
    g_assert_cmpfloat(window(provider, 0, "primary")->used_percent, ==, 25);
    g_assert_nonnull(provider->provider_cost);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 42.5);
    g_assert_cmpstr(provider->provider_cost->period, ==, "Zen balance");
    codexbar_provider_free(provider);
    json_object_put(config.raw);

    double balance = 0;
    const char *billing = "{\"customer\":{\"customerID\":\"cus_fixture\",\"balance\":4250000000}}";
    g_assert_true(codexbar_opencode_go_parse_zen_balance(
        billing, strlen(billing), TRUE, &balance));
    g_assert_cmpfloat(balance, ==, 42.5);
    const char *unrelated = "{\"balance\":999,\"plan\":\"go\"}";
    g_assert_false(codexbar_opencode_go_parse_zen_balance(
        unrelated, strlen(unrelated), FALSE, &balance));

    config.raw = json_object_new_object();
    json_object_object_add(config.raw, "cookieHeader", json_object_new_string("session=fixture"));
    json_object_object_add(config.raw, "workspaceID", json_object_new_string("wrk_balance"));
    web_call = 0;
    provider = codexbar_opencode_go_fetch_for_source_with_transport_and_cancellable(
        &config, "auto", balance_only_transport, NULL, 1800000000000LL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(web_call, ==, 2);
    g_assert_cmpuint(provider->quota_windows->len, ==, 0);
    g_assert_nonnull(provider->provider_cost);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 7.25);
    codexbar_provider_free(provider);
    json_object_put(config.raw);
}

static void write_auth(const Fixture *fixture, const char *contents) {
    GError *error = NULL;
    g_assert_true(g_file_set_contents(fixture->auth_path, contents, -1, &error));
    g_assert_no_error(error);
}

static void assert_fetch_error(const Fixture *fixture, const char *message) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_opencode_go_fetch_from_home(fixture->home, 1800000000000LL, &error);
    g_assert_null(provider);
    g_assert_nonnull(error);
    g_assert_cmpstr(error->message, ==, message);
    g_clear_error(&error);
}

static void test_detection_errors(void) {
    Fixture fixture = fixture_new();
    const char *not_detected = "OpenCode Go not detected. Log in with OpenCode Go or use it locally first.";
    assert_fetch_error(&fixture, not_detected);

    write_auth(&fixture, "{\"opencode-go\":{\"key\":\"  token  \"}}");
    assert_fetch_error(&fixture, "OpenCode Go local usage history is unavailable: database not found");

    sqlite3 *database = open_database(&fixture);
    create_message_table(database);
    sqlite3_close(database);
    assert_fetch_error(&fixture, "OpenCode Go local usage history is unavailable: no local usage rows");

    g_remove(fixture.auth_path);
    assert_fetch_error(&fixture, not_detected);

    write_auth(&fixture, "{\"opencode-go\":{\"key\":\"token\"}} // invalid");
    assert_fetch_error(&fixture, not_detected);
    fixture_free(&fixture);
}

static void test_schema_error(void) {
    Fixture fixture = fixture_new();
    sqlite3 *database = open_database(&fixture);
    sqlite3_close(database);
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_opencode_go_fetch_from_home(fixture.home, 1800000000000LL, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_true(g_str_has_prefix(error->message, "SQLite error reading OpenCode Go usage: "));
    g_clear_error(&error);
    fixture_free(&fixture);
}

static void test_filters_unusable_rows(void) {
    Fixture fixture = fixture_new();
    sqlite3 *database = open_database(&fixture);
    create_message_table(database);
    gint64 now_ms = 1800000000000LL;
    insert_message(database, "valid", now_ms - 60000, "opencode-go", "assistant", "1.2");
    insert_message(database, "wrong-provider", now_ms - 60000, "other", "assistant", "50");
    insert_message(database, "wrong-role", now_ms - 60000, "opencode-go", "user", "50");
    insert_message(database, "negative", now_ms - 60000, "opencode-go", "assistant", "-1");
    insert_message(database, "bad-time", 0, "opencode-go", "assistant", "50");
    execute(database, "INSERT INTO message VALUES ('invalid-json', 1, 'not json')");
    sqlite3_close(database);

    GError *error = NULL;
    CodexBarProvider *provider = codexbar_opencode_go_fetch_from_home(fixture.home, now_ms, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(window(provider, 0, "primary")->used_percent, ==, 10);
    codexbar_provider_free(provider);
    fixture_free(&fixture);
}

static void test_month_anchor_clamps_short_month(void) {
    Fixture fixture = fixture_new();
    sqlite3 *database = open_database(&fixture);
    create_message_table(database);
    gint64 anchor_ms = timestamp_ms("2027-01-31T10:00:00Z");
    gint64 now_ms = timestamp_ms("2027-02-15T12:00:00Z");
    insert_message(database, "anchor", anchor_ms, "opencode-go", "assistant", "6");
    sqlite3_close(database);

    GError *error = NULL;
    CodexBarProvider *provider = codexbar_opencode_go_fetch_from_home(fixture.home, now_ms, &error);
    g_assert_no_error(error);
    CodexBarQuotaWindow *monthly = window(provider, 2, "tertiary");
    g_assert_cmpfloat(monthly->used_percent, ==, 10);
    g_assert_cmpint(monthly->resets_at_ms, ==, timestamp_ms("2027-02-28T10:00:00Z"));
    codexbar_provider_free(provider);
    fixture_free(&fixture);
}

static void test_registry_entry(void) {
    const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find("opencodego");
    g_assert_nonnull(descriptor);
    g_assert_cmpint(descriptor->native_provider, ==, CODEXBAR_NATIVE_OPENCODE_GO);
    g_assert_true(codexbar_provider_supports_source(descriptor, "auto"));
    g_assert_true(codexbar_provider_supports_source(descriptor, "web"));
    const char *plan[1] = {0};
    g_assert_cmpuint(codexbar_provider_auto_source_plan(descriptor, plan, G_N_ELEMENTS(plan)), ==, 1);
    g_assert_cmpstr(plan[0], ==, "local");
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/opencode-go/local-database", test_local_database_contract);
    g_test_add_func("/opencode-go/part-costs", test_part_costs_and_message_precedence);
    g_test_add_func("/opencode-go/idle-wal", test_idle_wal_immutable_fallback);
    g_test_add_func("/opencode-go/active-wal", test_active_wal_uses_ordinary_readonly);
    g_test_add_func("/opencode-go/web-usage", test_web_usage_parser);
    g_test_add_func("/opencode-go/web-fallback-balance", test_web_discovery_fallback_and_zen_balance);
    g_test_add_func("/opencode-go/detection-errors", test_detection_errors);
    g_test_add_func("/opencode-go/schema-error", test_schema_error);
    g_test_add_func("/opencode-go/filtering", test_filters_unusable_rows);
    g_test_add_func("/opencode-go/month-anchor", test_month_anchor_clamps_short_month);
    g_test_add_func("/opencode-go/registry", test_registry_entry);
    return g_test_run();
}
