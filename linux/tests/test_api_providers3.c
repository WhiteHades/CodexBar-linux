#include "api_providers3.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

typedef struct {
    const char *urls[8];
    const char *bodies[8];
    long statuses[8];
    const char *limits[8];
    const char *remaining[8];
    const char *resets[8];
    gboolean failures[8];
    gboolean cancel_after[8];
    GCancellable *cancellable;
    guint count;
} TransportFixture;

static TransportFixture fixture;
static const char *cli_output;
static const char *cli_error;
static int cli_status;
static guint cli_calls;

static CodexBarProcessResult *stub_process(const CodexBarProcessRequest *request,
                                          GCancellable *cancellable,
                                          GError **error) {
    (void)error;
    cli_calls++;
    g_assert_cmpstr(request->arguments[0], ==, "/test/arkcli");
    g_assert_cmpstr(request->arguments[1], ==, "usage");
    g_assert_cmpstr(request->arguments[2], ==, "plan");
    g_assert_cmpstr(request->arguments[3], ==, "--format");
    g_assert_cmpstr(request->arguments[4], ==, "json");
    g_assert_null(request->arguments[5]);
    g_assert_cmpuint(request->timeout_milliseconds, ==, 15000);
    g_assert_cmpuint(request->maximum_output_bytes, ==, 256U * 1024U);
    g_assert_true(request->new_session);
    g_assert_null(cancellable);
    CodexBarProcessResult *result = g_new0(CodexBarProcessResult, 1);
    result->standard_output = g_strdup(cli_output ? cli_output : "");
    result->standard_output_length = strlen(result->standard_output);
    result->standard_error = g_strdup(cli_error ? cli_error : "");
    result->standard_error_length = strlen(result->standard_error);
    result->exit_status = cli_status;
    return result;
}

static void response_header_free(gpointer data) {
    CodexBarHttpResponseHeader *header = data;
    g_free(header->name);
    g_free(header->value);
    g_free(header);
}

static void add_header(GPtrArray *headers, const char *name, const char *value) {
    if (!value) return;
    CodexBarHttpResponseHeader *header = g_new0(CodexBarHttpResponseHeader, 1);
    header->name = g_strdup(name);
    header->value = g_strdup(value);
    g_ptr_array_add(headers, header);
}

static CodexBarHttpResponse *make_response(guint index) {
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = fixture.statuses[index];
    response->body = g_strdup(fixture.bodies[index] ? fixture.bodies[index] : "{}");
    response->body_length = strlen(response->body);
    response->headers = g_ptr_array_new_with_free_func(response_header_free);
    add_header(response->headers, "X-RateLimit-Limit-Requests", fixture.limits[index]);
    add_header(response->headers, "x-ratelimit-remaining-requests", fixture.remaining[index]);
    add_header(response->headers, "X-Ratelimit-Reset-Requests", fixture.resets[index]);
    return response;
}

static const char *request_header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static CodexBarHttpResponse *stub_transport(const CodexBarHttpRequest *request, GError **error) {
    guint index = fixture.count++;
    g_assert_cmpuint(index, <, G_N_ELEMENTS(fixture.urls));
    g_assert_cmpstr(request->url, ==, fixture.urls[index]);
    g_assert_true(g_str_has_prefix(request->url, "https://"));
    g_assert_cmpstr(request->method, ==, strstr(request->url, "minimax") ? "GET" : "POST");
    g_assert_nonnull(request_header(request, "Authorization"));
    if (strstr(request->url, "open.volcengineapi.com")) {
        g_assert_cmpstr(request_header(request, "Host"), ==, "open.volcengineapi.com");
        g_assert_cmpstr(request_header(request, "X-Content-Sha256"), ==,
                        "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
        g_assert_nonnull(strstr(request_header(request, "Authorization"),
                                "SignedHeaders=content-type;host;x-content-sha256;x-date"));
        g_assert_null(strstr(request_header(request, "Authorization"), "secret"));
    }
    if (strstr(request->url, "alibaba") || strstr(request->url, "aliyun")) {
        g_assert_cmpstr(request_header(request, "x-api-key"), ==, "dash-key");
        g_assert_cmpstr(request_header(request, "X-DashScope-API-Key"), ==, "dash-key");
        g_assert_nonnull(strstr(request->body, "commodityCode"));
    }
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_DENY);
    g_assert_true(request->cancellable == fixture.cancellable);
    if (fixture.cancel_after[index] && fixture.cancellable) g_cancellable_cancel(fixture.cancellable);
    if (fixture.failures[index]) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED, "offline");
        return NULL;
    }
    return make_response(index);
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static void reset_fixture(void) {
    memset(&fixture, 0, sizeof(fixture));
}

