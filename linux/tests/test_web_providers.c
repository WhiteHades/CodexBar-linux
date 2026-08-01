#include "web_providers.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-c/json.h>
#include <sqlite3.h>
#include <string.h>

typedef struct {
    const char *scenario;
    const char *bodies[4];
    long statuses[4];
    guint response_count;
    guint count;
    GCancellable *cancellable;
    gboolean cancel_after_first;
} TransportFixture;

static TransportFixture fixture;

static CodexBarHttpResponse *make_response(long status, const char *body) {
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = status;
    response->body = g_strdup(body ? body : "");
    response->body_length = strlen(response->body);
    response->headers = g_ptr_array_new();
    return response;
}

static const char *request_header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static CodexBarHttpResponse *stub_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    guint index = fixture.count++;
    g_assert_cmpuint(index, <, fixture.response_count);
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == fixture.cancellable);
    if (g_str_equal(fixture.scenario, "cursor")) {
        g_assert_cmpstr(request_header(request, "Cookie"), ==, "WorkosCursorSessionToken=manual-token");
        g_assert_true(g_str_has_prefix(request->url, "https://cursor.com/api/"));
        if (index == 0) g_assert_cmpstr(request->url, ==, "https://cursor.com/api/usage-summary");
        if (index == 1) g_assert_cmpstr(request->url, ==, "https://cursor.com/api/auth/me");
        if (index == 2) g_assert_cmpstr(request->url, ==, "https://cursor.com/api/usage?user=user-123");
    } else if (g_str_equal(fixture.scenario, "opencode")) {
        g_assert_cmpstr(request->url,
                        ==,
                        "https://opencode.ai/_server?id="
                        "7abeebee372f304e050aaaf92be863f4a86490e382f8c79db68fd94040d691b4&args=%5B%22wrk_abc123%22%5D");
        g_assert_cmpstr(request_header(request, "Cookie"), ==, "__Host-auth=session-token; auth=second");
        g_assert_cmpstr(request_header(request, "Origin"), ==, "https://opencode.ai");
        g_assert_cmpstr(request_header(request, "X-Server-Id"),
                        ==,
                        "7abeebee372f304e050aaaf92be863f4a86490e382f8c79db68fd94040d691b4");
    } else {
        g_assert_cmpstr(request->url, ==, "https://app.devin.ai/api/org_abc/billing/quota/usage");
        g_assert_cmpstr(request_header(request, "Authorization"), ==, "Bearer devin-token");
        g_assert_cmpstr(request_header(request, "x-cog-org-id"), ==, "org_abc");
    }
    CodexBarHttpResponse *response = make_response(fixture.statuses[index], fixture.bodies[index]);
    if (fixture.cancel_after_first) g_cancellable_cancel(fixture.cancellable);
    return response;
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static void reset_fixture(const char *scenario, guint response_count) {
    memset(&fixture, 0, sizeof(fixture));
    fixture.scenario = scenario;
    fixture.response_count = response_count;
}

static CodexBarProviderConfig make_config(const char *cookie_header) {
    CodexBarProviderConfig config = {0};
    config.raw = json_object_new_object();
    if (cookie_header) json_object_object_add(config.raw, "cookieHeader", json_object_new_string(cookie_header));
    return config;
}

static void clear_config(CodexBarProviderConfig *config) {
    if (config->raw) json_object_put(config->raw);
}

