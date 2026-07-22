#include "clinepass.h"

#include <gio/gio.h>
#include <string.h>

static const char *expected_authorization;
static long response_status;
static const char *response_body;

static CodexBarHttpResponse *make_response(long status, const char *body) {
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = status;
    response->body = g_strdup(body);
    response->body_length = strlen(body);
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
    g_assert_cmpstr(request->url, ==, "https://api.cline.bot/api/v1/users/me/plan/usage-limits");
    g_assert_cmpstr(request->method, ==, "GET");
    g_assert_cmpstr(request_header(request, "Authorization"), ==, expected_authorization);
    g_assert_cmpstr(request_header(request, "Accept"), ==, "application/json");
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_DENY);
    return make_response(response_status, response_body);
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static CodexBarHttpResponse *cancelled_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
    return NULL;
}

static gint64 timestamp_ms(const char *raw) {
    GDateTime *time = g_date_time_new_from_iso8601(raw, NULL);
    g_assert_nonnull(time);
    gint64 result = g_date_time_to_unix(time) * 1000 + g_date_time_get_microsecond(time) / 1000;
    g_date_time_unref(time);
    return result;
}

static void test_parse_all_windows(void) {
    const char *json =
        "{\"data\":{\"limits\":["
        "{\"type\":\"five_hour\",\"percentUsed\":12.5,\"resetsAt\":\"2026-07-16T10:20:30Z\"},"
        "{\"type\":\"weekly\",\"percentUsed\":34,\"resetsAt\":\"2026-07-20T00:00:00.123Z\"},"
        "{\"type\":\"monthly\",\"percentUsed\":156.75,\"resetsAt\":\"2026-08-01T00:00:00Z\"}"
        "]},\"success\":true}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_clinepass_parse_usage(json, 123000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->provider, ==, "clinepass");
    g_assert_cmpstr(provider->source, ==, "api");
    g_assert_cmpstr(provider->identity->login_method, ==, "API key");
    g_assert_cmpint(provider->updated_at_ms, ==, 123000);
    g_assert_true(provider->explicit_quota_slots);
    g_assert_cmpuint(provider->quota_windows->len, ==, 3);
    CodexBarQuotaWindow *primary = codexbar_provider_quota_window(provider, 0);
    CodexBarQuotaWindow *secondary = codexbar_provider_quota_window(provider, 1);
    CodexBarQuotaWindow *tertiary = codexbar_provider_quota_window(provider, 2);
    g_assert_cmpstr(primary->id, ==, "primary");
    g_assert_cmpfloat_with_epsilon(primary->used_percent, 12.5, 0.0001);
    g_assert_cmpint(primary->window_minutes, ==, 300);
    g_assert_cmpint(primary->resets_at_ms, ==, timestamp_ms("2026-07-16T10:20:30Z"));
    g_assert_cmpstr(secondary->id, ==, "secondary");
    g_assert_cmpfloat_with_epsilon(secondary->used_percent, 34.0, 0.0001);
    g_assert_cmpint(secondary->window_minutes, ==, 10080);
    g_assert_cmpint(secondary->resets_at_ms, ==, timestamp_ms("2026-07-20T00:00:00.123Z"));
    g_assert_cmpstr(tertiary->id, ==, "tertiary");
    g_assert_cmpfloat(tertiary->used_percent, ==, 100.0);
    g_assert_cmpint(tertiary->window_minutes, ==, 43200);
    codexbar_provider_free(provider);
}

static void test_parse_sparse_and_unknown_windows(void) {
    const char *json =
        "{\"data\":{\"limits\":["
        "{\"type\":\"future\",\"percentUsed\":90,\"resetsAt\":\"not-a-date\"},"
        "{\"type\":\"five_hour\\u0000future\",\"percentUsed\":80},"
        "{\"type\":\"weekly\",\"percentUsed\":-40}"
        "]},\"success\":true}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_clinepass_parse_usage(json, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpstr(window->id, ==, "secondary");
    g_assert_cmpfloat(window->used_percent, ==, 0.0);
    g_assert_false(window->has_resets_at);
    codexbar_provider_free(provider);
}

static void assert_parse_error(const char *json) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_clinepass_parse_usage(json, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void assert_parse_bytes_error(const char *json, size_t length) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_clinepass_parse_usage_bytes(json, length, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_parse_errors(void) {
    assert_parse_error("{\"data\":{\"limits\":[]},\"success\":false}");
    assert_parse_error("{\"data\":{\"limits\":[{\"type\":\"weekly\",\"percentUsed\":\"forty\"}]},\"success\":true}");
    assert_parse_error("{\"data\":{\"limits\":[{\"type\":\"weekly\",\"percentUsed\":40,"
                       "\"resetsAt\":\"invalid\"}]},\"success\":true}");
    assert_parse_error("{\"data\":{\"limits\":[{\"type\":\"weekly\",\"percentUsed\":40,"
                       "\"resetsAt\":\"2026-07-20T00:00:00Z\\u0000tail\"}]},\"success\":true}");
    assert_parse_error("{\"data\":{\"limits\":[],},\"success\":true}");
    assert_parse_error("{\"data\":{\"limits\":[]},\"success\":true} trailing");
    static const char embedded_nul[] = "{\"data\":{\"limits\":[]},\"success\":true}\0trailing";
    assert_parse_bytes_error(embedded_nul, sizeof(embedded_nul) - 1);
}

static void test_fetch_request_and_credentials(void) {
    g_setenv("CLINE_API_KEY", "environment", TRUE);
    g_setenv("CLINEPASS_API_KEY", "alternate", TRUE);
    CodexBarProviderConfig config = {.api_key = " 'test-key' "};
    expected_authorization = "Bearer test-key";
    response_status = 200;
    response_body = "{\"data\":{\"limits\":[{\"type\":\"five_hour\",\"percentUsed\":25}]},\"success\":true}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_clinepass_fetch_with_transport(&config, stub_transport, 456000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 25.0);
    g_assert_cmpint(provider->updated_at_ms, ==, 456000);
    codexbar_provider_free(provider);

    config.api_key = NULL;
    g_setenv("CLINE_API_KEY", " 'primary' ", TRUE);
    expected_authorization = "Bearer primary";
    provider = codexbar_clinepass_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    codexbar_provider_free(provider);
    g_unsetenv("CLINE_API_KEY");

    expected_authorization = "Bearer alternate";
    provider = codexbar_clinepass_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    codexbar_provider_free(provider);
    g_unsetenv("CLINEPASS_API_KEY");
}

static void test_fetch_errors(void) {
    g_unsetenv("CLINE_API_KEY");
    g_unsetenv("CLINEPASS_API_KEY");
    CodexBarProviderConfig config = {0};
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_clinepass_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

    config.api_key = "'";
    provider = codexbar_clinepass_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

    config.api_key = "test-key";
    expected_authorization = "Bearer test-key";
    response_body = "{}";
    response_status = 401;
    provider = codexbar_clinepass_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    response_status = 503;
    provider = codexbar_clinepass_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr(error->message, "503"));
    g_clear_error(&error);

    provider = codexbar_clinepass_fetch_with_transport(&config, cancelled_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/clinepass/parse-all-windows", test_parse_all_windows);
    g_test_add_func("/clinepass/parse-sparse-unknown", test_parse_sparse_and_unknown_windows);
    g_test_add_func("/clinepass/parse-errors", test_parse_errors);
    g_test_add_func("/clinepass/fetch-request-credentials", test_fetch_request_and_credentials);
    g_test_add_func("/clinepass/fetch-errors", test_fetch_errors);
    return g_test_run();
}
