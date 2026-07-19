#include "openai_api.h"

#include <gio/gio.h>
#include <math.h>
#include <string.h>

static void test_usage_totals(void) {
    const char *costs =
        "{\"data\":[{\"start_time\":1730332800,\"end_time\":1730419200,\"results\":[{\"amount\":{"
        "\"value\":0.02,\"currency\":\"usd\"}}]},{\"start_time\":1730419200,\"end_time\":1730505600,"
        "\"results\":[{\"amount\":{\"value\":0.06,\"currency\":\"usd\"}}]}],\"has_more\":false}";
    const char *completions =
        "{\"data\":[{\"start_time\":1730332800,\"end_time\":1730419200,\"results\":[{\"input_tokens\":10,"
        "\"output_tokens\":5,\"num_model_requests\":1}]},{\"start_time\":1730419200,\"end_time\":1730505600,"
        "\"results\":[{\"input_tokens\":1000,\"output_tokens\":500,\"input_cached_tokens\":800,"
        "\"input_audio_tokens\":300,\"output_audio_tokens\":200,\"num_model_requests\":5}]}],"
        "\"has_more\":false}";
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_openai_api_parse_usage(costs, completions, 1730450000, "proj_abc", &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_true(fabs(provider->provider_cost->used - 0.08) < 0.0001);
    g_assert_cmpint(provider->token_cost->today_tokens, ==, 2000);
    g_assert_cmpint(provider->token_cost->last_days_tokens, ==, 2015);
    g_assert_cmpint(provider->token_cost->today_requests, ==, 5);
    g_assert_cmpint(provider->token_cost->last_days_requests, ==, 6);
    g_assert_true(fabs(provider->token_cost->today_cost - 0.06) < 0.0001);
    g_assert_cmpstr(provider->identity->login_method, ==, "Admin API: proj_abc");
    g_assert_cmpstr(provider->identity->organization, ==, "Project: proj_abc");
    codexbar_provider_free(provider);
}

static void test_overflow_fails(void) {
    const char *costs = "{\"data\":[],\"has_more\":false}";
    const char *completions =
        "{\"data\":[{\"start_time\":1,\"end_time\":2,\"results\":[{\"input_tokens\":9223372036854775807,"
        "\"output_tokens\":1}]}],\"has_more\":false}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_openai_api_parse_usage(costs, completions, 1, NULL, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_pagination_fixture_fails_closed(void) {
    const char *costs = "{\"data\":[],\"has_more\":true,\"next_page\":\"page_2\"}";
    const char *completions = "{\"data\":[],\"has_more\":false}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_openai_api_parse_usage(costs, completions, 1, NULL, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_malformed_bucket_fails(void) {
    const char *costs = "{\"data\":[{\"results\":[]}],\"has_more\":false}";
    const char *completions = "{\"data\":[],\"has_more\":false}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_openai_api_parse_usage(costs, completions, 1, NULL, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_request_url(void) {
    char *url = codexbar_openai_api_url("https://api.openai.com/v1/organization/costs",
                                       100,
                                       200,
                                       "line_item",
                                       "proj a/b",
                                       "page=2");
    g_assert_nonnull(strstr(url, "start_time=100&end_time=200&bucket_width=1d&limit=30"));
    g_assert_nonnull(strstr(url, "group_by=line_item"));
    g_assert_nonnull(strstr(url, "project_ids=proj%20a%2Fb"));
    g_assert_nonnull(strstr(url, "page=page%3D2"));
    g_free(url);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/openai-api/totals", test_usage_totals);
    g_test_add_func("/openai-api/overflow", test_overflow_fails);
    g_test_add_func("/openai-api/pagination", test_pagination_fixture_fails_closed);
    g_test_add_func("/openai-api/malformed-bucket", test_malformed_bucket_fails);
    g_test_add_func("/openai-api/url", test_request_url);
    return g_test_run();
}
