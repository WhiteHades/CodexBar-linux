#include "windsurf.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <glib/gstdio.h>
#include <sqlite3.h>
#include <string.h>
#include <unistd.h>

static GByteArray *fixture_body;
static GCancellable *expected_cancellable;
static gboolean cancel_request;
static long response_status = 200;
static gboolean redirect_response;

typedef struct {
    char *directory;
    char *database;
} LocalDatabase;

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static LocalDatabase make_local_database(const char *json, gboolean utf16) {
    char *current = g_get_current_dir();
    char *base = g_build_filename(current, ".tmp", NULL);
    g_free(current);
    g_assert_cmpint(g_mkdir_with_parents(base, 0700), ==, 0);
    char *pattern = g_strdup_printf("%s/windsurf-%ld-XXXXXX", base, (long)getpid());
    g_free(base);
    g_assert_nonnull(g_mkdtemp(pattern));
    LocalDatabase local = {.directory = pattern, .database = g_build_filename(pattern, "state.vscdb", NULL)};
    sqlite3 *database = NULL;
    g_assert_cmpint(sqlite3_open(local.database, &database), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(database,
                                "CREATE TABLE ItemTable(key TEXT PRIMARY KEY, value BLOB);",
                                NULL,
                                NULL,
                                NULL),
                    ==,
                    SQLITE_OK);
    gsize value_length = strlen(json);
    char *converted = NULL;
    const void *value = json;
    if (utf16) {
        converted = g_convert(json, -1, "UTF-16LE", "UTF-8", NULL, &value_length, NULL);
        g_assert_nonnull(converted);
        value = converted;
    }
    sqlite3_stmt *statement = NULL;
    g_assert_cmpint(sqlite3_prepare_v2(database,
                                      "INSERT INTO ItemTable(key, value) VALUES(?, ?);",
                                      -1,
                                      &statement,
                                      NULL),
                    ==,
                    SQLITE_OK);
    g_assert_cmpint(sqlite3_bind_text(statement, 1, "windsurf.settings.cachedPlanInfo", -1, SQLITE_STATIC),
                    ==,
                    SQLITE_OK);
    g_assert_cmpint(sqlite3_bind_blob(statement, 2, value, (int)value_length, SQLITE_TRANSIENT), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_step(statement), ==, SQLITE_DONE);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    g_free(converted);
    return local;
}

static void clear_local_database(LocalDatabase *local) {
    g_remove(local->database);
    g_rmdir(local->directory);
    g_free(local->database);
    g_free(local->directory);
    *local = (LocalDatabase){0};
}

static void append_varint(GByteArray *data, guint64 value) {
    while (value >= 0x80) {
        guint8 byte = (guint8)((value & 0x7f) | 0x80);
        g_byte_array_append(data, &byte, 1);
        value >>= 7;
    }
    guint8 byte = (guint8)value;
    g_byte_array_append(data, &byte, 1);
}

static void append_key(GByteArray *data, guint field, guint wire) {
    append_varint(data, (guint64)field << 3 | wire);
}

static void append_varint_field(GByteArray *data, guint field, guint64 value) {
    append_key(data, field, 0);
    append_varint(data, value);
}

static void append_bytes_field(GByteArray *data, guint field, const guint8 *bytes, size_t length) {
    append_key(data, field, 2);
    append_varint(data, length);
    g_byte_array_append(data, bytes, length);
}

static void append_string_field(GByteArray *data, guint field, const char *value) {
    append_bytes_field(data, field, (const guint8 *)value, strlen(value));
}

static GByteArray *timestamp(gint64 seconds) {
    GByteArray *data = g_byte_array_new();
    append_varint_field(data, 1, (guint64)seconds);
    return data;
}

static GByteArray *plan_response(void) {
    GByteArray *plan_info = g_byte_array_new();
    append_varint_field(plan_info, 1, 2);
    append_string_field(plan_info, 2, "Pro");

    GByteArray *status = g_byte_array_new();
    append_bytes_field(status, 1, plan_info->data, plan_info->len);
    GByteArray *start = timestamp(1782864000);
    GByteArray *end = timestamp(1788220800);
    append_bytes_field(status, 2, start->data, start->len);
    append_bytes_field(status, 3, end->data, end->len);
    append_varint_field(status, 12, 0);
    append_varint_field(status, 14, 68);
    append_varint_field(status, 15, 84);
    append_varint_field(status, 17, 1785636000);
    append_varint_field(status, 18, 1786147200);

    GByteArray *outer = g_byte_array_new();
    append_bytes_field(outer, 1, status->data, status->len);
    g_byte_array_unref(plan_info);
    g_byte_array_unref(start);
    g_byte_array_unref(end);
    g_byte_array_unref(status);
    return outer;
}

