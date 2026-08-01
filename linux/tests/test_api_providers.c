#include "api_providers.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

typedef struct {
    const char *urls[12];
    long statuses[12];
    const char *bodies[12];
    guint response_count;
    guint count;
    const char *authorization;
    GCancellable *cancellable;
    gboolean cancel_after[12];
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
    g_assert_cmpstr(request->url, ==, fixture.urls[index]);
    g_assert_cmpstr(request->method, ==, "GET");
    g_assert_cmpstr(request_header(request, "Authorization"), ==, fixture.authorization);
    g_assert_cmpstr(request_header(request, "Accept"), ==, "application/json");
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == fixture.cancellable);
    CodexBarHttpResponse *response = make_response(fixture.statuses[index], fixture.bodies[index]);
    if (fixture.cancel_after[index]) g_cancellable_cancel(fixture.cancellable);
    return response;
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static void reset_fixture(const char *authorization, guint response_count) {
    memset(&fixture, 0, sizeof(fixture));
    fixture.authorization = authorization;
    fixture.response_count = response_count;
}

static json_object *extension(const CodexBarProvider *provider, const char *name) {
    json_object *value = NULL;
    g_assert_nonnull(provider->usage_extensions);
    g_assert_true(json_object_object_get_ex(provider->usage_extensions, name, &value));
    return value;
}

static void test_deepgram_parser(void) {
    const char *body = "{\"start\":\"2025-01-16\",\"end\":\"2025-01-23\",\"results\":["
                       "{\"hours\":1.5,\"total_hours\":2,\"agent_hours\":0.5,\"tokens_in\":10,"
                       "\"tokens_out\":20,\"tts_characters\":30,\"requests\":7},"
                       "{\"hours\":2.5,\"total_hours\":3,\"requests\":9}]}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_deepgram_parse_usage_bytes(
        body, strlen(body), "project-a", "Alpha", 1, 123000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->provider, ==, "deepgram");
    g_assert_cmpstr(provider->identity->login_method, ==, "Project: Alpha");
    json_object *usage = extension(provider, "deepgramUsage");
    g_assert_cmpfloat(json_object_get_double(json_object_object_get(usage, "hours")), ==, 4.0);
    g_assert_cmpfloat(json_object_get_double(json_object_object_get(usage, "totalHours")), ==, 5.0);
    g_assert_cmpint(json_object_get_int64(json_object_object_get(usage, "requests")), ==, 16);
    g_assert_cmpint(json_object_get_int64(json_object_object_get(usage, "tokensIn")), ==, 10);
    codexbar_provider_free(provider);

    static const char embedded_nul[] = "{\"results\":[]}\0junk";
    provider = codexbar_deepgram_parse_usage_bytes(
        embedded_nul, sizeof(embedded_nul) - 1, "project-a", NULL, 1, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_deepgram_transport(void) {
    g_setenv("DEEPGRAM_API_URL", "https://deepgram.test/v1/", TRUE);
    reset_fixture("Token dg-key", 3);
    fixture.urls[0] = "https://deepgram.test/v1/projects";
    fixture.urls[1] = "https://deepgram.test/v1/projects/project-a/usage/breakdown";
    fixture.urls[2] = "https://deepgram.test/v1/projects/project-b/usage/breakdown";
    fixture.statuses[0] = fixture.statuses[1] = fixture.statuses[2] = 200;
    fixture.bodies[0] = "{\"projects\":[{\"project_id\":\"project-a\",\"name\":\"Alpha\"},"
                        "{\"project_id\":\"project-b\",\"name\":\"Beta\"}]}";
    fixture.bodies[1] = "{\"start\":\"2025-01-16\",\"end\":\"2025-01-23\","
                        "\"results\":[{\"hours\":1,\"requests\":3}]}";
    fixture.bodies[2] = "{\"start\":\"2025-01-17\",\"end\":\"2025-01-24\","
                        "\"results\":[{\"hours\":4,\"requests\":6}]}";
    CodexBarProviderConfig config = {.api_key = " 'dg-key' "};
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_deepgram_fetch_with_transport(&config, stub_transport, 1000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 3);
    g_assert_cmpstr(provider->identity->login_method, ==, "2 projects");
    json_object *usage = extension(provider, "deepgramUsage");
    g_assert_cmpstr(json_object_get_string(json_object_object_get(usage, "projectID")), ==, "all");
    g_assert_cmpfloat(json_object_get_double(json_object_object_get(usage, "hours")), ==, 5.0);
    g_assert_cmpint(json_object_get_int64(json_object_object_get(usage, "requests")), ==, 9);
    codexbar_provider_free(provider);
    g_unsetenv("DEEPGRAM_API_URL");
}

static void test_poe_parser(void) {
    const char *balance = "{\"current_point_balance\":\"2500\"}";
    const char *history = "{\"data\":["
                          "{\"query_id\":\"a1\",\"creation_time\":1717000000000000,"
                          "\"bot_name\":\"GPT-4o\",\"usage_type\":\"API\",\"cost_points\":12.5,"
                          "\"cost_usd\":\"0.03\"},"
                          "{\"query_id\":\"a2\",\"creation_time\":1717003600,"
                          "\"bot_name\":\"GPT-4o\",\"usage_type\":\"Chat\",\"cost_points\":\"8\"}]}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_poe_parse_usage_bytes(
        balance, strlen(balance), history, strlen(history), 1800000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->identity->login_method, ==, "Balance: 2,500 points");
    json_object *usage = extension(provider, "poeUsage");
    json_object *entries = json_object_object_get(usage, "entries");
    json_object *daily = json_object_object_get(usage, "daily");
    g_assert_cmpuint(json_object_array_length(entries), ==, 2);
    g_assert_cmpuint(json_object_array_length(daily), ==, 1);
    g_assert_cmpfloat(json_object_get_double(json_object_object_get(json_object_array_get_idx(daily, 0), "points")),
                      ==,
                      20.5);
    codexbar_provider_free(provider);
}

