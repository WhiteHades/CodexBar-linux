#include "web_providers3.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

typedef enum {
    FIXTURE_NONE,
    FIXTURE_SAKANA,
    FIXTURE_ABACUS,
    FIXTURE_MISTRAL,
    FIXTURE_WRONG_ORIGIN,
} FixtureKind;

typedef struct {
    FixtureKind kind;
    guint count;
    GCancellable *cancellable;
    gboolean cancel_in_transport;
} TransportFixture;

static TransportFixture fixture;

static json_object *object_member(json_object *object, const char *key) {
    json_object *value = NULL;
    return object && json_object_is_type(object, json_type_object) &&
                   json_object_object_get_ex(object, key, &value)
               ? value
               : NULL;
}

static CodexBarHttpResponse *make_response(long status, const char *body, const char *effective_url) {
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = status;
    response->body = g_strdup(body ? body : "");
    response->body_length = strlen(response->body);
    response->headers = g_ptr_array_new();
    response->effective_url = g_strdup(effective_url);
    return response;
}

static const char *request_header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static void assert_policy(const CodexBarHttpRequest *request, long timeout) {
    g_assert_cmpint(request->timeout_seconds, ==, timeout);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == fixture.cancellable);
}

static const char *sakana_billing_html(void) {
    return
        "<main>"
        "<div data-slot=\"card-title\"><span>Standard</span><span>$20/mo</span></div>"
        "<div data-slot=\"card-title\">Usage limit</div>"
        "<p class=\"font-medium\">5-hour</p>"
        "<p>Resets on June 23, 2026 at 2:53 PM</p><p>92% used</p>"
        "<p class=\"font-medium\">Weekly</p>"
        "<p>Resets on June 29, 2026 at 12:00 AM</p><p>32% used</p>"
        "</main>";
}

static const char *sakana_payg_html(void) {
    return
        "<main><h2>Credit balance</h2>"
        "<p class=\"font-semibold text-3xl tabular-nums\">$12.34</p>"
        "<button aria-label=\"Usage date range\">Jun 02, 2026<!-- --> -<!-- --> Jul 01, 2026</button>"
        "<h2>Usage</h2><span>Total<!-- -->: <!-- -->$5.67</span></main>";
}

static const char *mistral_usage_json(void) {
    return
        "{\"completion\":{\"models\":{\"mistral-large\":{"
        "\"input\":[{\"billing_metric\":\"tokens\",\"billing_group\":\"input\","
        "\"billing_display_name\":\"Mistral Large\",\"timestamp\":\"2026-07-14\",\"value\":100}],"
        "\"output\":[{\"billing_metric\":\"tokens\",\"billing_group\":\"output\","
        "\"timestamp\":\"2026-07-14\",\"value_paid\":20}],"
        "\"cached\":[{\"billing_metric\":\"tokens\",\"billing_group\":\"cached\","
        "\"timestamp\":\"2026-07-15\",\"value\":5}]}}},"
        "\"libraries_api\":{\"pages\":{\"models\":{\"ocr\":{\"input\":[{"
        "\"billing_metric\":\"pages\",\"billing_group\":\"input\","
        "\"timestamp\":\"2026-07-14\",\"value\":2}]}}}},"
        "\"currency\":\"EUR\",\"currency_symbol\":\"€\","
        "\"start_date\":\"2026-07-01T00:00:00Z\",\"end_date\":\"2026-07-31T23:59:59Z\","
        "\"prices\":["
        "{\"billing_metric\":\"tokens\",\"billing_group\":\"input\",\"price\":\"0.001\"},"
        "{\"billing_metric\":\"tokens\",\"billing_group\":\"output\",\"price\":\"0.002\"},"
        "{\"billing_metric\":\"tokens\",\"billing_group\":\"cached\",\"price\":\"0.0001\"},"
        "{\"billing_metric\":\"pages\",\"billing_group\":\"input\",\"price\":\"0.01\"}]}";
}

