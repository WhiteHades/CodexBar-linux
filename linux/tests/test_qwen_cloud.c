#include "qwen_cloud.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

typedef struct {
    guint count;
    GCancellable *cancellable;
    GPtrArray *apis;
} TransportFixture;

static TransportFixture fixture;

static const char *nested_equity_fixture =
    "{\"code\":\"200\",\"successResponse\":true,\"data\":{\"TotalCount\":1,\"Data\":[{"
    "\"InstanceCode\":\"qwen-token-plan\",\"Status\":\"NORMAL\",\"EndTime\":1.701e12,"
    "\"EquityList\":[{\"Type\":\"CREDITS\",\"CycleTotalValue\":\"1000\","
    "\"CycleSurplusValue\":\"875\"}]}]}}";

static const char *flat_summary_fixture =
    "{\"Success\":true,\"Data\":{\"TotalCount\":1,\"TotalValue\":2000,"
    "\"TotalSurplusValue\":1500}}";

static const char *no_subscription_fixture =
    "{\"code\":\"200\",\"data\":{\"Data\":{\"TotalSurplusValue\":\"0\",\"TotalCount\":0,"
    "\"TotalValue\":\"0\",\"ProductCode\":\"sfm_tokenplansolo_public_intl\"},\"Success\":true},"
    "\"httpStatusCode\":\"200\",\"successResponse\":true}";

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

static char *form_value(const CodexBarHttpRequest *request, const char *name) {
    char *body = g_strndup(request->body, request->body_length);
    char **parts = g_strsplit(body, "&", -1);
    char *result = NULL;
    for (size_t index = 0; parts[index]; index++) {
        char *equals = strchr(parts[index], '=');
        if (!equals) continue;
        *equals = '\0';
        if (!g_str_equal(parts[index], name)) continue;
        result = g_uri_unescape_string(equals + 1, NULL);
        break;
    }
    g_strfreev(parts);
    g_free(body);
    return result;
}