static const char *header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static CodexBarHttpResponse *transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    g_assert_cmpstr(request->url, ==,
                    "https://windsurf.com/_backend/exa.seat_management_pb.SeatManagementService/GetPlanStatus");
    g_assert_cmpstr(request->method, ==, "POST");
    g_assert_cmpstr(header(request, "Content-Type"), ==, "application/proto");
    g_assert_cmpstr(header(request, "Connect-Protocol-Version"), ==, "1");
    g_assert_cmpstr(header(request, "Origin"), ==, "https://windsurf.com");
    g_assert_cmpstr(header(request, "Referer"), ==, "https://windsurf.com/profile");
    g_assert_cmpstr(header(request, "x-auth-token"), ==, "devin-session$abc");
    g_assert_cmpstr(header(request, "x-devin-session-token"), ==, "devin-session$abc");
    g_assert_cmpstr(header(request, "x-devin-auth1-token"), ==, "auth-one");
    g_assert_cmpstr(header(request, "x-devin-account-id"), ==, "account-123");
    g_assert_cmpstr(header(request, "x-devin-primary-org-id"), ==, "org-456");
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == expected_cancellable);
    GByteArray *expected = g_byte_array_new();
    append_string_field(expected, 1, "devin-session$abc");
    append_varint_field(expected, 2, 1);
    g_assert_cmpuint(request->body_length, ==, expected->len);
    g_assert_cmpmem(request->body, request->body_length, expected->data, expected->len);
    g_byte_array_unref(expected);
    if (cancel_request) g_cancellable_cancel(request->cancellable);

    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = response_status;
    response->body = g_memdup2(fixture_body->data, fixture_body->len);
    response->body_length = fixture_body->len;
    response->headers = g_ptr_array_new();
    response->effective_url = g_strdup(redirect_response ? "https://attacker.example/stolen" : request->url);
    return response;
}

static CodexBarProviderConfig json_config(void) {
    CodexBarProviderConfig config = {0};
    config.raw = json_object_new_object();
    json_object_object_add(config.raw, "session", json_object_new_string(
        "{\"devinSessionToken\":\"devin-session$abc\",\"devinAuth1Token\":\"auth-one\","
        "\"devinAccountId\":\"account-123\",\"devinPrimaryOrgId\":\"org-456\"}"));
    return config;
}

static CodexBarProviderConfig key_value_config(void) {
    CodexBarProviderConfig config = {0};
    config.raw = json_object_new_object();
    json_object_object_add(config.raw, "cookieHeader", json_object_new_string(
        "devin_session_token=devin-session$abc; devin_auth1_token=auth-one; "
        "devin_account_id=account-123; devin_primary_org_id=org-456"));
    return config;
}

