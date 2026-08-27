#include "web_providers5.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

typedef enum {
    FIXTURE_ALIBABA,
    FIXTURE_MIMO,
    FIXTURE_WRONG_ORIGIN,
} FixtureKind;

typedef struct {
    FixtureKind kind;
    guint count;
    GCancellable *cancellable;
    gboolean cancel;
} Fixture;

static Fixture fixture;
static guint alibaba_personal_count;
static gboolean alibaba_personal_china;
static gboolean alibaba_personal_user_info;
static gboolean alibaba_personal_empty_usage_once;
static guint alibaba_personal_usage_requests;

static CodexBarHttpResponse *response(long status, const char *body, const char *url) {
    CodexBarHttpResponse *value = g_new0(CodexBarHttpResponse, 1);
    value->status = status;
    value->body = g_strdup(body);
    value->body_length = strlen(body);
    value->headers = g_ptr_array_new();
    value->effective_url = g_strdup(url);
    return value;
}

static const char *header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) {
            return request->headers[index].value;
        }
    }
    return NULL;
}

static void assert_policy(const CodexBarHttpRequest *request) {
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == fixture.cancellable);
}

static CodexBarHttpResponse *stub_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    fixture.count++;
    assert_policy(request);
    if (fixture.cancel && fixture.cancellable) g_cancellable_cancel(fixture.cancellable);
    if (fixture.kind == FIXTURE_WRONG_ORIGIN) {
        return response(200, "{}", "https://attacker.example/stolen");
    }
    if (fixture.kind == FIXTURE_ALIBABA) {
        g_assert_cmpstr(request->url, ==,
                        "https://bailian.console.aliyun.com/data/api.json?"
                        "action=GetSubscriptionSummary&product=BssOpenAPI-V3&_tag=");
        g_assert_cmpstr(request->method, ==, "POST");
        g_assert_cmpint(request->timeout_seconds, ==, 20);
        g_assert_cmpstr(header(request, "Cookie"), ==,
                        "login_aliyunid_ticket=ticket; sec_token=sec; csrf=csrf-value");
        g_assert_cmpstr(header(request, "Origin"), ==, "https://bailian.console.aliyun.com");
        g_assert_cmpstr(header(request, "x-xsrf-token"), ==, "csrf-value");
        g_assert_nonnull(strstr(request->body, "sfm_tokenplanteams_dp_cn"));
        g_assert_nonnull(strstr(request->body, "sec_token=sec"));
        return response(200,
                        "{\"Success\":true,\"Data\":{\"TotalCount\":1,"
                        "\"TotalValue\":1000,\"TotalSurplusValue\":875,"
                        "\"NearestExpireDate\":1790000000000}}",
                        request->url);
    }
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_nonnull(strstr(header(request, "Cookie"), "api-platform_serviceToken=svc"));
    g_assert_nonnull(strstr(header(request, "Cookie"), "userId=123"));
    g_assert_cmpstr(header(request, "x-timeZone"), ==, "UTC+01:00");
    if (fixture.count == 1) {
        g_assert_true(g_str_has_suffix(request->url, "/balance"));
        return response(200,
                        "{\"code\":0,\"data\":{\"balance\":\"25.51\","
                        "\"currency\":\"USD\",\"cashBalance\":\"20\","
                        "\"giftBalance\":\"5.51\"}}",
                        request->url);
    }
    if (fixture.count == 2) {
        g_assert_true(g_str_has_suffix(request->url, "/tokenPlan/detail"));
        return response(200,
                        "{\"code\":0,\"data\":{\"planCode\":\"standard\","
                        "\"currentPeriodEnd\":\"2026-09-01 00:00:00\",\"expired\":false}}",
                        request->url);
    }
    g_assert_true(g_str_has_suffix(request->url, "/tokenPlan/usage"));
    return response(200,
                    "{\"code\":0,\"data\":{\"monthUsage\":{\"percent\":0.0505,"
                    "\"items\":[{\"name\":\"month_total_token\",\"used\":10100158,"
                    "\"limit\":200000000,\"percent\":0.0505}]}}}",
                    request->url);
}