static void assert_parse_error(CodexBarProvider *(*parser)(const char *, size_t, gint64, GError **),
                               const char *body,
                               size_t length) {
    GError *error = NULL;
    CodexBarProvider *provider = parser(body, length, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_credentials(void) {
    CodexBarProviderConfig config = {0};
    g_assert_false(codexbar_minimax_has_api_key(&config));
    g_assert_false(codexbar_alibaba_has_api_key(&config));
    g_assert_false(codexbar_doubao_has_credentials(&config));

    g_setenv("MINIMAX_CODING_API_KEY", " 'sk-cp-env' ", TRUE);
    g_setenv("ALIBABA_QWEN_API_KEY", " dash-env ", TRUE);
    g_setenv("ARK_API_KEY", "ark-env", TRUE);
    g_assert_true(codexbar_minimax_has_api_key(&config));
    g_assert_true(codexbar_alibaba_has_api_key(&config));
    g_assert_true(codexbar_doubao_has_credentials(&config));
    g_unsetenv("MINIMAX_CODING_API_KEY");
    g_unsetenv("ALIBABA_QWEN_API_KEY");
    g_unsetenv("ARK_API_KEY");

    config.api_key = "key";
    config.secret_key = "secret";
    g_assert_true(codexbar_doubao_has_credentials(&config));
    config.api_key = "bad\nkey";
    config.secret_key = NULL;
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_doubao_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
}

static void test_minimax_parse(void) {
    const char *body =
        "{\"base_resp\":{\"status_code\":0},\"current_subscribe_title\":\"Max\","
        "\"points_balance\":12.5,\"model_remains\":["
        "{\"model_name\":\"general\",\"current_interval_total_count\":1000,"
        "\"current_interval_usage_count\":250,\"start_time\":1700000000000,"
        "\"end_time\":1700018000000,\"current_weekly_remaining_percent\":70,"
        "\"current_weekly_total_count\":0,\"current_weekly_usage_count\":0,"
        "\"weekly_start_time\":1700000000000,\"weekly_end_time\":1700604800000}]}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_minimax_parse_api_usage(body, strlen(body), 1700000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->provider, ==, "minimax");
    g_assert_cmpstr(provider->plan, ==, "Max");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 75.0);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 0)->window_minutes, ==, 300);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 1)->used_percent, ==, 30.0);
    g_assert_cmpuint(provider->balances->len, ==, 1);
    codexbar_provider_free(provider);

    const char *multi =
        "{\"data\":{\"services\":[{\"service_type\":\"Text Generation\","
        "\"window_type\":\"5 hours\",\"time_range\":\"x\",\"usage\":20,\"limit\":100}]}}";
    provider = codexbar_minimax_parse_api_usage(multi, strlen(multi), 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 0)->window_minutes, ==, 300);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 20.0);
    codexbar_provider_free(provider);

    const char *invalid = "{\"base_resp\":{\"status_code\":1004,\"status_msg\":\"invalid api key\"}}";
    provider = codexbar_minimax_parse_api_usage(invalid, strlen(invalid), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    assert_parse_error(codexbar_minimax_parse_api_usage, "{}", 2);
    const char embedded_nul[] = "{}\0tail";
    assert_parse_error(codexbar_minimax_parse_api_usage, embedded_nul, sizeof(embedded_nul) - 1);
}