static CodexBarHttpResponse *fetch_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    guint index = fixture.count++;
    g_assert_true(request->cancellable == fixture.cancellable);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_DENY);
    g_assert_cmpstr(request_header(request, "Cookie"), ==,
                    "login_aliyunid_ticket=ticket; login_aliyunid_csrf=csrf-value; cna=anonymous-id");
    GUri *uri = g_uri_parse(request->url, G_URI_FLAGS_NONE, NULL);
    g_assert_nonnull(uri);
    g_assert_cmpstr(g_uri_get_host(uri), ==, "qwen-cloud.test");

    if (index == 0) {
        g_assert_cmpstr(request->method, ==, "GET");
        g_assert_cmpstr(g_uri_get_path(uri), ==, "/billing/subscription/token-plan-individual");
        g_assert_cmpstr(request_header(request, "Accept"), ==, "text/html,application/xhtml+xml");
        g_uri_unref(uri);
        return make_response(200, "<html><script>sec_token = \"qwen-html-token\";</script></html>");
    }

    g_assert_cmpstr(request->method, ==, "POST");
    g_assert_cmpstr(g_uri_get_path(uri), ==, "/data/api.json");
    g_assert_cmpstr(request_header(request, "Origin"), ==, "https://qwen-cloud.test");
    g_assert_cmpstr(request_header(request, "Referer"), ==,
                    "https://qwen-cloud.test/billing/subscription/token-plan-individual");
    g_assert_cmpstr(request_header(request, "x-xsrf-token"), ==, "csrf-value");
    g_assert_cmpstr(request_header(request, "x-csrf-token"), ==, "csrf-value");
    g_assert_cmpint(request->timeout_seconds, ==, 30);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 2U * 1024U * 1024U);
    g_uri_unref(uri);

    char *token = form_value(request, "sec_token");
    char *product = form_value(request, "product");
    char *params_text = form_value(request, "params");
    g_assert_cmpstr(token, ==, "qwen-html-token");
    g_assert_cmpstr(product, ==, "sfm_bailian");
    json_object *params = json_tokener_parse(params_text);
    g_assert_nonnull(params);
    json_object *api_value = NULL;
    json_object *data = NULL;
    json_object *cornerstone = NULL;
    g_assert_true(json_object_object_get_ex(params, "Api", &api_value));
    g_assert_true(json_object_object_get_ex(params, "Data", &data));
    g_assert_true(json_object_object_get_ex(data, "cornerstoneParam", &cornerstone));
    g_assert_cmpstr(json_object_get_string(json_object_object_get(cornerstone, "consoleSite")), ==, "QWENCLOUD");
    g_assert_cmpstr(json_object_get_string(json_object_object_get(cornerstone, "X-Anonymous-Id")), ==,
                    "anonymous-id");
    const char *api = json_object_get_string(api_value);
    g_ptr_array_add(fixture.apis, g_strdup(api));

    CodexBarHttpResponse *response = NULL;
    if (g_str_has_suffix(api, "/usage")) {
        response = make_response(
            200,
            "{\"data\":{\"per5HourPercentage\":0.03,\"per5HourResetTime\":1700003600000,"
            "\"per1WeekPercentage\":0.01,\"per1WeekResetTime\":1700086400000}}");
    } else if (g_str_has_suffix(api, "/subscription")) {
        g_assert_cmpstr(json_object_get_string(json_object_object_get(data, "commodityCode")), ==,
                        "sfm_tokenplansolo_public_intl");
        response = make_response(200, "{\"data\":{\"specCode\":\"standard\",\"status\":\"VALID\"}}");
    } else if (g_str_has_suffix(api, "/quota-config")) {
        response = make_response(
            200,
            "{\"data\":{\"lite\":{\"five_hour\":1000,\"weekly\":10000},"
            "\"standard\":{\"five_hour\":5000,\"weekly\":50000}}}");
    } else {
        g_assert_not_reached();
    }
    json_object_put(params);
    g_free(params_text);
    g_free(product);
    g_free(token);
    return response;
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static void test_current_embedded_usage(void) {
    const char *usage =
        "{\"data\":{\"DataV2\":{\"data\":\"{\\\"code\\\":0,\\\"data\\\":{"
        "\\\"per5HourPercentage\\\":0.03,\\\"per5HourResetTime\\\":1700003600000,"
        "\\\"per1WeekPercentage\\\":0.01,\\\"per1WeekResetTime\\\":1700086400000},"
        "\\\"success\\\":true}\"}},\"httpStatusCode\":200}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_qwen_cloud_parse_usage(usage, NULL, NULL, 1700000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->provider, ==, "qwencloud");
    g_assert_cmpstr(provider->source, ==, "web");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    CodexBarQuotaWindow *primary = codexbar_provider_quota_window(provider, 0);
    CodexBarQuotaWindow *secondary = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpstr(primary->title, ==, "5-hour");
    g_assert_cmpfloat_with_epsilon(primary->used_percent, 3.0, 0.000001);
    g_assert_cmpint(primary->window_minutes, ==, 300);
    g_assert_cmpint(primary->resets_at_ms, ==, 1700003600000);
    g_assert_cmpstr(secondary->title, ==, "Weekly");
    g_assert_cmpfloat_with_epsilon(secondary->used_percent, 1.0, 0.000001);
    g_assert_cmpint(secondary->window_minutes, ==, 10080);
    codexbar_provider_free(provider);
}

static void test_upstream_legacy_fixtures(void) {
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_qwen_cloud_parse_usage(nested_equity_fixture, NULL, NULL, 1700000000000, &error);
    g_assert_no_error(error);
    CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, 0);
    g_assert_nonnull(window);
    g_assert_cmpfloat_with_epsilon(window->used_percent, 12.5, 0.000001);
    g_assert_cmpint(window->resets_at_ms, ==, 1701000000000);
    g_assert_cmpstr(window->reset_description, ==, "125 / 1,000 credits used");
    codexbar_provider_free(provider);

    provider = codexbar_qwen_cloud_parse_usage(flat_summary_fixture, NULL, NULL, 1, &error);
    g_assert_no_error(error);
    window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat_with_epsilon(window->used_percent, 25.0, 0.000001);
    g_assert_cmpstr(window->reset_description, ==, "500 / 2,000 credits used");
    codexbar_provider_free(provider);

    provider = codexbar_qwen_cloud_parse_usage(no_subscription_fixture, NULL, NULL, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 0);
    codexbar_provider_free(provider);
}

