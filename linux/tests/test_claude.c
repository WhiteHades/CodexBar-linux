#include "claude.h"

#include <glib.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

static void test_oauth_usage(void) {
    const char *json =
        "{\"five_hour\":{\"utilization\":12.5,\"resets_at\":\"2026-07-03T00:30:00.282668+00:00\"},"
        "\"seven_day\":{\"utilization\":30,\"resets_at\":\"2026-07-08T09:00:00Z\"},"
        "\"seven_day_sonnet\":{\"utilization\":5},"
        "\"seven_day_cowork\":{\"utilization\":18},"
        "\"limits\":[{\"kind\":\"weekly_scoped\",\"group\":\"weekly\",\"percent\":7,"
        "\"resets_at\":\"2026-07-08T09:00:00Z\","
        "\"scope\":{\"model\":{\"id\":null,\"display_name\":\"Fable\"}},\"is_active\":false}],"
        "\"extra_usage\":{\"is_enabled\":true,\"monthly_limit\":2050,\"used_credits\":325,"
        "\"currency\":\"USD\"}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_claude_parse_oauth_usage(
        json, "default_claude_max_20x", "max", 1700000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->provider, ==, "claude");
    g_assert_cmpstr(provider->source, ==, "oauth");
    g_assert_cmpstr(provider->plan, ==, "Claude Max 20x");
    g_assert_cmpuint(provider->quota_windows->len, ==, 5);
    CodexBarQuotaWindow *session = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat_with_epsilon(session->used_percent, 12.5, 0.0001);
    g_assert_cmpint(session->window_minutes, ==, 300);
    g_assert_true(session->has_resets_at);
    CodexBarQuotaWindow *fable = codexbar_provider_quota_window(provider, 4);
    g_assert_cmpstr(fable->id, ==, "claude-weekly-scoped-fable");
    g_assert_cmpstr(fable->title, ==, "Fable only");
    g_assert_nonnull(provider->provider_cost);
    g_assert_cmpfloat_with_epsilon(provider->provider_cost->used, 3.25, 0.0001);
    g_assert_cmpfloat_with_epsilon(provider->provider_cost->limit, 20.5, 0.0001);
    g_assert_cmpstr(provider->provider_cost->period, ==, "Monthly cap");
    codexbar_provider_free(provider);
}

static void test_null_routines(void) {
    const char *json = "{\"five_hour\":{\"utilization\":1},\"seven_day_cowork\":null}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_claude_parse_oauth_usage(json, NULL, "pro", 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    CodexBarQuotaWindow *routines = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpstr(routines->id, ==, "claude-routines");
    g_assert_cmpfloat(routines->used_percent, ==, 0);
    codexbar_provider_free(provider);
}

static void test_spend_only(void) {
    const char *json =
        "{\"extra_usage\":{\"is_enabled\":true,\"monthly_limit\":2000,\"used_credits\":763,"
        "\"utilization\":38.15,\"currency\":\"EUR\"}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_claude_parse_oauth_usage(json, NULL, "enterprise", 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    g_assert_cmpstr(provider->provider_cost->period, ==, "Spend limit");
    g_assert_cmpfloat_with_epsilon(provider->provider_cost->used, 7.63, 0.0001);
    g_assert_cmpfloat_with_epsilon(codexbar_provider_quota_window(provider, 0)->used_percent, 38.15, 0.0001);
    codexbar_provider_free(provider);
}

static void test_invalid_usage(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_claude_parse_oauth_usage("{}", NULL, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, g_quark_from_static_string("codexbar-claude-error"), 5);
    g_clear_error(&error);
    provider = codexbar_claude_parse_oauth_usage("{", NULL, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_nonnull(error);
    g_clear_error(&error);
}

static guint transport_calls;
static guint runner_calls;
static int transport_mode;

static CodexBarHttpResponse *response(long status, const char *body) {
    CodexBarHttpResponse *value = g_new0(CodexBarHttpResponse, 1);
    value->status = status;
    value->body = g_strdup(body);
    value->body_length = strlen(body);
    return value;
}

static CodexBarHttpResponse *source_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    transport_calls++;
    if (transport_mode == 1) return response(401, "{}");
    if (transport_mode == 2) {
        if (strstr(request->url, "cost_report")) {
            return response(200,
                            "{\"data\":[{\"results\":[{\"amount\":\"850\"}]}]}");
        }
        g_assert_nonnull(strstr(request->url, "usage_report/messages"));
        return response(200,
                        "{\"data\":[{\"results\":[{\"uncached_input_tokens\":1000,"
                        "\"cache_creation\":{\"ephemeral_1h_input_tokens\":400},"
                        "\"cache_read_input_tokens\":300,\"output_tokens\":250}]}]}");
    }
    if (strstr(request->url, "/organizations/") && strstr(request->url, "/usage")) {
        return response(200,
                        "{\"five_hour\":{\"utilization\":12},"
                        "\"seven_day\":{\"utilization\":34}}");
    }
    return response(200, "[{\"uuid\":\"org-1\",\"name\":\"Example\"}]");
}

