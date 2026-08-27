#include "web_providers4.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

typedef enum {
    FIXTURE_COMMAND,
    FIXTURE_QODER,
    FIXTURE_PERPLEXITY,
    FIXTURE_LONGCAT,
    FIXTURE_WRONG_ORIGIN,
} FixtureKind;

typedef struct {
    FixtureKind kind;
    guint count;
    GCancellable *cancellable;
    gboolean cancel_in_transport;
} TransportFixture;

static TransportFixture fixture;

static CodexBarHttpResponse *response(long status, const char *body, const char *effective_url) {
    CodexBarHttpResponse *value = g_new0(CodexBarHttpResponse, 1);
    value->status = status;
    value->body = g_strdup(body);
    value->body_length = strlen(body);
    value->headers = g_ptr_array_new();
    value->effective_url = g_strdup(effective_url);
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
    long expected_timeout = fixture.kind == FIXTURE_COMMAND && fixture.count == 2 ? 2 : 15;
    g_assert_cmpint(request->timeout_seconds, ==, expected_timeout);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == fixture.cancellable);
}

static const char *command_credits(void) {
    return "{\"credits\":{\"monthlyCredits\":8.75,\"purchasedCredits\":3,"
           "\"premiumMonthlyCredits\":0,\"opensourceMonthlyCredits\":2},"
           "\"windowLimits\":{\"fiveHour\":{\"cap\":3,\"used\":0.75,"
           "\"resetAt\":1780000000000},\"weekly\":{\"cap\":15,\"used\":1.5,"
           "\"resetAt\":1780100000000}}}";
}

static const char *command_subscription(void) {
    return "{\"success\":true,\"data\":{\"planId\":\"individual-go\","
           "\"status\":\"active\",\"currentPeriodEnd\":\"2026-09-01T00:00:00Z\"}}";
}

static const char *perplexity_credits(void) {
    return "{\"balance_cents\":23065,\"renewal_date_ts\":1788220800,"
           "\"current_period_purchased_cents\":3000,\"credit_grants\":["
           "{\"type\":\"recurring\",\"amount_cents\":10000,\"expires_at_ts\":1790000000},"
           "{\"type\":\"purchased\",\"amount_cents\":40000},"
           "{\"type\":\"promotional\",\"amount_cents\":55000,\"expires_at_ts\":1791000000}],"
           "\"total_usage_cents\":81935}";
}

static CodexBarHttpResponse *stub_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    fixture.count++;
    assert_policy(request);
    if (fixture.cancel_in_transport && fixture.cancellable) g_cancellable_cancel(fixture.cancellable);
    if (fixture.kind == FIXTURE_WRONG_ORIGIN) {
        return response(200, command_credits(), "https://attacker.example/stolen");
    }
    switch (fixture.kind) {
    case FIXTURE_COMMAND:
        g_assert_cmpstr(header(request, "Cookie"), ==,
                        "__Secure-better-auth.session_token=secret");
        g_assert_cmpstr(header(request, "Origin"), ==, "https://commandcode.ai");
        if (fixture.count == 1) {
            g_assert_cmpstr(request->url, ==,
                            "https://api.commandcode.ai/internal/billing/credits");
            return response(200, command_credits(), request->url);
        }
        g_assert_cmpstr(request->url, ==,
                        "https://api.commandcode.ai/internal/billing/subscriptions");
        return response(200, command_subscription(), request->url);
    case FIXTURE_QODER:
        g_assert_cmpstr(request->url, ==,
                        "https://qoder.com.cn/api/v2/me/usages/big_model_credits");
        g_assert_cmpstr(header(request, "Cookie"), ==, "session=qoder");
        g_assert_cmpstr(header(request, "X-Requested-With"), ==, "XMLHttpRequest");
        g_assert_cmpstr(header(request, "Bx-V"), ==, "2.5.35");
        return response(200,
                        "{\"total_quota\":{\"quota_summary\":{\"used_value\":25,"
                        "\"limit_value\":100,\"remaining_value\":75,\"usage_percentage\":25}},"
                        "\"next_reset_at\":1788220800000}",
                        request->url);
    case FIXTURE_PERPLEXITY:
        g_assert_cmpstr(header(request, "Cookie"), ==,
                        "__Secure-authjs.session-token=perplexity-token");
        return response(200, perplexity_credits(), request->url);
    case FIXTURE_LONGCAT:
        g_assert_cmpstr(header(request, "Cookie"), ==, "passport_token=abc; uid=42");
        if (fixture.count == 1) {
            return response(200, "{\"code\":0,\"data\":{\"name\":\"LongCat User\"}}", request->url);
        }
        if (fixture.count == 2) {
            return response(200,
                            "{\"code\":0,\"data\":{\"usage\":{\"totalToken\":500000,"
                            "\"usedToken\":120000,\"availableToken\":380000}}}",
                            request->url);
        }
        return response(200,
                        "{\"code\":0,\"data\":{\"totalQuota\":1000,\"list\":["
                        "{\"availableToken\":600,\"expireTime\":1790000000000},"
                        "{\"availableToken\":150,\"expireTime\":1791000000000}]}}",
                        request->url);
    case FIXTURE_WRONG_ORIGIN:
        break;
    }
    g_assert_not_reached();
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
    *config = (CodexBarProviderConfig){0};
}

