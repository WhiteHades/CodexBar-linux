#include "web_providers2.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

typedef enum {
    FIXTURE_NONE,
    FIXTURE_MANUS,
    FIXTURE_AMP_API,
    FIXTURE_AMP_WEB,
    FIXTURE_T3CHAT,
    FIXTURE_T3CHAT_CHALLENGE,
} FixtureKind;

typedef struct {
    FixtureKind kind;
    guint count;
    GCancellable *cancellable;
    gboolean cancel_in_transport;
} TransportFixture;

static TransportFixture fixture;

static void response_header_free(gpointer data) {
    CodexBarHttpResponseHeader *header = data;
    g_free(header->name);
    g_free(header->value);
    g_free(header);
}

static void add_response_header(CodexBarHttpResponse *response, const char *name, const char *value) {
    CodexBarHttpResponseHeader *header = g_new0(CodexBarHttpResponseHeader, 1);
    header->name = g_strdup(name);
    header->value = g_strdup(value);
    g_ptr_array_add(response->headers, header);
}

static CodexBarHttpResponse *make_response(long status, const char *body) {
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = status;
    response->body = g_strdup(body ? body : "");
    response->body_length = strlen(response->body);
    response->headers = g_ptr_array_new_with_free_func(response_header_free);
    return response;
}

static const char *request_header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static void assert_request_policy(const CodexBarHttpRequest *request,
                                  CodexBarHttpRedirectPolicy redirect_policy) {
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, redirect_policy);
    g_assert_true(request->cancellable == fixture.cancellable);
}