static void test_parser(void) {
    fixture_body = plan_response();
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_windsurf_parse(
        fixture_body->data, fixture_body->len, 1785542400000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->provider, ==, "windsurf");
    g_assert_cmpstr(provider->source, ==, "web");
    g_assert_cmpstr(provider->plan, ==, "Pro");
    g_assert_cmpstr(provider->identity->organization, ==, "Expires 2026-09-01");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    CodexBarQuotaWindow *daily = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat(daily->used_percent, ==, 32);
    g_assert_cmpstr(daily->reset_description, ==, "Resets in 1d 2h");
    CodexBarQuotaWindow *weekly = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpfloat(weekly->used_percent, ==, 16);
    g_assert_cmpstr(weekly->reset_description, ==, "Resets in 7d 0h");
    codexbar_provider_free(provider);

    const guint8 malformed[] = {0x0a, 0x02, 0x08};
    provider = codexbar_windsurf_parse(malformed, sizeof(malformed), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_byte_array_unref(fixture_body);
    fixture_body = NULL;
}

static void test_cached_plan(void) {
    const char *json =
        "{\"planName\":\"Pro\",\"endTimestamp\":1774029950000,"
        "\"usage\":{\"messages\":50000,\"usedMessages\":1200,\"flowActions\":150000,"
        "\"remainingFlowActions\":150000},\"quotaUsage\":{\"dailyRemainingPercent\":9.5,"
        "\"dailyResetAtUnix\":1774080000}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_windsurf_parse_cached_plan(json, strlen(json), 1773993600000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->source, ==, "local");
    g_assert_cmpstr(provider->plan, ==, "Pro");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 90.5);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 0)->resets_at_ms, ==, 1774080000000);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 1)->used_percent, ==, 0);
    g_assert_cmpstr(codexbar_provider_quota_window(provider, 1)->reset_description,
                    ==,
                    "0 / 150000 flow actions");
    g_assert_true(provider->has_subscription_expires_at);
    codexbar_provider_free(provider);

    provider = codexbar_windsurf_parse_cached_plan("[]", 2, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_local_database_and_sources(void) {
    const char *json =
        "{\"planName\":\"Local Pro\",\"quotaUsage\":{\"dailyRemainingPercent\":68,"
        "\"weeklyRemainingPercent\":84}}";
    for (guint utf16 = 0; utf16 < 2; utf16++) {
        LocalDatabase local = make_local_database(json, utf16 != 0);
        GError *error = NULL;
        CodexBarProvider *provider = codexbar_windsurf_fetch_local(local.database, 1, &error);
        g_assert_no_error(error);
        g_assert_nonnull(provider);
        g_assert_cmpstr(provider->plan, ==, "Local Pro");
        codexbar_provider_free(provider);

        CodexBarProviderConfig config = {0};
        config.raw = json_object_new_object();
        json_object_object_add(config.raw, "dbPath", json_object_new_string(local.database));
        provider = codexbar_windsurf_fetch_for_source_with_transport_and_cancellable(
            &config, utf16 ? "auto" : "cli", unexpected_transport, NULL, 1, &error);
        g_assert_no_error(error);
        g_assert_nonnull(provider);
        g_assert_cmpstr(provider->source, ==, "local");
        codexbar_provider_free(provider);
        json_object_put(config.raw);
        clear_local_database(&local);
    }
}

static void test_transport_sessions(void) {
    fixture_body = plan_response();
    response_status = 200;
    redirect_response = FALSE;
    cancel_request = FALSE;
    GError *error = NULL;
    CodexBarProviderConfig config = json_config();
    CodexBarProvider *provider = codexbar_windsurf_fetch_with_transport_and_cancellable(
        &config, transport, NULL, 1785542400000, &error);
    g_assert_no_error(error);
    codexbar_provider_free(provider);
    json_object_put(config.raw);

    config = key_value_config();
    provider = codexbar_windsurf_fetch_with_transport_and_cancellable(
        &config, transport, NULL, 1785542400000, &error);
    g_assert_no_error(error);
    codexbar_provider_free(provider);
    json_object_put(config.raw);
    g_byte_array_unref(fixture_body);
    fixture_body = NULL;
}

static void test_security_and_errors(void) {
    fixture_body = plan_response();
    GError *error = NULL;
    CodexBarProviderConfig config = {0};
    config.raw = json_object_new_object();
    CodexBarProvider *provider = codexbar_windsurf_fetch_with_transport_and_cancellable(
        &config, transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    json_object_put(config.raw);

    config = json_config();
    response_status = 401;
    provider = codexbar_windsurf_fetch_with_transport_and_cancellable(&config, transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    response_status = 200;

    redirect_response = TRUE;
    provider = codexbar_windsurf_fetch_with_transport_and_cancellable(&config, transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    redirect_response = FALSE;

    expected_cancellable = g_cancellable_new();
    cancel_request = TRUE;
    provider = codexbar_windsurf_fetch_with_transport_and_cancellable(
        &config, transport, expected_cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(expected_cancellable);
    expected_cancellable = NULL;
    cancel_request = FALSE;
    json_object_put(config.raw);
    g_byte_array_unref(fixture_body);
    fixture_body = NULL;
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/windsurf/parser", test_parser);
    g_test_add_func("/windsurf/cached-plan", test_cached_plan);
    g_test_add_func("/windsurf/local-database-sources", test_local_database_and_sources);
    g_test_add_func("/windsurf/transport-sessions", test_transport_sessions);
    g_test_add_func("/windsurf/security-errors", test_security_and_errors);
    return g_test_run();
}