static void reset_fixture(FixtureKind kind) {
    fixture = (TransportFixture){.kind = kind};
}

static void test_command_parser_and_transport(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_commandcode_parse(
        command_credits(), strlen(command_credits()),
        command_subscription(), strlen(command_subscription()), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->plan, ==, "Go");
    g_assert_true(provider->explicit_quota_slots);
    g_assert_cmpuint(provider->quota_windows->len, ==, 3);
    const CodexBarQuotaWindow *five_hour = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpstr(five_hour->id, ==, "primary");
    g_assert_cmpfloat(five_hour->used_percent, ==, 25);
    g_assert_cmpint(five_hour->window_minutes, ==, 300);
    g_assert_cmpint(five_hour->resets_at_ms, ==, 1780000000000LL);
    const CodexBarQuotaWindow *weekly = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpstr(weekly->id, ==, "secondary");
    g_assert_cmpfloat(weekly->used_percent, ==, 10);
    g_assert_cmpint(weekly->window_minutes, ==, 10080);
    g_assert_cmpint(weekly->resets_at_ms, ==, 1780100000000LL);
    const CodexBarQuotaWindow *monthly = codexbar_provider_quota_window(provider, 2);
    g_assert_cmpstr(monthly->id, ==, "tertiary");
    g_assert_cmpfloat_with_epsilon(
        monthly->used_percent, 12.5, 0.0001);
    g_assert_cmpint(monthly->window_minutes, ==, 31 * 24 * 60);
    g_assert_cmpuint(provider->balances->len, ==, 3);
    codexbar_provider_free(provider);

    const char *nested =
        "{\"credits\":{\"monthlyCredits\":7.25,\"purchasedCredits\":2,"
        "\"premiumMonthlyCredits\":0,\"opensourceMonthlyCredits\":0,\"windowLimits\":{"
        "\"fiveHour\":{\"cap\":\"4\",\"used\":\"1\",\"resetAt\":\"1780200000\"},"
        "\"weekly\":{\"cap\":20,\"used\":4,\"resetAt\":1780300000000}}}}";
    provider = codexbar_commandcode_parse(nested, strlen(nested), NULL, 0, 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 3);
    five_hour = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat(five_hour->used_percent, ==, 25);
    g_assert_cmpint(five_hour->resets_at_ms, ==, 1780200000000LL);
    weekly = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpfloat(weekly->used_percent, ==, 20);
    g_assert_cmpint(weekly->resets_at_ms, ==, 1780300000000LL);
    codexbar_provider_free(provider);

    const char *goat_subscription =
        "{\"success\":true,\"data\":{\"planId\":\"individual-goat\","
        "\"status\":\"active\",\"currentPeriodEnd\":\"2026-09-01T00:00:00Z\"}}";
    provider = codexbar_commandcode_parse(
        command_credits(), strlen(command_credits()), goat_subscription, strlen(goat_subscription), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->plan, ==, "GOAT");
    monthly = codexbar_provider_quota_window(provider, 2);
    g_assert_cmpfloat_with_epsilon(monthly->used_percent, 87.5, 0.0001);
    codexbar_provider_free(provider);

    const char *pro_v1_subscription =
        "{\"success\":true,\"data\":{\"planId\":\"individual-pro-v1\","
        "\"status\":\"active\",\"currentPeriodEnd\":\"2026-09-01T00:00:00Z\"}}";
    provider = codexbar_commandcode_parse(
        command_credits(), strlen(command_credits()), pro_v1_subscription, strlen(pro_v1_subscription), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->plan, ==, "Pro");
    monthly = codexbar_provider_quota_window(provider, 2);
    g_assert_cmpfloat_with_epsilon(monthly->used_percent, 90.625, 0.0001);
    codexbar_provider_free(provider);

    const char *failed_subscription = "{\"success\":false,\"error\":\"temporarily unavailable\"}";
    provider = codexbar_commandcode_parse(
        command_credits(), strlen(command_credits()),
        failed_subscription, strlen(failed_subscription), 1000, &error);
    g_assert_no_error(error);
    json_object *unavailable = NULL;
    g_assert_true(json_object_object_get_ex(
        provider->usage_extensions, "commandCodeSubscriptionEnrichmentUnavailable", &unavailable));
    g_assert_true(json_object_get_boolean(unavailable));
    codexbar_provider_free(provider);

    const char *malformed_subscription = "{\"success\":true,\"data\":{\"status\":\"active\"}}";
    provider = codexbar_commandcode_parse(
        command_credits(), strlen(command_credits()),
        malformed_subscription, strlen(malformed_subscription), 1000, &error);
    g_assert_no_error(error);
    g_assert_true(json_object_object_get_ex(
        provider->usage_extensions, "commandCodeSubscriptionEnrichmentUnavailable", &unavailable));
    g_assert_true(json_object_get_boolean(unavailable));
    codexbar_provider_free(provider);

    const char *depleted_with_purchased =
        "{\"credits\":{\"monthlyCredits\":0,\"purchasedCredits\":5,"
        "\"premiumMonthlyCredits\":0,\"opensourceMonthlyCredits\":0},"
        "\"windowLimits\":{\"fiveHour\":{\"cap\":3,\"used\":1},"
        "\"weekly\":{\"cap\":15,\"used\":3}}}";
    provider = codexbar_commandcode_parse(
        depleted_with_purchased, strlen(depleted_with_purchased),
        command_subscription(), strlen(command_subscription()), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(
        codexbar_provider_quota_window(provider, 0)->used_percent, 100.0 / 3.0, 0.0001);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 1)->used_percent, ==, 20);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 2)->used_percent, ==, 100);
    codexbar_provider_free(provider);

    CodexBarProviderConfig config = config_with_cookie("secret");
    reset_fixture(FIXTURE_COMMAND);
    provider = codexbar_commandcode_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 2);
    codexbar_provider_free(provider);
    clear_config(&config);
}