static CodexBarHttpResponse *stub_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    fixture.count++;
    if (fixture.cancel_in_transport && fixture.cancellable) g_cancellable_cancel(fixture.cancellable);
    if (fixture.kind == FIXTURE_WRONG_ORIGIN) {
        return make_response(200, sakana_billing_html(), "https://attacker.example/billing");
    }
    switch (fixture.kind) {
    case FIXTURE_SAKANA:
        g_assert_cmpstr(request_header(request, "Cookie"), ==, "session=abc; theme=dark");
        g_assert_cmpstr(request_header(request, "Accept-Language"), ==, "en-US,en;q=0.9");
        if (fixture.count == 1) {
            assert_policy(request, 15);
            g_assert_cmpstr(request->url, ==, "https://console.sakana.ai/billing");
            return make_response(200, sakana_billing_html(), request->url);
        }
        assert_policy(request, 4);
        g_assert_cmpstr(request->url, ==, "https://console.sakana.ai/billing?tab=payAsYouGo");
        return make_response(200, sakana_payg_html(), request->url);
    case FIXTURE_ABACUS:
        g_assert_cmpstr(request_header(request, "Cookie"), ==, "session=abacus");
        if (fixture.count == 1) {
            assert_policy(request, 15);
            g_assert_cmpstr(request->url, ==, "https://apps.abacus.ai/api/_getOrganizationComputePoints");
            g_assert_cmpstr(request->method, ==, "GET");
            return make_response(200,
                                 "{\"success\":true,\"result\":{"
                                 "\"totalComputePoints\":1000,\"computePointsLeft\":750}}",
                                 request->url);
        }
        assert_policy(request, 4);
        g_assert_cmpstr(request->url, ==, "https://apps.abacus.ai/api/_getBillingInfo");
        g_assert_cmpstr(request->method, ==, "POST");
        g_assert_cmpstr(request->body, ==, "{}");
        return make_response(200,
                             "{\"success\":true,\"result\":{\"currentTier\":\"Pro\","
                             "\"nextBillingDate\":\"2026-08-15T00:00:00Z\"}}",
                             request->url);
    case FIXTURE_MISTRAL:
        if (fixture.count == 1) {
            assert_policy(request, 15);
            g_assert_true(g_str_has_prefix(
                request->url, "https://admin.mistral.ai/api/billing/v2/usage?month="));
            g_assert_cmpstr(request_header(request, "Cookie"), ==,
                            "ory_session_test=abc; csrftoken=csrf; analytics=private");
            g_assert_cmpstr(request_header(request, "X-CSRFTOKEN"), ==, "csrf");
            return make_response(200, mistral_usage_json(), request->url);
        }
        if (fixture.count == 2) {
            assert_policy(request, 4);
            g_assert_cmpstr(request->url, ==,
                            "https://console.mistral.ai/api-ui/trpc/billing.vibeUsage?batch=1&input="
                            "%7B%220%22%3A%7B%22json%22%3Anull%2C%22meta%22%3A%7B%22values%22%3A"
                            "%5B%22undefined%22%5D%2C%22v%22%3A1%7D%7D%7D");
            g_assert_cmpstr(request_header(request, "Cookie"), ==,
                            "csrftoken=csrf; ory_session_test=abc");
            g_assert_null(strstr(request_header(request, "Cookie"), "analytics"));
            return make_response(
                200,
                "[{\"result\":{\"data\":{\"json\":{\"usage_percentage\":37.5,"
                "\"reset_at\":\"2026-08-01T00:00:00Z\"}}}}]",
                request->url);
        }
        assert_policy(request, 4);
        g_assert_cmpstr(request->url, ==, "https://admin.mistral.ai/api/billing/credits");
        g_assert_cmpstr(request_header(request, "Cookie"), ==,
                        "ory_session_test=abc; csrftoken=csrf; analytics=private");
        return make_response(200,
                             "{\"wallet_amount\":12.5,\"credit_notes_amount\":2.25,"
                             "\"ongoing_usage_balance\":1.5,\"currency\":\"EUR\"}",
                             request->url);
    case FIXTURE_NONE:
    case FIXTURE_WRONG_ORIGIN:
        break;
    }
    g_assert_not_reached();
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static CodexBarProviderConfig config_with_cookie(const char *value) {
    CodexBarProviderConfig config = {0};
    config.raw = json_object_new_object();
    json_object_object_add(config.raw, "cookieHeader", json_object_new_string(value));
    return config;
}