static void test_minimax_fetch_fallback_and_cancellation(void) {
    reset_fixture();
    fixture.urls[0] = "https://api.minimax.io/v1/token_plan/remains";
    fixture.statuses[0] = 401;
    fixture.urls[1] = "https://api.minimax.io/v1/api/openplatform/coding_plan/remains";
    fixture.statuses[1] = 404;
    fixture.urls[2] = "https://api.minimaxi.com/v1/token_plan/remains";
    fixture.statuses[2] = 200;
    fixture.bodies[2] =
        "{\"model_remains\":[{\"current_interval_total_count\":100,"
        "\"current_interval_usage_count\":75}],\"base_resp\":{\"status_code\":0}}";
    CodexBarProviderConfig config = {.api_key = "test-key"};
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_minimax_fetch_with_transport(&config, stub_transport, 1700000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 3);
    codexbar_provider_free(provider);

    reset_fixture();
    fixture.urls[0] = "https://api.minimax.io/v1/token_plan/remains";
    fixture.statuses[0] = 200;
    fixture.bodies[0] = "{}";
    fixture.cancel_after[0] = TRUE;
    fixture.cancellable = g_cancellable_new();
    provider = codexbar_minimax_fetch_with_transport_and_cancellable(
        &config, stub_transport, fixture.cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(fixture.cancellable);
}

static void test_alibaba_parse(void) {
    const char *body =
        "{\"data\":{\"codingPlanInstanceInfos\":[{\"planName\":\"Expired\","
        "\"status\":\"EXPIRED\",\"codingPlanQuotaInfo\":{\"per5HourUsedQuota\":1,"
        "\"per5HourTotalQuota\":10}},{\"planName\":\"Active Pro\",\"status\":\"VALID\","
        "\"codingPlanQuotaInfo\":{\"per5HourUsedQuota\":52,\"per5HourTotalQuota\":1000,"
        "\"per5HourQuotaNextRefreshTime\":1700000300000,\"perWeekUsedQuota\":800,"
        "\"perWeekTotalQuota\":5000,\"perBillMonthUsedQuota\":1200,"
        "\"perBillMonthTotalQuota\":20000}}]},\"status_code\":0}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_alibaba_parse_api_usage(body, strlen(body), 1700000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->plan, ==, "Active Pro");
    g_assert_cmpuint(provider->quota_windows->len, ==, 3);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 5.2);
    codexbar_provider_free(provider);

    const char *wrapped =
        "{\"successResponse\":{\"body\":\"{\\\"data\\\":{\\\"codingPlanInstanceInfos\\\":[{"
        "\\\"planName\\\":\\\"Lite\\\",\\\"status\\\":\\\"VALID\\\","
        "\\\"codingPlanQuotaInfo\\\":{\\\"per5HourUsedQuota\\\":0,"
        "\\\"per5HourTotalQuota\\\":100}}]},\\\"statusCode\\\":200}\"}}";
    provider = codexbar_alibaba_parse_api_usage(wrapped, strlen(wrapped), 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->plan, ==, "Lite");
    codexbar_provider_free(provider);

    const char *login = "{\"code\":\"NeedLogin\",\"message\":\"console session required\"}";
    provider = codexbar_alibaba_parse_api_usage(login, strlen(login), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_clear_error(&error);
    assert_parse_error(codexbar_alibaba_parse_api_usage, "{\"status_code\":0}", 17);
}

static void test_alibaba_fetch(void) {
    reset_fixture();
    fixture.urls[0] =
        "https://modelstudio.console.alibabacloud.com/data/api.json?"
        "action=zeldaEasy.broadscope-bailian.codingPlan.queryCodingPlanInstanceInfoV2&"
        "product=broadscope-bailian&api=queryCodingPlanInstanceInfoV2&currentRegionId=ap-southeast-1";
    fixture.statuses[0] = 403;
    fixture.urls[1] =
        "https://bailian.console.aliyun.com/data/api.json?"
        "action=zeldaEasy.broadscope-bailian.codingPlan.queryCodingPlanInstanceInfoV2&"
        "product=broadscope-bailian&api=queryCodingPlanInstanceInfoV2&currentRegionId=cn-beijing";
    fixture.statuses[1] = 200;
    fixture.bodies[1] =
        "{\"data\":{\"codingPlanInstanceInfos\":[{\"planName\":\"CN\",\"status\":\"VALID\","
        "\"codingPlanQuotaInfo\":{\"per5HourUsedQuota\":1,\"per5HourTotalQuota\":10}}]},"
        "\"status_code\":0}";
    CodexBarProviderConfig config = {.api_key = "dash-key"};
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_alibaba_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 2);
    codexbar_provider_free(provider);

    reset_fixture();
    fixture.urls[0] =
        "https://bailian.console.aliyun.com/data/api.json?"
        "action=zeldaEasy.broadscope-bailian.codingPlan.queryCodingPlanInstanceInfoV2&"
        "product=broadscope-bailian&api=queryCodingPlanInstanceInfoV2&currentRegionId=cn-beijing";
    fixture.statuses[0] = 401;
    config.region = "cn";
    provider = codexbar_alibaba_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
}

