#include "claude.h"

#include <glib.h>
#include <math.h>

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

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/claude/oauth-usage", test_oauth_usage);
    g_test_add_func("/claude/null-routines", test_null_routines);
    g_test_add_func("/claude/spend-only", test_spend_only);
    g_test_add_func("/claude/invalid-usage", test_invalid_usage);
    return g_test_run();
}