static void test_error_fixtures(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_qwen_cloud_parse_usage(
        "{\"code\":\"ConsoleNeedLogin\",\"message\":\"You need to log in.\",\"successResponse\":false}",
        NULL,
        NULL,
        1,
        &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_nonnull(strstr(error->message, "login required"));
    g_clear_error(&error);

    provider = codexbar_qwen_cloud_parse_usage(
        "{\"statusCode\":403,\"message\":\"Forbidden\"}", NULL, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_nonnull(strstr(error->message, "rejected"));
    g_clear_error(&error);

    provider = codexbar_qwen_cloud_parse_usage("not-json", NULL, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_cookie_sources(void) {
    g_setenv("QWEN_CLOUD_COOKIE", "\"login_aliyunid_ticket=environment\"", TRUE);
    json_object *raw = json_tokener_parse(
        "{\"cookieSource\":\"manual\",\"cookieHeader\":\" Cookie: login_aliyunid_ticket=manual \"}");
    CodexBarProviderConfig config = {.raw = raw};
    g_assert_true(codexbar_qwen_cloud_has_cookie(&config));
    json_object_put(raw);

    raw = json_tokener_parse("{\"cookieSource\":\"off\"}");
    config.raw = raw;
    g_assert_false(codexbar_qwen_cloud_has_cookie(&config));
    json_object_put(raw);

    config.raw = NULL;
    g_assert_true(codexbar_qwen_cloud_has_cookie(&config));
    g_unsetenv("QWEN_CLOUD_COOKIE");
    g_assert_false(codexbar_qwen_cloud_has_cookie(&config));
}

static void test_fetch_transport(void) {
    g_setenv("QWEN_CLOUD_HOST", "qwen-cloud.test", TRUE);
    g_unsetenv("QWEN_CLOUD_QUOTA_URL");
    json_object *raw = json_tokener_parse(
        "{\"cookieSource\":\"manual\",\"cookieHeader\":"
        "\"login_aliyunid_ticket=ticket; login_aliyunid_csrf=csrf-value; cna=anonymous-id\"}");
    CodexBarProviderConfig config = {.raw = raw};
    memset(&fixture, 0, sizeof(fixture));
    fixture.cancellable = g_cancellable_new();
    fixture.apis = g_ptr_array_new_with_free_func(g_free);
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_qwen_cloud_fetch_with_transport_and_cancellable(
        &config, fetch_transport, fixture.cancellable, 1700000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 4);
    g_assert_cmpuint(fixture.apis->len, ==, 3);
    g_assert_true(g_str_has_suffix(g_ptr_array_index(fixture.apis, 0), "/usage"));
    g_assert_true(g_str_has_suffix(g_ptr_array_index(fixture.apis, 1), "/subscription"));
    g_assert_true(g_str_has_suffix(g_ptr_array_index(fixture.apis, 2), "/quota-config"));
    g_assert_cmpstr(provider->plan, ==, "Standard");
    g_assert_cmpstr(provider->identity->login_method, ==, "Standard");
    CodexBarQuotaWindow *primary = codexbar_provider_quota_window(provider, 0);
    CodexBarQuotaWindow *secondary = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpstr(primary->reset_description, ==, "150 / 5,000 credits used");
    g_assert_cmpstr(secondary->reset_description, ==, "500 / 50,000 credits used");
    codexbar_provider_free(provider);
    g_ptr_array_unref(fixture.apis);
    g_object_unref(fixture.cancellable);
    json_object_put(raw);
    g_unsetenv("QWEN_CLOUD_HOST");
}

static void test_credential_host_boundary(void) {
    json_object *raw = json_tokener_parse(
        "{\"cookieSource\":\"manual\",\"cookieHeader\":\"login_aliyunid_ticket=ticket\"}");
    CodexBarProviderConfig config = {.raw = raw};
    GError *error = NULL;
    g_setenv("QWEN_CLOUD_HOST", "https://evil.example", TRUE);
    CodexBarProvider *provider =
        codexbar_qwen_cloud_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    g_setenv("QWEN_CLOUD_HOST", "qwen-cloud.test", TRUE);
    g_setenv("QWEN_CLOUD_QUOTA_URL", "https://evil.example/collect", TRUE);
    provider = codexbar_qwen_cloud_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    g_unsetenv("QWEN_CLOUD_QUOTA_URL");
    g_unsetenv("QWEN_CLOUD_HOST");
    json_object_put(raw);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/qwen-cloud/current-embedded", test_current_embedded_usage);
    g_test_add_func("/qwen-cloud/upstream-legacy-fixtures", test_upstream_legacy_fixtures);
    g_test_add_func("/qwen-cloud/error-fixtures", test_error_fixtures);
    g_test_add_func("/qwen-cloud/cookie-sources", test_cookie_sources);
    g_test_add_func("/qwen-cloud/fetch-transport", test_fetch_transport);
    g_test_add_func("/qwen-cloud/credential-host-boundary", test_credential_host_boundary);
    return g_test_run();
}