static void test_cursor_enterprise_and_legacy(void) {
    const char *summary =
        "{\"billingCycleStart\":\"2026-04-01T00:00:00Z\","
        "\"billingCycleEnd\":\"2026-05-01T00:00:00Z\",\"membershipType\":\"enterprise\","
        "\"individualUsage\":{\"overall\":{\"used\":7384,\"limit\":10000},"
        "\"onDemand\":{\"used\":1250,\"limit\":null}},"
        "\"teamUsage\":{\"onDemand\":{\"used\":5000,\"limit\":20000},"
        "\"pooled\":{\"used\":12725135,\"limit\":28122000}}}";
    const char *user = "{\"email\":\"cursor@example.test\",\"sub\":\"user-123\"}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_cursor_parse_usage(
        summary, strlen(summary), user, strlen(user), NULL, 0, 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->provider, ==, "cursor");
    g_assert_cmpstr(provider->account, ==, "cursor@example.test");
    g_assert_cmpstr(provider->identity->account_id, ==, "user-123");
    g_assert_cmpstr(provider->identity->login_method, ==, "Cursor Enterprise");
    g_assert_cmpfloat_with_epsilon(codexbar_provider_quota_window(provider, 0)->used_percent, 73.84, 0.0001);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 0)->window_minutes, ==, 43200);
    g_assert_nonnull(provider->provider_cost);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 50);
    g_assert_cmpfloat(provider->provider_cost->limit, ==, 200);
    g_assert_true(provider->provider_cost->has_personal_used);
    g_assert_cmpfloat(provider->provider_cost->personal_used, ==, 12.5);
    codexbar_provider_free(provider);

    const char *plan =
        "{\"individualUsage\":{\"plan\":{\"totalPercentUsed\":0.36,"
        "\"autoPercentUsed\":0.25,\"apiPercentUsed\":1.5}}}";
    const char *requests = "{\"gpt-4\":{\"numRequestsTotal\":125,\"maxRequestUsage\":500}}";
    provider = codexbar_cursor_parse_usage(
        plan, strlen(plan), NULL, 0, requests, strlen(requests), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    g_assert_cmpstr(codexbar_provider_quota_window(provider, 0)->title, ==, "Requests");
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 25);
    codexbar_provider_free(provider);
}

static void test_cursor_local_auth_database(void) {
    g_assert_cmpint(g_mkdir_with_parents(".tmp/web-provider-test", 0700), ==, 0);
    char *database_path = g_strdup(".tmp/web-provider-test/cursor-state.vscdb");
    g_unlink(database_path);
    sqlite3 *database = NULL;
    g_assert_cmpint(sqlite3_open(database_path, &database), ==, SQLITE_OK);
    g_assert_cmpint(sqlite3_exec(database,
                                "CREATE TABLE ItemTable (key TEXT, value TEXT);"
                                "INSERT INTO ItemTable VALUES "
                                "('cursorAuth/accessToken',"
                                "'a.eyJzdWIiOiJhdXRoMHx1c2VyLTEyMyIsImV4cCI6MjAwMDAwMDAwMH0.b');",
                                NULL,
                                NULL,
                                NULL),
                    ==,
                    SQLITE_OK);
    sqlite3_close(database);
    GError *error = NULL;
    char *cookie = codexbar_cursor_load_app_cookie(database_path, 1800000000000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(cookie,
                    ==,
                    "WorkosCursorSessionToken=user-123%3A%3Aa.eyJzdWIiOiJhdXRoMHx1c2VyLTEyMyIsImV4cCI6MjAwMDAwMDAwMH0.b");
    g_free(cookie);
    g_unlink(database_path);
    g_free(database_path);
    g_rmdir(".tmp/web-provider-test");
}

static void test_cursor_transport_and_cookie_security(void) {
    CodexBarProviderConfig config = make_config(
        "Cookie: WorkosCursorSessionToken=manual-token; harmless=ok\r\nX-Injected: yes");
    g_assert_false(codexbar_cursor_has_auth(&config));
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_cursor_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    clear_config(&config);

    config = make_config("Cookie: WorkosCursorSessionToken=manual-token");
    reset_fixture("cursor", 3);
    fixture.statuses[0] = fixture.statuses[1] = fixture.statuses[2] = 200;
    fixture.bodies[0] = "{\"individualUsage\":{\"plan\":{\"totalPercentUsed\":30}}}";
    fixture.bodies[1] = "{\"email\":\"cursor@example.test\",\"sub\":\"user-123\"}";
    fixture.bodies[2] = "{\"gpt-4\":{\"numRequests\":10}}";
    provider = codexbar_cursor_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 3);
    codexbar_provider_free(provider);
    clear_config(&config);
}