static void clear_config(CodexBarProviderConfig *config) {
    if (config->raw) json_object_put(config->raw);
    *config = (CodexBarProviderConfig){0};
}

static void reset_fixture(FixtureKind kind) {
    fixture = (TransportFixture){.kind = kind};
}

static void test_sakana_parsers(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_sakana_parse_billing_html(
        sakana_billing_html(), strlen(sakana_billing_html()), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->plan, ==, "Standard $20/mo");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    CodexBarQuotaWindow *primary = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat(primary->used_percent, ==, 92);
    g_assert_cmpint(primary->window_minutes, ==, 300);
    g_assert_true(primary->has_resets_at);
    g_assert_cmpint(primary->resets_at_ms, ==, G_GINT64_CONSTANT(1782226380000));
    g_assert_true(codexbar_sakana_apply_payg_html(
        provider, sakana_payg_html(), strlen(sakana_payg_html()), &error));
    g_assert_no_error(error);
    g_assert_cmpuint(provider->balances->len, ==, 1);
    g_assert_cmpfloat(codexbar_provider_balance(provider, 0)->remaining, ==, 12.34);
    json_object *payg = NULL;
    g_assert_true(json_object_object_get_ex(provider->usage_extensions, "sakanaPayAsYouGo", &payg));
    g_assert_cmpfloat(json_object_get_double(object_member(payg, "periodUsageTotal")), ==, 5.67);
    codexbar_provider_free(provider);

    const char *invalid = "<p>5-hour</p><p>101% used</p>";
    provider = codexbar_sakana_parse_billing_html(invalid, strlen(invalid), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_sakana_transport_and_security(void) {
    CodexBarProviderConfig config = config_with_cookie(" Cookie: session=abc; theme=dark ");
    reset_fixture(FIXTURE_SAKANA);
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_sakana_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 2);
    codexbar_provider_free(provider);

    reset_fixture(FIXTURE_WRONG_ORIGIN);
    provider = codexbar_sakana_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    clear_config(&config);
    config = config_with_cookie("session=abc\r\nX-Leak: yes");
    provider = codexbar_sakana_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    clear_config(&config);
}

static void test_abacus_parser_and_transport(void) {
    const char *compute =
        "{\"success\":true,\"result\":{\"totalComputePoints\":50000,"
        "\"computePointsLeft\":37654.5}}";
    const char *billing =
        "{\"success\":true,\"result\":{\"currentTier\":\"Pro\","
        "\"nextBillingDate\":\"2026-08-15T00:00:00Z\"}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_abacus_parse_results(
        compute, strlen(compute), billing, strlen(billing), 1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->plan, ==, "Pro");
    g_assert_cmpstr(codexbar_provider_quota_window(provider, 0)->detail, ==,
                    "12,345.5 / 50,000 credits");
    g_assert_cmpfloat_with_epsilon(codexbar_provider_quota_window(provider, 0)->used_percent,
                                   24.691, 0.0001);
    codexbar_provider_free(provider);

    CodexBarProviderConfig config = config_with_cookie("session=abacus");
    reset_fixture(FIXTURE_ABACUS);
    provider = codexbar_abacus_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 2);
    codexbar_provider_free(provider);
    clear_config(&config);

    const char *rejected = "{\"success\":false,\"error\":\"session expired\"}";
    provider = codexbar_abacus_parse_results(
        rejected, strlen(rejected), NULL, 0, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
}

