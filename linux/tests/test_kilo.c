#include "kilo.h"

#include <gio/gio.h>
#include <math.h>
#include <string.h>

static void test_batch_url(void) {
    char *url = codexbar_kilo_usage_url();
    g_assert_nonnull(strstr(url, "user.getCreditBlocks,kiloPass.getState,user.getAutoTopUpPaymentMethod"));
    g_assert_nonnull(strstr(url, "batch=1&input=%7B%220%22%3A%7B%22json%22%3Anull%7D"));
    g_free(url);
}

static void test_business_fields(void) {
    const char *json =
        "[{\"result\":{\"data\":{\"json\":{\"blocks\":[{\"usedCredits\":25,\"totalCredits\":100,"
        "\"remainingCredits\":75}]}}}},{\"result\":{\"data\":{\"json\":{\"plan\":{\"name\":\"Kilo Pass "
        "Pro\"}}}}},{\"result\":{\"data\":{\"json\":{\"enabled\":true,\"paymentMethod\":\"visa\"}}}}]";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_kilo_parse_usage(json, "api", NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, 0);
    g_assert_nonnull(window);
    g_assert_true(fabs(window->used_percent - 25.0) < 0.0001);
    g_assert_cmpstr(window->reset_description, ==, "25/100 credits");
    g_assert_cmpstr(provider->plan, ==, "Kilo Pass Pro");
    g_assert_cmpstr(provider->identity->login_method, ==, "Kilo Pass Pro \xC2\xB7 Auto top-up: visa");
    codexbar_provider_free(provider);
}

static void test_subscription_and_microdollars(void) {
    const char *json =
        "[{\"result\":{\"data\":{\"creditBlocks\":[{\"balance_mUsd\":19000000,\"amount_mUsd\":"
        "19000000}],\"totalBalance_mUsd\":19000000,\"autoTopUpEnabled\":false}}},{\"result\":{\"data\":{"
        "\"subscription\":{\"tier\":\"tier_19\",\"currentPeriodUsageUsd\":0,"
        "\"currentPeriodBaseCreditsUsd\":19,\"currentPeriodBonusCreditsUsd\":9.5,"
        "\"nextBillingAt\":\"2026-03-28T04:00:00.000Z\"}}}},{\"result\":{\"data\":{\"enabled\":false}}}]";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_kilo_parse_usage(json, "cli", "org_42", &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    CodexBarQuotaWindow *primary = codexbar_provider_quota_window(provider, 0);
    CodexBarQuotaWindow *secondary = codexbar_provider_quota_window(provider, 1);
    g_assert_nonnull(primary);
    g_assert_nonnull(secondary);
    g_assert_true(fabs(primary->used_percent) < 0.0001);
    g_assert_true(fabs(secondary->used_percent) < 0.0001);
    g_assert_true(secondary->has_resets_at);
    g_assert_cmpstr(secondary->reset_description, ==, "$0.00 / $19.00 (+ $9.50 bonus)");
    g_assert_cmpstr(provider->identity->login_method, ==, "Starter \xC2\xB7 Auto top-up: off");
    g_assert_cmpstr(provider->identity->organization, ==, "org_42");
    g_assert_cmpstr(provider->source, ==, "cli");
    codexbar_provider_free(provider);
}

static void test_sparse_optional_error(void) {
    const char *json =
        "{\"0\":{\"result\":{\"data\":{\"json\":{\"creditsUsed\":10,\"creditsRemaining\":90}}}},"
        "\"2\":{\"error\":{\"json\":{\"message\":\"Internal server error\"}}}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_kilo_parse_usage(json, "api", NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_true(fabs(codexbar_provider_quota_window(provider, 0)->used_percent - 10.0) < 0.0001);
    codexbar_provider_free(provider);
}

static void test_zero_balance(void) {
    const char *json =
        "[{\"result\":{\"data\":{\"creditBlocks\":[],\"totalBalance_mUsd\":0,\"autoTopUpEnabled\":false}}},"
        "{\"result\":{\"data\":{\"subscription\":null}}},{\"result\":{\"data\":{\"enabled\":false}}}]";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_kilo_parse_usage(json, "api", NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, 0);
    g_assert_nonnull(window);
    g_assert_true(fabs(window->used_percent - 100.0) < 0.0001);
    g_assert_cmpstr(window->reset_description, ==, "0/0 credits");
    codexbar_provider_free(provider);
}

static void test_required_error(void) {
    const char *json = "[{\"error\":{\"json\":{\"message\":\"Unauthorized\",\"data\":{\"code\":"
                       "\"UNAUTHORIZED\"}}}}]";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_kilo_parse_usage(json, "api", NULL, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
}

static void test_auth_token(void) {
    GError *error = NULL;
    char *token = codexbar_kilo_parse_auth_token("{\"kilo\":{\"access\":\" token-value \"}}", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(token, ==, "token-value");
    g_free(token);
    token = codexbar_kilo_parse_auth_token("{\"kilo\":{}}", &error);
    g_assert_null(token);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_invalid_json(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_kilo_parse_usage("not-json", "api", NULL, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/kilo/batch-url", test_batch_url);
    g_test_add_func("/kilo/business-fields", test_business_fields);
    g_test_add_func("/kilo/subscription", test_subscription_and_microdollars);
    g_test_add_func("/kilo/sparse-optional-error", test_sparse_optional_error);
    g_test_add_func("/kilo/zero-balance", test_zero_balance);
    g_test_add_func("/kilo/required-error", test_required_error);
    g_test_add_func("/kilo/auth-token", test_auth_token);
    g_test_add_func("/kilo/invalid-json", test_invalid_json);
    return g_test_run();
}