static void test_doubao_parse(void) {
    const char *coding =
        "{\"Result\":{\"Status\":\"Running\",\"UpdateTimestamp\":1782226444,\"QuotaUsage\":["
        "{\"Level\":\"session\",\"Percent\":12.5,\"ResetTimestamp\":1782226478},"
        "{\"Level\":\"weekly\",\"Percent\":24}]}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_doubao_parse_coding_plan(coding, strlen(coding), 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->plan, ==, "Running");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 12.5);
    g_assert_cmpint(provider->updated_at_ms, ==, 1782226444000);
    codexbar_provider_free(provider);

    const char *agent =
        "{\"Result\":{\"AFPFiveHour\":{\"Quota\":10000,\"Used\":2500,\"ResetTime\":1785686400000},"
        "\"AFPWeekly\":{\"Quota\":35000,\"Used\":0,\"ResetTime\":1785686400000}}}";
    provider = codexbar_doubao_parse_agent_plan(agent, strlen(agent), 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 25.0);
    codexbar_provider_free(provider);
    assert_parse_error(codexbar_doubao_parse_coding_plan, "{}", 2);
    assert_parse_error(codexbar_doubao_parse_agent_plan, "[]", 2);
}

static void test_doubao_cli_parse(void) {
    const char *body =
        "{\"viewer\":{\"auth_method\":\"sso\"},\"items\":["
        "{\"product\":\"coding-plan\",\"updated_at\":1784199993,\"periods\":["
        "{\"label\":\"session\",\"percent\":7.48,\"reset_at\":\"2026-08-02T12:00:00Z\"},"
        "{\"label\":\"weekly\",\"percent\":25,\"reset_at\":1786296000000}]},"
        "{\"product\":\"agent-plan-team\",\"updated_at\":1784200000000,\"periods\":["
        "{\"label\":\"5h\",\"percent\":5},{\"label\":\"monthly\",\"percent\":15}]},"
        "{\"product\":\"other\",\"periods\":[]},"
        "{\"product\":\"agent-plan\",\"subscribed\":false,\"periods\":["
        "{\"label\":\"5h\",\"percent\":99}]}]}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_doubao_parse_cli_usage(body, strlen(body), 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->source, ==, "cli");
    g_assert_cmpstr(provider->plan, ==, "sso");
    g_assert_cmpint(provider->updated_at_ms, ==, 1784200000000);
    g_assert_cmpuint(provider->quota_windows->len, ==, 4);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 7.48);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 0)->window_minutes, ==, 300);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 0)->resets_at_ms, ==, 1785672000000);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 1)->resets_at_ms, ==, 1786296000000);
    codexbar_provider_free(provider);

    const char *logged_out = "{\"viewer\":{\"auth_method\":\"none\"},\"items\":[]}";
    provider = codexbar_doubao_parse_cli_usage(logged_out, strlen(logged_out), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    const char *incomplete =
        "{\"items\":[{\"product\":\"coding-plan\",\"error\":\"no seat bound to caller\"},"
        "{\"product\":\"agent-plan\",\"periods\":[{\"label\":\"5h\",\"percent\":5}]}]}";
    provider = codexbar_doubao_parse_cli_usage(incomplete, strlen(incomplete), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull(strstr(error->message, "no seat bound to caller"));
    g_clear_error(&error);
}

static CodexBarProviderConfig doubao_cli_config(void) {
    CodexBarProviderConfig config = {0};
    config.raw = json_object_new_object();
    json_object_object_add(config.raw, "arkcliPath", json_object_new_string("/test/arkcli"));
    return config;
}

static void test_doubao_source_routing(void) {
    g_unsetenv("ARK_API_KEY");
    g_unsetenv("VOLCENGINE_API_KEY");
    g_unsetenv("DOUBAO_API_KEY");
    g_unsetenv("VOLCENGINE_ACCESS_KEY_ID");
    g_unsetenv("VOLCENGINE_ACCESS_KEY");
    g_unsetenv("VOLC_ACCESSKEY");
    g_unsetenv("DOUBAO_ACCESS_KEY_ID");
    CodexBarProviderConfig config = doubao_cli_config();
    cli_output =
        "{\"viewer\":{\"auth_method\":\"sso\"},\"items\":[{\"product\":\"coding-plan\","
        "\"periods\":[{\"label\":\"session\",\"percent\":12.5}]}]}";
    cli_error = NULL;
    cli_status = 0;
    cli_calls = 0;
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_doubao_fetch_for_source_with_adapters(
        &config, "auto", unexpected_transport, stub_process, NULL, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->source, ==, "cli");
    g_assert_cmpuint(cli_calls, ==, 1);
    codexbar_provider_free(provider);

    config.api_key = "ark-key";
    provider = codexbar_doubao_fetch_for_source_with_adapters(
        &config, "cli", unexpected_transport, stub_process, NULL, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(cli_calls, ==, 2);
    codexbar_provider_free(provider);

    reset_fixture();
    fixture.urls[0] = "https://ark.cn-beijing.volces.com/api/coding/v3/chat/completions";
    fixture.statuses[0] = 200;
    fixture.bodies[0] = "{\"usage\":{\"total_tokens\":1}}";
    provider = codexbar_doubao_fetch_for_source_with_adapters(
        &config, "auto", stub_transport, stub_process, NULL, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->source, ==, "api");
    g_assert_cmpuint(cli_calls, ==, 2);
    codexbar_provider_free(provider);

    cli_status = 1;
    cli_error = "please log in first";
    provider = codexbar_doubao_fetch_for_source_with_adapters(
        &config, "cli", unexpected_transport, stub_process, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    json_object_put(config.raw);
}

static void test_doubao_bearer(void) {
    reset_fixture();
    fixture.urls[0] = "https://ark.cn-beijing.volces.com/api/coding/v3/chat/completions";
    fixture.statuses[0] = 200;
    fixture.bodies[0] = "{\"usage\":{\"total_tokens\":1}}";
    fixture.limits[0] = "1000";
    fixture.remaining[0] = "250";
    fixture.resets[0] = "1h30m";
    CodexBarProviderConfig config = {.api_key = "ark-key"};
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_doubao_fetch_with_transport(&config, stub_transport, 1700000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 75.0);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 0)->resets_at_ms, ==, 1700005400000);
    codexbar_provider_free(provider);

    reset_fixture();
    fixture.urls[0] = "https://ark.cn-beijing.volces.com/api/coding/v3/chat/completions";
    fixture.statuses[0] = 401;
    provider = codexbar_doubao_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    reset_fixture();
    fixture.urls[0] = "https://ark.cn-beijing.volces.com/api/coding/v3/chat/completions";
    fixture.statuses[0] = 403;
    fixture.urls[1] = "https://ark.cn-beijing.volces.com/api/coding/v3/chat/completions";
    fixture.statuses[1] = 200;
    provider = codexbar_doubao_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 2);
    codexbar_provider_free(provider);

    reset_fixture();
    fixture.urls[0] = "https://ark.cn-beijing.volces.com/api/coding/v3/chat/completions";
    fixture.statuses[0] = 200;
    fixture.limits[0] = "1000";
    fixture.remaining[0] = "0";
    fixture.urls[1] = "https://ark.cn-beijing.volces.com/api/coding/v3/chat/completions";
    fixture.statuses[1] = 200;
    fixture.limits[1] = "1000";
    fixture.remaining[1] = "0";
    provider = codexbar_doubao_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(provider->quota_windows->len, ==, 0);
    codexbar_provider_free(provider);
}