static CodexBarHttpResponse *alibaba_personal_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    assert_policy(request);
    guint index = alibaba_personal_count++;
    g_assert_cmpstr(header(request, "Cookie"), ==,
                    "login_aliyunid_ticket=ticket; sec_token=cookie-sec; cna=anon; csrf=csrf-value");
    if (index == 0) {
        g_assert_cmpstr(request->method, ==, "GET");
        g_assert_cmpstr(request->url,
                        ==,
                        alibaba_personal_china
                            ? "https://bailian.console.aliyun.com/cn-beijing?tab=plan#/efm/subscription/token-plan/personal"
                            : "https://modelstudio.console.alibabacloud.com/ap-southeast-1/"
                              "?tab=plan#/efm/subscription/token-plan/personal");
        return response(200,
                        alibaba_personal_user_info
                            ? "<html></html>"
                            : "<script>window.config={SEC_TOKEN: \"personal-sec-token\"}</script>",
                        request->url);
    }
    if (alibaba_personal_user_info && index == 1) {
        g_assert_cmpstr(request->method, ==, "GET");
        g_assert_cmpstr(request->url, ==,
                        alibaba_personal_china
                            ? "https://bailian.console.aliyun.com/tool/user/info.json"
                            : "https://modelstudio.console.alibabacloud.com/tool/user/info.json");
        return response(200, "{\"code\":\"200\",\"data\":{\"secToken\":\"personal-sec-token\"}}", request->url);
    }

    guint api_index = index - (alibaba_personal_user_info ? 1 : 0);
    g_assert_cmpstr(request->method, ==, "POST");
    g_assert_true(g_str_has_prefix(
        request->url,
        alibaba_personal_china
            ? "https://bailian-cs.console.aliyun.com/data/api.json?action=BroadScopeAspnGateway&"
              "product=sfm_bailian&api=zeldaHttp.apikeyMgr./tokenplan/personal/api/v2/"
            : "https://bailian-singapore-cs.alibabacloud.com/data/api.json?"
              "action=IntlBroadScopeAspnGateway&product=sfm_bailian&"
              "api=zeldaHttp.apikeyMgr./tokenplan/personal/api/v2/"));
    g_assert_cmpstr(header(request, "Origin"), ==,
                    alibaba_personal_china ? "https://bailian.console.aliyun.com"
                                           : "https://modelstudio.console.alibabacloud.com");
    g_assert_cmpstr(header(request, "x-xsrf-token"), ==, "csrf-value");
    g_assert_nonnull(strstr(request->body, "sec_token=personal-sec-token"));
    g_assert_null(strstr(request->body, "switchAgent"));
    g_assert_nonnull(strstr(request->body, "switchUserType"));

    if (strstr(request->url, "/usage&_v=undefined")) {
        g_assert_nonnull(strstr(request->url, "/usage&_v=undefined"));
        alibaba_personal_usage_requests++;
        if (alibaba_personal_empty_usage_once && alibaba_personal_usage_requests == 1) {
            return response(200,
                            "{\"code\":\"SUCCESS\",\"data\":{},\"successResponse\":true}",
                            request->url);
        }
        return response(200,
                        "{\"code\":\"200\",\"data\":{\"DataV2\":{\"data\":{"
                        "\"success\":true,\"data\":{\"per5HourPercentage\":0.25,"
                        "\"per5HourResetTime\":1784813220000,\"per1WeekPercentage\":0.1,"
                        "\"per1WeekResetTime\":1785234900000}}}},\"successResponse\":true}",
                        request->url);
    }
    if (strstr(request->url, "/subscription&_v=undefined")) {
        g_assert_nonnull(strstr(request->url, "/subscription&_v=undefined"));
        g_assert_nonnull(strstr(request->body,
                                alibaba_personal_china ? "sfm_tokenplansolo_public_cn"
                                                       : "sfm_tokenplansolo_public_intl"));
        return response(200,
                        "{\"code\":\"200\",\"data\":{\"DataV2\":{\"data\":{"
                        "\"success\":true,\"data\":{\"specCode\":\"pro\"}}}},"
                        "\"successResponse\":true}",
                        request->url);
    }
    (void)api_index;
    g_assert_nonnull(strstr(request->url, "/quota-config&_v=undefined"));
    return response(200,
                    "{\"code\":\"200\",\"data\":{\"DataV2\":{\"data\":{"
                    "\"success\":true,\"data\":{\"pro\":{\"five_hour\":12000,"
                    "\"weekly\":40000}}}}},\"successResponse\":true}",
                    request->url);
}

static CodexBarProviderConfig config_with_cookie(const char *cookie) {
    CodexBarProviderConfig config = {0};
    config.raw = json_object_new_object();
    json_object_object_add(config.raw, "cookieHeader", json_object_new_string(cookie));
    return config;
}

static void clear_config(CodexBarProviderConfig *config) {
    if (config->raw) json_object_put(config->raw);
    g_free(config->region);
    g_free(config->source);
    *config = (CodexBarProviderConfig){0};
}

static void reset_fixture(FixtureKind kind) {
    fixture = (Fixture){.kind = kind};
}

