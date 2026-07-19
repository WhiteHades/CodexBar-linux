#include "copilot.h"

#include <gio/gio.h>
#include <math.h>

static void test_direct_and_monthly_quotas(void) {
    const char *json =
        "{\"copilot_plan\":\"free\",\"quota_reset_date\":\"2026-08-01T00:00:00Z\","
        "\"quota_snapshots\":{\"premium_interactions\":{\"entitlement\":200,\"remaining\":156.2,"
        "\"percent_remaining\":78.1},\"chat\":{\"entitlement\":0,\"remaining\":0,\"unlimited\":true}},"
        "\"monthly_quotas\":{\"chat\":500},\"limited_user_quotas\":{\"chat\":125}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_copilot_parse_usage(json, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->plan, ==, "Free");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);

    CodexBarQuotaWindow *premium = codexbar_provider_quota_window(provider, 0);
    CodexBarQuotaWindow *chat = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpstr(premium->id, ==, "premium");
    g_assert_true(fabs(premium->used_percent - 21.9) < 0.0001);
    g_assert_true(premium->has_resets_at);
    g_assert_cmpstr(chat->id, ==, "chat");
    g_assert_true(fabs(chat->used_percent - 75.0) < 0.0001);
    codexbar_provider_free(provider);
}

static void test_token_billing_without_fake_quota(void) {
    const char *json =
        "{\"copilot_plan\":\"business\",\"token_based_billing\":true,\"quota_snapshots\":{"
        "\"premium_interactions\":{\"entitlement\":0,\"remaining\":0,\"percent_remaining\":100},"
        "\"chat\":{\"entitlement\":0,\"remaining\":0,\"percent_remaining\":100}}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_copilot_parse_usage(json, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(provider->quota_windows->len, ==, 0);
    g_assert_cmpstr(provider->identity->login_method, ==, "Business");
    codexbar_provider_free(provider);
}

static void test_unknown_and_over_quota(void) {
    const char *json =
        "{\"copilot_plan\":\"paid\",\"quota_snapshots\":{\"premium_interactions\":{},"
        "\"mystery_bucket\":{\"entitlement\":500,\"remaining\":-75}}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_copilot_parse_usage(json, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    CodexBarQuotaWindow *chat = codexbar_provider_quota_window(provider, 0);
    g_assert_true(fabs(chat->used_percent - 115.0) < 0.0001);
    g_assert_cmpstr(chat->reset_description, ==, "115% used");
    codexbar_provider_free(provider);
}

static void test_unusable_payload_fails(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_copilot_parse_usage(
        "{\"copilot_plan\":\"free\",\"monthly_quotas\":{\"chat\":500}}", &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_usage_urls(void) {
    GError *error = NULL;
    char *url = codexbar_copilot_usage_url(NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "https://api.github.com/copilot_internal/user");
    g_free(url);

    url = codexbar_copilot_usage_url("https://octocorp.ghe.com/settings", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "https://api.octocorp.ghe.com/copilot_internal/user");
    g_free(url);

    url = codexbar_copilot_usage_url("github.com", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "https://api.github.com/copilot_internal/user");
    g_free(url);

    url = codexbar_copilot_usage_url("api.octocorp.ghe.com:8443", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "https://api.octocorp.ghe.com:8443/copilot_internal/user");
    g_free(url);

    url = codexbar_copilot_usage_url("https://user@example.com", &error);
    g_assert_null(url);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    url = codexbar_copilot_usage_url("bad..example.com", &error);
    g_assert_null(url);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/copilot/direct-and-monthly", test_direct_and_monthly_quotas);
    g_test_add_func("/copilot/token-billing", test_token_billing_without_fake_quota);
    g_test_add_func("/copilot/unknown-over-quota", test_unknown_and_over_quota);
    g_test_add_func("/copilot/unusable", test_unusable_payload_fails);
    g_test_add_func("/copilot/urls", test_usage_urls);
    return g_test_run();
}