static void test_doubao_signed_and_agent_fallback(void) {
    reset_fixture();
    fixture.urls[0] =
        "https://open.volcengineapi.com/?Action=GetCodingPlanUsage&Version=2024-01-01";
    fixture.statuses[0] = 200;
    fixture.bodies[0] = "{\"Result\":{\"Status\":\"Reclaimed\"}}";
    fixture.urls[1] = "https://open.volcengineapi.com/?Action=GetAFPUsage&Version=2024-01-01";
    fixture.statuses[1] = 200;
    fixture.bodies[1] =
        "{\"Result\":{\"AFPWeekly\":{\"Quota\":35000,\"Used\":7000,"
        "\"ResetTime\":1785686400000}}}";
    CodexBarProviderConfig config = {
        .api_key = "AKLTTEST",
        .secret_key = "secret",
        .region = "cn-beijing",
    };
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_doubao_fetch_with_transport(&config, stub_transport, 1781654400000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 2);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 20.0);
    codexbar_provider_free(provider);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/api-providers3/credentials", test_credentials);
    g_test_add_func("/api-providers3/minimax/parse", test_minimax_parse);
    g_test_add_func("/api-providers3/minimax/fetch", test_minimax_fetch_fallback_and_cancellation);
    g_test_add_func("/api-providers3/alibaba/parse", test_alibaba_parse);
    g_test_add_func("/api-providers3/alibaba/fetch", test_alibaba_fetch);
    g_test_add_func("/api-providers3/doubao/parse", test_doubao_parse);
    g_test_add_func("/api-providers3/doubao/cli-parse", test_doubao_cli_parse);
    g_test_add_func("/api-providers3/doubao/sources", test_doubao_source_routing);
    g_test_add_func("/api-providers3/doubao/bearer", test_doubao_bearer);
    g_test_add_func("/api-providers3/doubao/signed", test_doubao_signed_and_agent_fallback);
    return g_test_run();
}
