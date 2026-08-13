#include "codex.h"
#include "openrouter.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

static CodexBarBalance *balance(CodexBarProvider *provider, guint index) {
    CodexBarBalance *result = codexbar_provider_balance(provider, index);
    g_assert_nonnull(result);
    return result;
}

static CodexBarQuotaWindow *window(CodexBarProvider *provider, guint index) {
    CodexBarQuotaWindow *result = codexbar_provider_quota_window(provider, index);
    g_assert_nonnull(result);
    return result;
}

static void test_codex_spend_control_credit_limit(void) {
    const char *usage =
        "{\"plan_type\":\"team\",\"rate_limit\":{\"primary_window\":{\"used_percent\":100,"
        "\"reset_at\":1786161204,\"limit_window_seconds\":604800}},"
        "\"credits\":{\"has_credits\":true,\"balance\":null},\"spend_control\":{"
        "\"individual_limit\":{\"limit\":\"1000\",\"used\":\"36.79748725891113\","
        "\"remaining_percent\":96,\"reset_at\":1788220800}}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_codex_parse_http_usage(
        usage, "oauth", 1800000000000LL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    g_assert_cmpuint(provider->balances->len, ==, 2);
    g_assert_cmpstr(balance(provider, 0)->id, ==, "credits");
    g_assert_cmpfloat(balance(provider, 0)->remaining, ==, 0.0);
    CodexBarBalance *limit = balance(provider, 1);
    g_assert_cmpstr(limit->id, ==, "codex-credit-limit");
    g_assert_cmpfloat_with_epsilon(limit->limit, 1000.0, 0.0001);
    g_assert_cmpfloat_with_epsilon(limit->used, 36.79748725891113, 1e-9);
    g_assert_cmpfloat_with_epsilon(limit->remaining, 963.2025127410889, 1e-9);
    g_assert_cmpfloat(limit->remaining_percent, ==, 96.0);
    g_assert_cmpint(limit->resets_at_ms, ==, G_GINT64_CONSTANT(1788220800000));
    codexbar_provider_free(provider);

    usage =
        "{\"individual_limit\":{\"limit\":500,\"used\":100,\"remaining_percent\":80},"
        "\"rate_limit\":{\"individual_limit\":{\"limit\":750,\"used\":150}},"
        "\"credits\":{\"has_credits\":true,\"balance\":\"12.5\"},"
        "\"spend_control\":{\"individual_limit\":{\"limit\":1000,\"used\":200}}}";
    provider = codexbar_codex_parse_http_usage(usage, "oauth", 1800000000000LL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(provider->balances->len, ==, 2);
    g_assert_cmpfloat(balance(provider, 0)->remaining, ==, 12.5);
    g_assert_cmpstr(balance(provider, 1)->id, ==, "codex-credit-limit");
    g_assert_cmpfloat(balance(provider, 1)->limit, ==, 500.0);
    g_assert_cmpfloat(balance(provider, 1)->used, ==, 100.0);
    codexbar_provider_free(provider);
}

static guint openrouter_calls;

static CodexBarHttpResponse *openrouter_response(const char *body) {
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = 200;
    response->body = g_strdup(body);
    response->body_length = strlen(body);
    response->headers = g_ptr_array_new();
    return response;
}

static CodexBarHttpResponse *openrouter_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    g_assert_cmpstr(request->method, ==, "GET");
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_DENY);
    gboolean authorized = FALSE;
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_str_equal(request->headers[index].name, "Authorization") &&
            g_str_equal(request->headers[index].value, "Bearer account-token")) {
            authorized = TRUE;
        }
    }
    g_assert_true(authorized);
    openrouter_calls++;
    if (g_str_has_suffix(request->url, "/credits")) {
        g_assert_cmpuint(request->timeout_seconds, ==, 15);
        return openrouter_response("{\"data\":{\"total_credits\":100,\"total_usage\":40}}");
    }
    g_assert_true(g_str_has_suffix(request->url, "/key"));
    g_assert_cmpuint(request->timeout_seconds, ==, 1);
    return openrouter_response(
        "{\"data\":{\"limit\":500,\"limit_remaining\":454.542594979,"
        "\"limit_reset\":\"monthly\",\"usage\":433.286754736,"
        "\"usage_monthly\":45.457405021}}");
}

static void test_openrouter_key_remaining(void) {
    CodexBarProviderConfig config = {
        .id = "openrouter",
        .api_key = "account-token",
        .raw = json_object_new_object(),
    };
    json_object_object_add(config.raw, "tokenAccountSelected", json_object_new_boolean(TRUE));
    openrouter_calls = 0;
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_openrouter_fetch_with_transport(
        &config, openrouter_transport, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(openrouter_calls, ==, 2);
    g_assert_cmpuint(provider->balances->len, ==, 1);
    g_assert_cmpfloat_with_epsilon(balance(provider, 0)->remaining, 60.0, 0.0001);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    g_assert_cmpfloat_with_epsilon(window(provider, 0)->used_percent, 9.0914810042, 1e-9);
    codexbar_provider_free(provider);
    json_object_put(config.raw);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/billing-parity/codex-spend-control", test_codex_spend_control_credit_limit);
    g_test_add_func("/billing-parity/openrouter-key-remaining", test_openrouter_key_remaining);
    return g_test_run();
}
