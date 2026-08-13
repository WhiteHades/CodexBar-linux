#include "zai.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

static void test_three_limits(void) {
    const char *json =
        "{\"code\":200,\"success\":true,\"data\":{\"level\":\"pro\",\"limits\":["
        "{\"type\":\"TIME_LIMIT\",\"unit\":5,\"number\":1,\"usage\":1000,\"currentValue\":147,"
        "\"remaining\":853,\"percentage\":14,\"nextResetTime\":1784706344993},"
        "{\"type\":\"TOKENS_LIMIT\",\"unit\":3,\"number\":5,\"percentage\":8,"
        "\"nextResetTime\":1783049703178},"
        "{\"type\":\"TOKENS_LIMIT\",\"unit\":6,\"number\":1,\"percentage\":7,"
        "\"nextResetTime\":1783496744998}]}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_zai_parse_usage(json, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->plan, ==, "Pro");
    g_assert_cmpuint(provider->quota_windows->len, ==, 3);

    CodexBarQuotaWindow *weekly = codexbar_provider_quota_window(provider, 0);
    CodexBarQuotaWindow *mcp = codexbar_provider_quota_window(provider, 1);
    CodexBarQuotaWindow *session = codexbar_provider_quota_window(provider, 2);
    g_assert_cmpint(weekly->window_minutes, ==, 10080);
    g_assert_true(fabs(weekly->used_percent - 7.0) < 0.0001);
    g_assert_true(fabs(mcp->used_percent - 14.7) < 0.0001);
    g_assert_cmpstr(mcp->reset_description, ==, "Monthly");
    g_assert_cmpint(session->window_minutes, ==, 300);
    g_assert_true(fabs(session->used_percent - 8.0) < 0.0001);
    codexbar_provider_free(provider);
}

static void test_missing_quota_fields(void) {
    const char *json =
        "{\"code\":200,\"success\":true,\"data\":{\"limits\":[{\"type\":\"TOKENS_LIMIT\","
        "\"unit\":3,\"number\":5,\"percentage\":1}]}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_zai_parse_usage(json, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, 0);
    g_assert_true(window->usage_known);
    g_assert_true(fabs(window->used_percent - 1.0) < 0.0001);
    codexbar_provider_free(provider);
}

static void test_raw_percentage_fallback_normalization(void) {
    const struct {
        double percentage;
        double expected;
    } cases[] = {
        {150.0, 100.0},
        {-5.0, 0.0},
        {42.0, 42.0},
    };
    for (guint index = 0; index < G_N_ELEMENTS(cases); index++) {
        char *json = g_strdup_printf(
            "{\"code\":200,\"success\":true,\"data\":{\"limits\":[{"
            "\"type\":\"TOKENS_LIMIT\",\"unit\":3,\"number\":5,\"percentage\":%.2f}]}}",
            cases[index].percentage);
        GError *error = NULL;
        CodexBarProvider *provider = codexbar_zai_parse_usage(json, &error);
        g_assert_no_error(error);
        g_assert_nonnull(provider);
        g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, cases[index].expected);
        codexbar_provider_free(provider);
        g_free(json);
    }

    const char *computed =
        "{\"code\":200,\"success\":true,\"data\":{\"limits\":[{"
        "\"type\":\"TOKENS_LIMIT\",\"unit\":3,\"number\":5,\"usage\":100,"
        "\"currentValue\":25,\"percentage\":999}]}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_zai_parse_usage(computed, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 25.0);
    codexbar_provider_free(provider);
}

static void test_credit_limits(void) {
    const char *json =
        "{\"code\":200,\"success\":true,\"data\":{\"level\":\"lite\",\"limits\":["
        "{\"type\":\"CREDIT_LIMIT\",\"unit\":3,\"number\":5,\"usage\":1000,"
        "\"currentValue\":35.5,\"remaining\":964.5,\"percentage\":3.55,"
        "\"nextResetTime\":1784706344993},"
        "{\"type\":\"CREDIT_LIMIT\",\"unit\":6,\"number\":1,\"usage\":5000,"
        "\"currentValue\":500,\"remaining\":4500,\"percentage\":10,"
        "\"nextResetTime\":1785138344993}]}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_zai_parse_usage(json, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->plan, ==, "Lite");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    CodexBarQuotaWindow *weekly = codexbar_provider_quota_window(provider, 0);
    CodexBarQuotaWindow *session = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpint(weekly->window_minutes, ==, 10080);
    g_assert_cmpint(session->window_minutes, ==, 300);
    g_assert_cmpfloat_with_epsilon(session->used_percent, 3.55, 0.0001);
    g_assert_cmpstr(session->reset_description, ==, "5-hour");
    g_assert_cmpint(session->resets_at_ms, ==, G_GINT64_CONSTANT(1784706344993));
    codexbar_provider_free(provider);
}

static void test_api_errors(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_zai_parse_usage(
        "{\"code\":1001,\"msg\":\"secret remote text\",\"success\":false}", &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_null(strstr(error->message, "secret remote text"));
    g_clear_error(&error);

    provider = codexbar_zai_parse_usage("{\"code\":200,\"success\":true}", &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_quota_urls(void) {
    CodexBarProviderConfig config = {0};
    GError *error = NULL;
    char *url = codexbar_zai_quota_url(&config, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "https://api.z.ai/api/monitor/usage/quota/limit");
    g_free(url);

    config.region = "bigmodel-cn";
    url = codexbar_zai_quota_url(&config, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "https://open.bigmodel.cn/api/monitor/usage/quota/limit");
    g_free(url);

    g_setenv("Z_AI_API_HOST", "open.bigmodel.cn", TRUE);
    url = codexbar_zai_quota_url(&config, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "https://open.bigmodel.cn/api/monitor/usage/quota/limit");
    g_free(url);
    g_unsetenv("Z_AI_API_HOST");

    g_setenv("Z_AI_QUOTA_URL", "http://example.com/quota", TRUE);
    url = codexbar_zai_quota_url(&config, &error);
    g_assert_null(url);
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_unsetenv("Z_AI_QUOTA_URL");

    config.raw = json_tokener_parse("{\"usageScope\":\"team\"}");
    url = codexbar_zai_quota_url(&config, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "https://open.bigmodel.cn/api/monitor/usage/quota/limit?type=2");
    g_free(url);
    json_object_put(config.raw);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/zai/three-limits", test_three_limits);
    g_test_add_func("/zai/missing-fields", test_missing_quota_fields);
    g_test_add_func("/zai/raw-percentage-normalization", test_raw_percentage_fallback_normalization);
    g_test_add_func("/zai/credit-limits", test_credit_limits);
    g_test_add_func("/zai/api-errors", test_api_errors);
    g_test_add_func("/zai/urls", test_quota_urls);
    return g_test_run();
}