static void test_alibaba_parsers(void) {
    GError *error = NULL;
    const char *captured =
        "{\"Success\":true,\"Data\":{\"TotalCount\":1,\"TotalValue\":1000,"
        "\"TotalSurplusValue\":875,\"NearestExpireDate\":1701000000000}}";
    CodexBarProvider *provider = codexbar_alibaba_token_plan_parse(
        captured, strlen(captured), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->plan, ==, "TOKEN PLAN");
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 12.5);
    g_assert_cmpfloat(codexbar_provider_balance(provider, 0)->remaining, ==, 875);
    codexbar_provider_free(provider);

    const char *nested =
        "{\"successResponse\":{\"body\":\"{\\\"success\\\":true,"
        "\\\"data\\\":{\\\"totalCount\\\":1,\\\"totalValue\\\":1000,"
        "\\\"totalSurplusValue\\\":750}}\"}}";
    provider = codexbar_alibaba_token_plan_parse(nested, strlen(nested), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 25);
    codexbar_provider_free(provider);

    const char *login = "{\"code\":\"ConsoleNeedLogin\",\"message\":\"login\"}";
    provider = codexbar_alibaba_token_plan_parse(login, strlen(login), 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    const char *workspace =
        "{\"code\":\"200\",\"data\":{\"success\":false,"
        "\"errorCode\":\"BailianGateway.Workspace.NotAuthorised\","
        "\"errorMsg\":\"BailianGateway.Workspace.NotAuthorised\"},"
        "\"successResponse\":true}";
    provider = codexbar_alibaba_token_plan_parse(workspace, strlen(workspace), 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr(error->message, "Workspace.NotAuthorised"));
    g_clear_error(&error);
}

static void test_alibaba_transport(void) {
    GError *error = NULL;
    CodexBarProviderConfig config = config_with_cookie(
        "login_aliyunid_ticket=ticket; sec_token=sec; csrf=csrf-value");
    config.region = g_strdup("cn");
    reset_fixture(FIXTURE_ALIBABA);
    CodexBarProvider *provider = codexbar_alibaba_token_plan_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 1);
    codexbar_provider_free(provider);
    clear_config(&config);

    const char *regions[] = {"cn-personal", "intl-personal"};
    for (guint index = 0; index < G_N_ELEMENTS(regions); index++) {
        config = config_with_cookie(
            "login_aliyunid_ticket=ticket; sec_token=cookie-sec; cna=anon; csrf=csrf-value");
        config.region = g_strdup(regions[index]);
        alibaba_personal_china = index == 0;
        alibaba_personal_user_info = index == 1;
        alibaba_personal_empty_usage_once = FALSE;
        alibaba_personal_usage_requests = 0;
        alibaba_personal_count = 0;
        provider = codexbar_alibaba_token_plan_fetch_with_transport_and_cancellable(
            &config, alibaba_personal_transport, NULL, 1000, &error);
        g_assert_no_error(error);
        g_assert_nonnull(provider);
        g_assert_cmpuint(alibaba_personal_count, ==, alibaba_personal_user_info ? 5 : 4);
        g_assert_cmpstr(provider->plan, ==, "Pro");
        g_assert_cmpuint(provider->quota_windows->len, ==, 2);
        g_assert_cmpstr(codexbar_provider_quota_window(provider, 0)->id, ==, "primary");
        g_assert_cmpint(codexbar_provider_quota_window(provider, 0)->window_minutes, ==, 300);
        g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 25.0);
        g_assert_cmpstr(codexbar_provider_quota_window(provider, 1)->id, ==, "secondary");
        g_assert_cmpint(codexbar_provider_quota_window(provider, 1)->window_minutes, ==, 10080);
        g_assert_cmpfloat(codexbar_provider_quota_window(provider, 1)->used_percent, ==, 10.0);
        codexbar_provider_free(provider);
        clear_config(&config);
    }

    config = config_with_cookie(
        "login_aliyunid_ticket=ticket; sec_token=cookie-sec; cna=anon; csrf=csrf-value");
    config.region = g_strdup("cn-personal");
    alibaba_personal_china = TRUE;
    alibaba_personal_user_info = FALSE;
    alibaba_personal_empty_usage_once = TRUE;
    alibaba_personal_usage_requests = 0;
    alibaba_personal_count = 0;
    provider = codexbar_alibaba_token_plan_fetch_with_transport_and_cancellable(
        &config, alibaba_personal_transport, NULL, 1000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(alibaba_personal_usage_requests, ==, 2);
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    codexbar_provider_free(provider);
    clear_config(&config);
}