static void test_qoder_parser_and_transport(void) {
    const char *merged =
        "{\"totalQuota\":{\"quotaSummary\":{\"usedValue\":20,\"limitValue\":100,"
        "\"remainingValue\":80}},\"sharedQuota\":{\"quotaSummary\":{\"usedValue\":5,"
        "\"limitValue\":50,\"remainingValue\":45}},\"nextResetAt\":1788220800}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_qoder_parse(merged, strlen(merged), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(
        codexbar_provider_quota_window(provider, 0)->used_percent, 100.0 / 6.0, 0.0001);
    codexbar_provider_free(provider);

    CodexBarProviderConfig config = config_with_cookie("session=qoder");
    config.region = g_strdup("cn");
    reset_fixture(FIXTURE_QODER);
    provider = codexbar_qoder_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 1);
    codexbar_provider_free(provider);
    clear_config(&config);
}

static void test_perplexity_parser_and_transport(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_perplexity_parse(
        perplexity_credits(), strlen(perplexity_credits()), 1780000000000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->plan, ==, "Max");
    g_assert_cmpuint(provider->quota_windows->len, ==, 3);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 100);
    g_assert_cmpfloat_with_epsilon(
        codexbar_provider_quota_window(provider, 1)->used_percent, 58.06363636363636, 0.0001);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 2)->used_percent, ==, 100);
    g_assert_cmpfloat(codexbar_provider_balance(provider, 0)->remaining, ==, 230.65);
    codexbar_provider_free(provider);

    CodexBarProviderConfig config = config_with_cookie("perplexity-token");
    reset_fixture(FIXTURE_PERPLEXITY);
    provider = codexbar_perplexity_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1780000000000, &error);
    g_assert_no_error(error);
    codexbar_provider_free(provider);
    clear_config(&config);
}

static void test_longcat_parser_and_transport(void) {
    const char *user = "{\"code\":0,\"data\":{\"name\":\"LongCat User\"}}";
    const char *usage =
        "{\"code\":0,\"data\":{\"usage\":{\"totalToken\":500000,"
        "\"usedToken\":120000,\"availableToken\":380000}}}";
    const char *fuel =
        "{\"code\":0,\"data\":{\"totalQuota\":1000,\"list\":["
        "{\"availableToken\":600,\"expireTime\":1790000000000},"
        "{\"availableToken\":150,\"expireTime\":1791000000000}]}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_longcat_parse(
        user, strlen(user), usage, strlen(usage), fuel, strlen(fuel), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->account, ==, "LongCat User");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 24);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 1)->used_percent, ==, 25);
    g_assert_cmpfloat(codexbar_provider_balance(provider, 1)->remaining, ==, 750);
    codexbar_provider_free(provider);

    CodexBarProviderConfig config = config_with_cookie("passport_token=abc; uid=42");
    reset_fixture(FIXTURE_LONGCAT);
    provider = codexbar_longcat_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 3);
    codexbar_provider_free(provider);
    clear_config(&config);
}

static void test_security_and_cancellation(void) {
    GError *error = NULL;
    CodexBarProviderConfig config = config_with_cookie("secret\nInjected: value");
    CodexBarProvider *provider = codexbar_commandcode_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    clear_config(&config);

    config = config_with_cookie("secret");
    reset_fixture(FIXTURE_WRONG_ORIGIN);
    provider = codexbar_commandcode_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    reset_fixture(FIXTURE_COMMAND);
    fixture.cancellable = g_cancellable_new();
    fixture.cancel_in_transport = TRUE;
    provider = codexbar_commandcode_fetch_with_transport_and_cancellable(
        &config, stub_transport, fixture.cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(fixture.cancellable);
    clear_config(&config);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/web4/command/parser-transport", test_command_parser_and_transport);
    g_test_add_func("/web4/qoder/parser-transport", test_qoder_parser_and_transport);
    g_test_add_func("/web4/perplexity/parser-transport", test_perplexity_parser_and_transport);
    g_test_add_func("/web4/longcat/parser-transport", test_longcat_parser_and_transport);
    g_test_add_func("/web4/security-cancellation", test_security_and_cancellation);
    return g_test_run();
}