static void test_poe_best_effort_and_cancellation(void) {
    reset_fixture("Bearer poe-key", 2);
    fixture.urls[0] = "https://api.poe.com/usage/current_balance";
    fixture.urls[1] = "https://api.poe.com/usage/points_history?limit=100";
    fixture.statuses[0] = 200;
    fixture.statuses[1] = 500;
    fixture.bodies[0] = "{\"current_point_balance\":1500}";
    fixture.bodies[1] = "server error";
    CodexBarProviderConfig config = {.api_key = "poe-key"};
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_poe_fetch_with_transport(&config, stub_transport, 1800000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->identity->login_method, ==, "Balance: 1,500 points");
    json_object *unused = NULL;
    g_assert_false(json_object_object_get_ex(provider->usage_extensions, "poeUsage", &unused));
    codexbar_provider_free(provider);

    GCancellable *cancellable = g_cancellable_new();
    reset_fixture("Bearer poe-key", 1);
    fixture.cancellable = cancellable;
    fixture.urls[0] = "https://api.poe.com/usage/current_balance";
    fixture.statuses[0] = 200;
    fixture.bodies[0] = "{\"current_point_balance\":1500}";
    fixture.cancel_after[0] = TRUE;
    provider = codexbar_poe_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 1800000000000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(cancellable);
}

static void test_chutes_parser(void) {
    const char *body = "{\"subscription\":{\"active\":true,\"plan_name\":\"Pro\","
                       "\"current_period_end\":\"2026-07-01T00:00:00Z\"},"
                       "\"monthly\":{\"used\":250,\"limit\":1000,\"unit\":\"credits\"},"
                       "\"rolling_window\":{\"requests\":40,\"limit\":100,"
                       "\"window_minutes\":240,\"unit\":\"requests\"}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_chutes_parse_usage_bytes(body, strlen(body), 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->identity->login_method, ==, "Pro");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    CodexBarQuotaWindow *primary = codexbar_provider_quota_window(provider, 0);
    CodexBarQuotaWindow *secondary = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpstr(primary->id, ==, "primary");
    g_assert_cmpfloat(primary->used_percent, ==, 40);
    g_assert_cmpstr(primary->detail, ==, "40/100 requests");
    g_assert_cmpstr(secondary->id, ==, "secondary");
    g_assert_cmpfloat(secondary->used_percent, ==, 25);
    g_assert_true(provider->has_subscription_renews_at);
    codexbar_provider_free(provider);

    body = "{\"quotas\":[{\"used\":0,\"limit\":100,\"window_minutes\":240},"
           "{\"used\":0,\"limit\":100,\"window_minutes\":43200}]}";
    provider = codexbar_chutes_parse_usage_bytes(body, strlen(body), 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 0)->window_minutes, ==, 240);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 1)->window_minutes, ==, 43200);
    codexbar_provider_free(provider);
}

static void test_chutes_transport_and_security(void) {
    g_setenv("CHUTES_API_URL", "https://chutes.test", TRUE);
    reset_fixture("Bearer chutes-key", 3);
    fixture.urls[0] = "https://chutes.test/users/me/subscription_usage";
    fixture.urls[1] = "https://chutes.test/users/me/quotas";
    fixture.urls[2] = "https://chutes.test/users/me/quota_usage/0";
    fixture.statuses[0] = fixture.statuses[1] = fixture.statuses[2] = 200;
    fixture.bodies[0] = "{\"subscription\":{\"active\":false,\"status\":\"free\"}}";
    fixture.bodies[1] = "[{\"chute_id\":\"0\",\"quota\":100}]";
    fixture.bodies[2] = "{\"quota\":100,\"used\":10}";
    CodexBarProviderConfig config = {.api_key = " chutes-key "};
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_chutes_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 3);
    g_assert_cmpstr(provider->identity->login_method, ==, "No active subscription");
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 10);
    codexbar_provider_free(provider);

    g_setenv("CHUTES_API_URL", "http://example.com", TRUE);
    reset_fixture(NULL, 0);
    provider = codexbar_chutes_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_unsetenv("CHUTES_API_URL");

    config.api_key = "key\r\nInjected: value";
    g_unsetenv("CHUTES_API_KEY");
    provider = codexbar_chutes_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/api-providers/deepgram/parser", test_deepgram_parser);
    g_test_add_func("/api-providers/deepgram/transport", test_deepgram_transport);
    g_test_add_func("/api-providers/poe/parser", test_poe_parser);
    g_test_add_func("/api-providers/poe/best-effort-cancellation", test_poe_best_effort_and_cancellation);
    g_test_add_func("/api-providers/chutes/parser", test_chutes_parser);
    g_test_add_func("/api-providers/chutes/transport-security", test_chutes_transport_and_security);
    return g_test_run();
}