static void test_mimo_parser(void) {
    const char *balance =
        "{\"code\":0,\"data\":{\"balance\":\"25.51\",\"currency\":\"USD\","
        "\"cashBalance\":\"20\",\"giftBalance\":\"5.51\"}}";
    const char *detail =
        "{\"code\":0,\"data\":{\"planCode\":\"standard\","
        "\"currentPeriodEnd\":\"2026-09-01 00:00:00\",\"expired\":false}}";
    const char *usage =
        "{\"code\":0,\"data\":{\"monthUsage\":{\"items\":[{"
        "\"name\":\"month_total_token\",\"used\":10100158,\"limit\":200000000,"
        "\"percent\":0.0505}]}}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_mimo_parse(
        balance, strlen(balance), detail, strlen(detail), usage, strlen(usage), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->plan, ==, "standard");
    g_assert_cmpuint(provider->balances->len, ==, 3);
    g_assert_cmpfloat(codexbar_provider_balance(provider, 0)->remaining, ==, 25.51);
    g_assert_cmpfloat_with_epsilon(
        codexbar_provider_quota_window(provider, 0)->used_percent, 5.05, 0.0001);
    g_assert_true(codexbar_provider_quota_window(provider, 0)->has_resets_at);
    g_assert_true(codexbar_provider_quota_window(provider, 0)->has_window_minutes);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 0)->window_minutes, ==, 43200);
    codexbar_provider_free(provider);

    const char *detail_without_reset = "{\"code\":0,\"data\":{\"planCode\":\"standard\"}}";
    provider = codexbar_mimo_parse(balance, strlen(balance),
                                   detail_without_reset, strlen(detail_without_reset),
                                   usage, strlen(usage), 1000, &error);
    g_assert_no_error(error);
    g_assert_false(codexbar_provider_quota_window(provider, 0)->has_resets_at);
    g_assert_false(codexbar_provider_quota_window(provider, 0)->has_window_minutes);
    codexbar_provider_free(provider);

    provider = codexbar_mimo_parse("{}", 2, NULL, 0, NULL, 0, 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);

    const char *local =
        "{\"updated_at\":\"2026-07-07T10:00:00Z\",\"sessions_scanned\":42,"
        "\"windows\":{\"today\":{\"input\":1000,\"output\":500},"
        "\"week\":{\"input\":30000,\"output\":10000,\"cache_read\":60000,"
        "\"cache_create\":10000},\"all_time\":{\"input\":1000000,"
        "\"output\":500000}}}";
    provider = codexbar_mimo_local_parse(local, strlen(local), 1783504800000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->source, ==, "local");
    g_assert_cmpstr(provider->plan, ==,
                    "Local · 1.5k today · 110.0k week · 1.5M total · 42 sessions · stale 1d");
    g_assert_cmpuint(provider->quota_windows->len, ==, 0);
    g_assert_cmpuint(provider->balances->len, ==, 0);
    codexbar_provider_free(provider);
}

static void test_mimo_transport(void) {
    GError *error = NULL;
    CodexBarProviderConfig config = config_with_cookie(
        "curl 'https://platform.xiaomimimo.com/api/v1/balance' -H "
        "'Cookie: ignored=value; userId=123; api-platform_serviceToken=svc; api-platform_ph=ph'");
    reset_fixture(FIXTURE_MIMO);
    CodexBarProvider *provider = codexbar_mimo_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 3);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    codexbar_provider_free(provider);
    clear_config(&config);
}

static void test_security_and_cancellation(void) {
    GError *error = NULL;
    CodexBarProviderConfig config = config_with_cookie(
        "userId=123; api-platform_serviceToken=svc");
    config.source = g_strdup("web");
    reset_fixture(FIXTURE_WRONG_ORIGIN);
    CodexBarProvider *provider = codexbar_mimo_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    reset_fixture(FIXTURE_MIMO);
    fixture.cancellable = g_cancellable_new();
    fixture.cancel = TRUE;
    provider = codexbar_mimo_fetch_with_transport_and_cancellable(
        &config, stub_transport, fixture.cancellable, 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(fixture.cancellable);
    clear_config(&config);

    config = config_with_cookie("userId=123\nInjected: yes; api-platform_serviceToken=svc");
    provider = codexbar_mimo_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    clear_config(&config);

    config = config_with_cookie("sec_token=sec");
    config.region = g_strdup("intl-personal");
    g_setenv("ALIBABA_TOKEN_PLAN_QUOTA_URL", "https://quota.example/path?leak=1", TRUE);
    provider = codexbar_alibaba_token_plan_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_unsetenv("ALIBABA_TOKEN_PLAN_QUOTA_URL");
    clear_config(&config);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/web5/alibaba/parsers", test_alibaba_parsers);
    g_test_add_func("/web5/alibaba/transport", test_alibaba_transport);
    g_test_add_func("/web5/mimo/parser", test_mimo_parser);
    g_test_add_func("/web5/mimo/transport", test_mimo_transport);
    g_test_add_func("/web5/security-cancellation", test_security_and_cancellation);
    return g_test_run();
}