static CodexBarHttpResponse *stub_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    fixture.count++;
    if (fixture.cancel_in_transport && fixture.cancellable) g_cancellable_cancel(fixture.cancellable);
    switch (fixture.kind) {
    case FIXTURE_MANUS:
        assert_request_policy(request, CODEXBAR_HTTP_REDIRECT_DENY);
        g_assert_cmpstr(request->url,
                        ==,
                        "https://api.manus.im/user.v1.UserService/GetAvailableCredits");
        g_assert_cmpstr(request->method, ==, "POST");
        g_assert_cmpstr(request_header(request, "Authorization"), ==, "Bearer manus-token");
        g_assert_cmpstr(request_header(request, "Connect-Protocol-Version"), ==, "1");
        g_assert_cmpstr(request->body, ==, "{}");
        return make_response(200, "{\"totalCredits\":100,\"proMonthlyCredits\":200,\"periodicCredits\":50}");
    case FIXTURE_AMP_API:
        assert_request_policy(request, CODEXBAR_HTTP_REDIRECT_DENY);
        g_assert_cmpstr(request->url, ==, "https://ampcode.com/api/internal?userDisplayBalanceInfo");
        g_assert_cmpstr(request->method, ==, "POST");
        g_assert_cmpstr(request_header(request, "Authorization"), ==, "Bearer sgamp-test");
        g_assert_null(request_header(request, "Cookie"));
        g_assert_cmpstr(request->body, ==, "{\"method\":\"userDisplayBalanceInfo\",\"params\":{}}");
        return make_response(
            200,
            "{\"ok\":true,\"result\":{\"displayText\":\"Amp Free: 75% remaining today (resets daily)\"}}");
    case FIXTURE_AMP_WEB:
        assert_request_policy(request, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
        g_assert_cmpstr(request->url, ==, "https://ampcode.com/settings");
        g_assert_cmpstr(request->method, ==, "GET");
        g_assert_cmpstr(request_header(request, "Cookie"), ==, "session=amp-session");
        return make_response(200,
                             "<script>freeTierUsage = {quota: 100, used: 25, "
                             "hourlyReplenishment: 5, windowHours: 20}</script>");
    case FIXTURE_T3CHAT:
        assert_request_policy(request, CODEXBAR_HTTP_REDIRECT_DENY);
        g_assert_true(g_str_has_prefix(request->url,
                                       "https://t3.chat/api/trpc/getCustomerData?batch=1&input="));
        g_assert_cmpstr(request->method, ==, "GET");
        g_assert_cmpstr(request_header(request, "Cookie"), ==, "session=abc; cf_clearance=token");
        g_assert_cmpstr(request_header(request, "Origin"), ==, "https://t3.chat");
        g_assert_cmpstr(request_header(request, "User-Agent"), ==, "Browser's Agent");
        g_assert_cmpstr(request_header(request, "Referer"), ==, "https://t3.chat/settings/customization");
        g_assert_cmpstr(request_header(request, "X-Deployment-Id"), ==, "dpl_test");
        g_assert_cmpstr(request_header(request, "trpc-accept"), ==, "application/jsonl");
        g_assert_null(request_header(request, "Authorization"));
        g_assert_null(request_header(request, "Evil"));
        return make_response(
            200,
            "{\"json\":[2,0,[[{\"subTier\":\"pro\",\"usageBand\":\"max\","
            "\"usageFourHourPercentage\":12.5,\"usageMonthPercentage\":34.25}]]]}\n");
    case FIXTURE_T3CHAT_CHALLENGE: {
        assert_request_policy(request, CODEXBAR_HTTP_REDIRECT_DENY);
        CodexBarHttpResponse *response = make_response(429, "checkpoint");
        add_response_header(response, "x-vercel-mitigated", "challenge");
        return response;
    }
    case FIXTURE_NONE:
        break;
    }
    g_assert_not_reached();
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static void reset_fixture(FixtureKind kind) {
    fixture = (TransportFixture){.kind = kind};
}

static CodexBarProviderConfig config_with_raw_string(const char *key, const char *value) {
    CodexBarProviderConfig config = {0};
    config.raw = json_object_new_object();
    json_object_object_add(config.raw, key, json_object_new_string(value));
    return config;
}

static void clear_config(CodexBarProviderConfig *config) {
    if (config->raw) json_object_put(config->raw);
    *config = (CodexBarProviderConfig){0};
}

static void assert_invalid(CodexBarProvider *(*parser)(const char *, size_t, gint64, GError **),
                           const char *body) {
    GError *error = NULL;
    CodexBarProvider *provider = parser(body, strlen(body), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_manus_credentials_and_parser(void) {
    char *token = codexbar_manus_extract_session_token("foo=bar; SESSION_ID=abc; baz=qux");
    g_assert_cmpstr(token, ==, "abc");
    g_free(token);
    token = codexbar_manus_extract_session_token(" plain-token ");
    g_assert_cmpstr(token, ==, "plain-token");
    g_free(token);
    g_assert_null(codexbar_manus_extract_session_token("session_id=bad\r\nInjected: yes"));
    char *oversized = g_malloc0(16386);
    memset(oversized, 'a', 16385);
    g_assert_null(codexbar_manus_extract_session_token(oversized));
    g_free(oversized);

    const char *wrapped =
        "{\"data\":{\"totalCredits\":\"2869\",\"freeCredits\":1500,"
        "\"periodicCredits\":1369,\"proMonthlyCredits\":4000,\"maxRefreshCredits\":300,"
        "\"refreshCredits\":0,\"nextRefreshTime\":\"2026-08-02T00:00:00Z\","
        "\"refreshInterval\":\"daily\"}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_manus_parse_credits(wrapped, strlen(wrapped), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->provider, ==, "manus");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    g_assert_cmpfloat_with_epsilon(codexbar_provider_quota_window(provider, 0)->used_percent, 65.775, 0.001);
    g_assert_cmpstr(codexbar_provider_quota_window(provider, 0)->detail, ==, "Total 2869 • Free 1500");
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 1)->used_percent, ==, 100);
    g_assert_true(codexbar_provider_quota_window(provider, 1)->has_resets_at);
    g_assert_cmpstr(provider->identity->login_method, ==, "Balance: 2869 credits");
    codexbar_provider_free(provider);

    assert_invalid(codexbar_manus_parse_credits, "{\"error\":\"unauthorized\"}");
    assert_invalid(codexbar_manus_parse_credits, "{\"totalCredits\":1} trailing");
    oversized = g_malloc0(1024U * 1024U + 2U);
    memset(oversized, ' ', 1024U * 1024U + 1U);
    provider = codexbar_manus_parse_credits(oversized, 1024U * 1024U + 1U, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    g_free(oversized);
}

static void test_manus_transport_and_cancellation(void) {
    CodexBarProviderConfig config = config_with_raw_string("cookieHeader", "session_id=manus-token");
    g_assert_true(codexbar_manus_has_auth(&config));
    reset_fixture(FIXTURE_MANUS);
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_manus_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 1);
    codexbar_provider_free(provider);

    reset_fixture(FIXTURE_MANUS);
    fixture.cancellable = g_cancellable_new();
    fixture.cancel_in_transport = TRUE;
    provider = codexbar_manus_fetch_with_transport_and_cancellable(
        &config, stub_transport, fixture.cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(fixture.cancellable);
    clear_config(&config);
}

static void test_amp_parsers(void) {
    const char *api =
        "{\"ok\":true,\"result\":{\"displayText\":"
        "\"Signed in as user@example.com (Acme)\\n"
        "Subscription Pro: 80% other usage and 60% orb usage remaining - resets upon renewal in 2 days\\n"
        "Individual credits: $12.50 remaining\\nWorkspace Team: $7 remaining\"}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_amp_parse_usage_api(api, strlen(api), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->account, ==, "user@example.com");
    g_assert_cmpstr(provider->identity->organization, ==, "Acme");
    g_assert_cmpstr(provider->plan, ==, "Pro");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 20);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 1)->used_percent, ==, 40);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 0)->window_minutes, ==, 43200);
    g_assert_cmpuint(provider->balances->len, ==, 2);
    codexbar_provider_free(provider);

    const char *html =
        "<script>getFreeTierUsage = {quota:100, used:25, hourlyReplenishment:5, windowHours:20}</script>";
    provider = codexbar_amp_parse_settings_html(html, strlen(html), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->source, ==, "web");
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 25);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 0)->window_minutes, ==, 1200);
    g_assert_true(codexbar_provider_quota_window(provider, 0)->has_resets_at);
    codexbar_provider_free(provider);

    assert_invalid(codexbar_amp_parse_usage_api, "{\"ok\":true,\"result\":{}} trailing");
    assert_invalid(codexbar_amp_parse_settings_html, "<html>settings</html>");
    const char *auth_error = "{\"ok\":false,\"error\":{\"code\":\"auth-required\"}}";
    provider = codexbar_amp_parse_usage_api(auth_error, strlen(auth_error), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
}