static void test_opencode_parser_and_transport(void) {
    const char *json =
        "{\"data\":{\"renewAt\":1900000000000,\"usage\":{"
        "\"rollingUsage\":{\"usagePercent\":0.25,\"resetInSec\":3600},"
        "\"weeklyUsage\":{\"used\":75,\"limit\":100,\"resetAt\":2000000000},"
        "\"renewAt\":2000000000000}}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_opencode_parse_usage(json, strlen(json), 1900000000000, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 25);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 1)->used_percent, ==, 75);
    g_assert_true(provider->has_subscription_renews_at);
    g_assert_cmpint(provider->subscription_renews_at_ms, ==, 2000000000000);
    codexbar_provider_free(provider);

    CodexBarProviderConfig config = make_config("ignored=1; __Host-auth=session-token; auth=second");
    config.workspace_id = "https://opencode.ai/workspace/wrk_abc123/billing";
    reset_fixture("opencode", 1);
    fixture.statuses[0] = 200;
    fixture.bodies[0] = json;
    provider = codexbar_opencode_fetch_with_transport(&config, stub_transport, 1900000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    codexbar_provider_free(provider);
    clear_config(&config);

    config = make_config("auth=safe\nAuthorization: evil");
    g_assert_false(codexbar_opencode_has_auth(&config));
    clear_config(&config);
}

static void test_devin_parser_normalization_and_transport(void) {
    char *organization = codexbar_devin_normalize_organization(
        "https://app.devin.ai/org/example-org/settings/usage");
    g_assert_cmpstr(organization, ==, "org/example-org");
    g_free(organization);
    organization = codexbar_devin_normalize_organization("org_GQ6LhcfkW1TSinM6");
    g_assert_cmpstr(organization, ==, "organizations/org_GQ6LhcfkW1TSinM6");
    g_free(organization);
    g_assert_null(codexbar_devin_normalize_organization("https://evil.test/org/example"));
    g_assert_null(codexbar_devin_normalize_organization("org/example/../admin"));

    const char *json =
        "{\"plan_name\":\"pro_plan\",\"quota_usage\":{"
        "\"daily_quota\":{\"used\":3,\"limit\":10,\"reset_at\":\"2026-06-01T08:00:00Z\"},"
        "\"weekly_quota\":{\"remaining_percent\":0.25,\"next_reset_at\":1780560000}},"
        "\"overage_balance_cents\":7087}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_devin_parse_usage(
        json, strlen(json), "org/example-org", 1780000000000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->plan, ==, "Pro Plan");
    g_assert_cmpstr(provider->identity->organization, ==, "example-org");
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 30);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 1)->used_percent, ==, 75);
    g_assert_cmpfloat_with_epsilon(provider->provider_cost->used, 70.87, 0.0001);
    codexbar_provider_free(provider);

    CodexBarProviderConfig config = make_config(NULL);
    json_object_object_add(config.raw, "cookieHeader", json_object_new_string("Authorization: Bearer devin-token"));
    config.workspace_id = "org_abc";
    reset_fixture("devin", 1);
    fixture.statuses[0] = 200;
    fixture.bodies[0] = "{\"daily_percentage\":0.12,\"weekly_percentage\":42}";
    provider = codexbar_devin_fetch_with_transport(&config, stub_transport, 1780000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 12);
    codexbar_provider_free(provider);
    clear_config(&config);
}

static void test_cancellation_and_malformed_inputs(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_opencode_parse_usage("{} trailing", 11, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    static const char embedded_nul[] = "{\"daily_percentage\":1}\0junk";
    provider = codexbar_devin_parse_usage(
        embedded_nul, sizeof(embedded_nul) - 1, "org/example", 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);

    CodexBarProviderConfig config = make_config("WorkosCursorSessionToken=manual-token");
    reset_fixture("cursor", 1);
    fixture.statuses[0] = 200;
    fixture.bodies[0] = "{\"individualUsage\":{\"plan\":{\"totalPercentUsed\":30}}}";
    fixture.cancellable = g_cancellable_new();
    fixture.cancel_after_first = TRUE;
    provider = codexbar_cursor_fetch_with_transport_and_cancellable(
        &config, stub_transport, fixture.cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(fixture.cancellable);
    clear_config(&config);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/web-providers/cursor/parser", test_cursor_enterprise_and_legacy);
    g_test_add_func("/web-providers/cursor/local-auth", test_cursor_local_auth_database);
    g_test_add_func("/web-providers/cursor/transport", test_cursor_transport_and_cookie_security);
    g_test_add_func("/web-providers/opencode", test_opencode_parser_and_transport);
    g_test_add_func("/web-providers/devin", test_devin_parser_normalization_and_transport);
    g_test_add_func("/web-providers/security-cancellation", test_cancellation_and_malformed_inputs);
    return g_test_run();
}