static CodexBarProcessResult *source_runner(const CodexBarProcessRequest *request,
                                           GCancellable *cancellable,
                                           GError **error) {
    (void)cancellable;
    (void)error;
    runner_calls++;
    g_assert_cmpstr(request->arguments[1], ==, "/usage");
    g_assert_null(g_environ_getenv((char **)request->environment, "ANTHROPIC_ADMIN_KEY"));
    g_assert_cmpstr(g_environ_getenv((char **)request->environment, "DISABLE_AUTOUPDATER"), ==, "1");
    CodexBarProcessResult *result = g_new0(CodexBarProcessResult, 1);
    result->standard_output = g_strdup(
        "Current session\n25% used\nCurrent week (all models)\n80% left\n"
        "Current week (Sonnet only)\n10% used\nAccount: cli@example.test\nOrg: CLI Org\n");
    result->standard_output_length = strlen(result->standard_output);
    return result;
}

static void test_cli_usage(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_claude_parse_cli_usage(
        "\033[35mCurrent session\033[0m\n93% left\n"
        "Current week (all models)\n1% used\nAccount: user@example.test\nOrg: Example Org\n",
        1000,
        &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->source, ==, "cli");
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 7);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 1)->used_percent, ==, 1);
    g_assert_cmpstr(provider->account, ==, "user@example.test");
    g_assert_cmpstr(provider->identity->organization, ==, "Example Org");
    codexbar_provider_free(provider);
}

static void test_source_planner(void) {
    CodexBarProviderConfig config = {.id = "claude", .raw = json_object_new_object()};
    json_object_object_add(config.raw, "cookieHeader", json_object_new_string("sessionKey=sk-ant-session"));
    transport_calls = 0;
    runner_calls = 0;
    transport_mode = 1;
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_claude_fetch_with_adapters(
        &config, "web", source_transport, source_runner, NULL, 1000, &error);
    g_assert_null(provider);
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_cmpuint(runner_calls, ==, 0);

    provider = codexbar_claude_fetch_with_adapters(
        &config, "auto", source_transport, source_runner, NULL, 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->source, ==, "cli");
    g_assert_cmpuint(runner_calls, ==, 1);
    codexbar_provider_free(provider);

    transport_mode = 0;
    provider = codexbar_claude_fetch_with_adapters(
        &config, "web", source_transport, source_runner, NULL, 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->source, ==, "web");
    g_assert_cmpstr(provider->identity->account_id, ==, "org-1");
    g_assert_cmpuint(runner_calls, ==, 1);
    codexbar_provider_free(provider);

    config.api_key = "admin-key";
    transport_mode = 2;
    transport_calls = 0;
    provider = codexbar_claude_fetch_with_adapters(
        &config, "auto", source_transport, source_runner, NULL, 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->source, ==, "api");
    g_assert_cmpfloat(provider->provider_cost->used, ==, 8.5);
    g_assert_cmpint(provider->token_cost->last_days_tokens, ==, 1950);
    g_assert_cmpuint(transport_calls, ==, 2);
    g_assert_cmpuint(runner_calls, ==, 1);
    codexbar_provider_free(provider);
    json_object_put(config.raw);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/claude/oauth-usage", test_oauth_usage);
    g_test_add_func("/claude/null-routines", test_null_routines);
    g_test_add_func("/claude/spend-only", test_spend_only);
    g_test_add_func("/claude/invalid-usage", test_invalid_usage);
    g_test_add_func("/claude/cli-usage", test_cli_usage);
    g_test_add_func("/claude/source-planner", test_source_planner);
    return g_test_run();
}