static void test_amp_transport(void) {
    CodexBarProviderConfig config = {.api_key = " sgamp-test ", .source = "api"};
    reset_fixture(FIXTURE_AMP_API);
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_amp_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 25);
    codexbar_provider_free(provider);

    config = config_with_raw_string("cookieHeader", "other=drop; session=amp-session");
    config.source = "web";
    reset_fixture(FIXTURE_AMP_WEB);
    provider = codexbar_amp_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->source, ==, "web");
    codexbar_provider_free(provider);
    clear_config(&config);

    config.api_key = "bad\nkey";
    config.source = "api";
    provider = codexbar_amp_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
}

static const char *t3chat_sample(void) {
    return
        "{\"json\":{\"0\":[[0],[null,0,0]]}}\n"
        "{\"json\":[2,0,[[{\"subTier\":\"pro\",\"subscription\":{\"productName\":\"pro\","
        "\"currentPeriodEnd\":1780763009},\"usageBand\":\"max\","
        "\"billingNextResetAt\":1779366216920,\"usageFourHourPercentage\":12.5,"
        "\"usageMonthPercentage\":34.25,\"usageFourHourNextResetAt\":1779366216920}]]]}\n";
}

static void test_t3chat_parser(void) {
    GError *error = NULL;
    const char *sample = t3chat_sample();
    CodexBarProvider *provider = codexbar_t3chat_parse_json_lines(sample, strlen(sample), 1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->provider, ==, "t3chat");
    g_assert_cmpstr(provider->plan, ==, "Pro");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    CodexBarQuotaWindow *primary = codexbar_provider_quota_window(provider, 0);
    CodexBarQuotaWindow *secondary = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpfloat(primary->used_percent, ==, 12.5);
    g_assert_cmpint(primary->window_minutes, ==, 240);
    g_assert_cmpstr(primary->detail, ==, "Base - max");
    g_assert_true(primary->has_resets_at);
    g_assert_cmpfloat(secondary->used_percent, ==, 34.25);
    g_assert_true(secondary->has_resets_at);
    g_assert_cmpint(secondary->resets_at_ms, ==, 1780763009000);
    codexbar_provider_free(provider);

    const char *fallback = "{\"usageFourHourPercentage\":5,\"usagePeriodPercentage\":65}";
    provider = codexbar_t3chat_parse_json_lines(fallback, strlen(fallback), 1, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 1)->used_percent, ==, 65);
    g_assert_false(codexbar_provider_quota_window(provider, 1)->has_resets_at);
    codexbar_provider_free(provider);
    assert_invalid(codexbar_t3chat_parse_json_lines, "{\"usageFourHourPercentage\":5} trailing");
}

static void test_t3chat_capture_transport_and_challenge(void) {
    const char *capture =
        "curl 'https://t3.chat/api/trpc/getCustomerData?batch=1&input=ignored' \\\n"
        "  --header=$'User-Agent: Browser\\'s Agent' \\\n"
        "  -H 'Referer: https://t3.chat/settings/customization' \\\n"
        "  --header 'X-Deployment-Id: dpl_test' \\\n"
        "  -H 'Origin: https://evil.example' \\\n"
        "  -H 'Authorization: should-not-forward' \\\n"
        "  -H 'Cookie: session=abc; cf_clearance=token'";
    CodexBarProviderConfig config = config_with_raw_string("cookieHeader", capture);
    g_assert_true(codexbar_t3chat_has_auth(&config));
    reset_fixture(FIXTURE_T3CHAT);
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_t3chat_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 1);
    codexbar_provider_free(provider);

    reset_fixture(FIXTURE_T3CHAT_CHALLENGE);
    provider = codexbar_t3chat_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_nonnull(strstr(error->message, "Vercel challenge"));
    g_clear_error(&error);
    clear_config(&config);

    config = config_with_raw_string("cookieHeader", "Cookie: session=abc\r\nAuthorization: injected");
    g_assert_false(codexbar_t3chat_has_auth(&config));
    provider = codexbar_t3chat_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    clear_config(&config);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/web2/manus/credentials-parser", test_manus_credentials_and_parser);
    g_test_add_func("/web2/manus/transport-cancellation", test_manus_transport_and_cancellation);
    g_test_add_func("/web2/amp/parsers", test_amp_parsers);
    g_test_add_func("/web2/amp/transport", test_amp_transport);
    g_test_add_func("/web2/t3chat/parser", test_t3chat_parser);
    g_test_add_func("/web2/t3chat/transport-challenge", test_t3chat_capture_transport_and_challenge);
    return g_test_run();
}