static void test_mistral_parsers(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_mistral_parse_usage(
        mistral_usage_json(), strlen(mistral_usage_json()), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->provider, ==, "mistral");
    g_assert_cmpint(provider->token_cost->last_days_tokens, ==, 125);
    g_assert_cmpfloat_with_epsilon(provider->token_cost->last_days_cost, 0.1605, 0.000001);
    json_object *snapshot = object_member(provider->usage_extensions, "mistralUsage");
    g_assert_nonnull(snapshot);
    g_assert_cmpint(json_object_get_int64(object_member(snapshot, "modelCount")), ==, 1);
    g_assert_cmpuint(json_object_array_length(object_member(snapshot, "daily")), ==, 2);
    const char *credits =
        "{\"wallet_amount\":12.5,\"credit_notes_amount\":2.25,"
        "\"ongoing_usage_balance\":1.5,\"currency\":\"USD\"}";
    g_assert_true(codexbar_mistral_apply_credits(provider, credits, strlen(credits), &error));
    g_assert_no_error(error);
    g_assert_cmpfloat(codexbar_provider_balance(provider, 0)->remaining, ==, 13.25);
    const char *vibe =
        "[{\"result\":{\"data\":{\"json\":{\"usage_percentage\":37,"
        "\"reset_at\":\"2026-08-01T00:00:00Z\"}}}}]";
    g_assert_true(codexbar_mistral_apply_vibe_usage(provider, vibe, strlen(vibe), &error));
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 37);
    codexbar_provider_free(provider);

    provider = codexbar_mistral_parse_usage("{} trailing", strlen("{} trailing"), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    const char *overflow =
        "{\"completion\":{\"models\":{\"m\":{\"input\":[{\"value\":1e30}]}}}}";
    provider = codexbar_mistral_parse_usage(overflow, strlen(overflow), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_mistral_credentials_transport_and_cancellation(void) {
    GError *error = NULL;
    char *console = codexbar_mistral_console_cookie_header(
        "ory_session_a=one; csrftoken=csrf; analytics=secret; ory_session_b=two", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(console, ==, "csrftoken=csrf; ory_session_a=one; ory_session_b=two");
    g_free(console);
    console = codexbar_mistral_console_cookie_header(
        "ory_session_a=one; csrftoken=csrf,leak", &error);
    g_assert_null(console);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    CodexBarProviderConfig config = config_with_cookie(
        "ory_session_test=abc; csrftoken=csrf; analytics=private");
    g_assert_true(codexbar_mistral_has_auth(&config));
    reset_fixture(FIXTURE_MISTRAL);
    CodexBarProvider *provider = codexbar_mistral_fetch_with_transport(&config, stub_transport, 1785542400000, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 3);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    g_assert_cmpuint(provider->balances->len, ==, 1);
    codexbar_provider_free(provider);

    reset_fixture(FIXTURE_MISTRAL);
    fixture.cancellable = g_cancellable_new();
    fixture.cancel_in_transport = TRUE;
    provider = codexbar_mistral_fetch_with_transport_and_cancellable(
        &config, stub_transport, fixture.cancellable, 1785542400000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(fixture.cancellable);
    clear_config(&config);

    config = config_with_cookie("csrftoken=csrf; unrelated=value");
    g_assert_false(codexbar_mistral_has_auth(&config));
    provider = codexbar_mistral_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    clear_config(&config);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/web3/sakana/parsers", test_sakana_parsers);
    g_test_add_func("/web3/sakana/transport-security", test_sakana_transport_and_security);
    g_test_add_func("/web3/abacus/parser-transport", test_abacus_parser_and_transport);
    g_test_add_func("/web3/mistral/parsers", test_mistral_parsers);
    g_test_add_func("/web3/mistral/credentials-transport-cancellation",
                    test_mistral_credentials_transport_and_cancellation);
    return g_test_run();
}
