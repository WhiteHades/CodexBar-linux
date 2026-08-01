#include "api_providers4.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

typedef enum {
    FIXTURE_NONE,
    FIXTURE_FACTORY_LIMITS,
    FIXTURE_FACTORY_UNAUTHORIZED,
    FIXTURE_GEMINI,
    FIXTURE_GEMINI_UNAUTHORIZED,
    FIXTURE_OLLAMA,
    FIXTURE_OLLAMA_LOOPBACK,
    FIXTURE_OLLAMA_UNAUTHORIZED,
    FIXTURE_OLLAMA_WEB,
} FixtureKind;

typedef struct {
    FixtureKind kind;
    guint count;
    GCancellable *cancellable;
    gboolean cancel_after_first;
} TransportFixture;

static TransportFixture fixture;

static const char *request_header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static CodexBarHttpResponse *make_response(long status, const char *body) {
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = status;
    response->body = g_strdup(body ? body : "");
    response->body_length = strlen(response->body);
    response->headers = g_ptr_array_new();
    return response;
}

static void assert_common_request(const CodexBarHttpRequest *request,
                                  long timeout,
                                  CodexBarHttpProtocolPolicy protocol) {
    g_assert_cmpint(request->timeout_seconds, ==, timeout);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, protocol);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_DENY);
    g_assert_true(request->cancellable == fixture.cancellable);
    g_assert_cmpstr(request_header(request, "Accept"), ==, "application/json");
}

static CodexBarHttpResponse *stub_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    guint index = fixture.count++;
    switch (fixture.kind) {
    case FIXTURE_FACTORY_LIMITS:
        assert_common_request(request, 15, CODEXBAR_HTTP_HTTPS_ONLY);
        g_assert_cmpstr(request_header(request, "Authorization"), ==, "Bearer fk-test");
        g_assert_cmpstr(request_header(request, "Origin"), ==, "https://app.factory.ai");
        g_assert_cmpstr(request_header(request, "Referer"), ==, "https://app.factory.ai/");
        g_assert_cmpstr(request_header(request, "x-factory-client"), ==, "web-app");
        if (index == 0) {
            g_assert_cmpstr(request->url, ==, "https://api.factory.ai/api/app/auth/me");
            return make_response(
                200,
                "{\"organization\":{\"name\":\"Acme\",\"subscription\":{\"factoryTier\":\"team\","
                "\"orbSubscription\":{\"plan\":{\"name\":\"Team\"}}}},\"userProfile\":{\"id\":\"u 1\"}}");
        }
        g_assert_cmpuint(index, ==, 1);
        g_assert_cmpstr(request->url, ==, "https://api.factory.ai/api/billing/limits");
        return make_response(
            200,
            "{\"usesTokenRateLimitsBilling\":true,\"limits\":{\"standard\":{"
            "\"fiveHour\":{\"usedPercent\":12,\"secondsRemaining\":60},"
            "\"weekly\":{\"usedPercent\":34,\"secondsRemaining\":120},"
            "\"monthly\":{\"usedPercent\":56,\"secondsRemaining\":180}}},"
            "\"extraUsageBalanceCents\":725}");
    case FIXTURE_FACTORY_UNAUTHORIZED:
        assert_common_request(request, 15, CODEXBAR_HTTP_HTTPS_ONLY);
        g_assert_cmpstr(request_header(request, "Authorization"), ==, "Bearer fk-bad");
        if (index == 0) {
            g_assert_cmpstr(request->url, ==, "https://api.factory.ai/api/app/auth/me");
            return make_response(401, "{}");
        }
        g_assert_cmpuint(index, ==, 1);
        g_assert_cmpstr(request->url, ==, "https://app.factory.ai/api/app/auth/me");
        return make_response(404, "{}");
    case FIXTURE_GEMINI:
        assert_common_request(request, 10, CODEXBAR_HTTP_HTTPS_ONLY);
        g_assert_cmpstr(request_header(request, "Authorization"), ==, "Bearer oauth-token");
        g_assert_cmpstr(request_header(request, "Content-Type"), ==, "application/json");
        if (index == 0) {
            g_assert_cmpstr(request->url,
                            ==,
                            "https://cloudcode-pa.googleapis.com/v1internal:loadCodeAssist");
            g_assert_cmpstr(request->method, ==, "POST");
            g_assert_nonnull(strstr(request->body, "GEMINI_CLI"));
            return make_response(
                200,
                "{\"currentTier\":{\"id\":\"standard-tier\"},"
                "\"cloudaicompanionProject\":{\"id\":\"managed-project\"}}");
        }
        g_assert_cmpuint(index, ==, 1);
        g_assert_cmpstr(request->url,
                        ==,
                        "https://cloudcode-pa.googleapis.com/v1internal:retrieveUserQuota");
        g_assert_cmpstr(request->method, ==, "POST");
        g_assert_nonnull(strstr(request->body, "managed-project"));
        if (fixture.cancel_after_first) g_cancellable_cancel(fixture.cancellable);
        return make_response(
            200,
            "{\"buckets\":[{\"modelId\":\"gemini-2.5-pro\",\"remainingFraction\":0.75,"
            "\"resetTime\":\"2026-08-02T00:00:00Z\"}]}");
    case FIXTURE_GEMINI_UNAUTHORIZED:
        assert_common_request(request, 10, CODEXBAR_HTTP_HTTPS_ONLY);
        if (index == 0) return make_response(404, "{}");
        if (index == 1) {
            g_assert_cmpstr(request->url, ==, "https://cloudresourcemanager.googleapis.com/v1/projects");
            return make_response(500, "{}");
        }
        g_assert_cmpuint(index, ==, 2);
        return make_response(401, "{}");
    case FIXTURE_OLLAMA:
    case FIXTURE_OLLAMA_LOOPBACK:
    case FIXTURE_OLLAMA_UNAUTHORIZED: {
        CodexBarHttpProtocolPolicy protocol = fixture.kind == FIXTURE_OLLAMA_LOOPBACK
                                                  ? CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP
                                                  : CODEXBAR_HTTP_HTTPS_ONLY;
        assert_common_request(request, 20, protocol);
        g_assert_cmpstr(request_header(request, "Authorization"), ==, "Bearer ollama-test");
        g_assert_cmpstr(request_header(request, "User-Agent"), ==, "CodexBar/1.0");
        const char *base = fixture.kind == FIXTURE_OLLAMA_LOOPBACK ? "http://127.0.0.1:11434" : "https://ollama.com";
        if (index == 0) {
            char *expected = g_strdup_printf("%s/api/web_search", base);
            g_assert_cmpstr(request->url, ==, expected);
            g_free(expected);
            g_assert_cmpstr(request->method, ==, "POST");
            g_assert_cmpstr(request->body, ==, "{\"query\":\"\"}");
            return make_response(fixture.kind == FIXTURE_OLLAMA_UNAUTHORIZED ? 401 : 400, "{}");
        }
        g_assert_cmpuint(index, ==, 1);
        char *expected = g_strdup_printf("%s/api/tags", base);
        g_assert_cmpstr(request->url, ==, expected);
        g_free(expected);
        g_assert_cmpstr(request->method, ==, "GET");
        g_assert_null(request->body);
        return make_response(200, "{\"models\":[{\"name\":\"gemma3\"},{\"name\":\"qwen3\"}]}");
    }
    case FIXTURE_OLLAMA_WEB:
        g_assert_cmpuint(index, ==, 0);
        g_assert_cmpstr(request->url, ==, "https://ollama.com/settings");
        g_assert_cmpstr(request->method, ==, "GET");
        g_assert_cmpstr(request_header(request, "Cookie"), ==, "__Secure-session=test-session");
        g_assert_cmpstr(request_header(request, "Origin"), ==, "https://ollama.com");
        g_assert_cmpstr(request_header(request, "Referer"), ==, "https://ollama.com/settings");
        g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
        return make_response(
            200,
            "<span>Cloud Usage</span><span class=\"plan\">pro</span>"
            "<h2 id=\"header-email\">user@example.com</h2>"
            "<span>Session usage</span><span>12.5% used</span>"
            "<div data-time=\"2026-08-02T01:02:03Z\"></div>"
            "<span>Weekly usage</span><span style=\"width: 34%\"></span>"
            "<div data-time=\"2026-08-08T00:00:00Z\"></div>");
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

static void test_factory_parsers(void) {
    const char *auth =
        "{\"organization\":{\"name\":\"Acme\",\"subscription\":{\"factoryTier\":\"enterprise\","
        "\"orbSubscription\":{\"plan\":{\"name\":\"Pro\"}}}}}";
    const char *usage =
        "{\"usage\":{\"endDate\":1785628800000,"
        "\"standard\":{\"userTokens\":50,\"totalAllowance\":100,\"usedRatio\":0},"
        "\"premium\":{\"userTokens\":10,\"totalAllowance\":0,\"usedRatio\":0.1}}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_factory_parse_legacy_usage(
        auth, strlen(auth), usage, strlen(usage), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->provider, ==, "factory");
    g_assert_cmpstr(provider->identity->organization, ==, "Acme");
    g_assert_cmpstr(provider->identity->login_method, ==, "Factory Enterprise - Pro");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 50);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 1)->used_percent, ==, 10);
    g_assert_true(codexbar_provider_quota_window(provider, 0)->has_resets_at);
    codexbar_provider_free(provider);

    const char *limits =
        "{\"usesTokenRateLimitsBilling\":true,\"limits\":{\"standard\":{"
        "\"fiveHour\":{\"usedPercent\":99,\"windowEnd\":500},"
        "\"weekly\":{\"usedPercent\":40,\"secondsRemaining\":60},"
        "\"monthly\":{\"usedPercent\":60,\"windowEnd\":\"2026-08-02T00:00:00Z\"}},"
        "\"core\":{\"fiveHour\":{\"usedPercent\":5,\"secondsRemaining\":10},"
        "\"weekly\":{\"usedPercent\":6,\"secondsRemaining\":20},"
        "\"monthly\":{\"usedPercent\":7,\"secondsRemaining\":30}}},"
        "\"extraUsageBalanceCents\":250}";
    provider = codexbar_factory_parse_billing_limits(
        auth, strlen(auth), limits, strlen(limits), 1000000, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 6);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 0);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 1)->used_percent, ==, 40);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 1)->window_minutes, ==, 10080);
    g_assert_nonnull(provider->provider_cost);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 2.5);
    codexbar_provider_free(provider);

    provider = codexbar_factory_parse_legacy_usage(
        auth, strlen(auth), "{} trailing", strlen("{} trailing"), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_factory_transport_and_credentials(void) {
    const char *previous_home = g_getenv("HOME");
    char *saved_home = g_strdup(previous_home);
    g_setenv("HOME", "/codexbar-api4-no-home", TRUE);
    g_unsetenv("FACTORY_API_KEY");
    CodexBarProviderConfig config = {.api_key = " fk-test "};
    g_assert_true(codexbar_factory_has_api_key(&config));
    reset_fixture(FIXTURE_FACTORY_LIMITS);
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_factory_fetch_with_transport(&config, stub_transport, 1000000, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 2);
    g_assert_cmpuint(provider->quota_windows->len, ==, 3);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 7.25);
    codexbar_provider_free(provider);

    config.api_key = "fk-bad";
    reset_fixture(FIXTURE_FACTORY_UNAUTHORIZED);
    provider = codexbar_factory_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    config.api_key = "bad\nkey";
    provider = codexbar_factory_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    if (saved_home) g_setenv("HOME", saved_home, TRUE);
    else g_unsetenv("HOME");
    g_free(saved_home);
}

static char *make_id_token(void) {
    static const char claims[] = "{\"email\":\"user@example.com\",\"hd\":\"example.com\"}";
    char *encoded = g_base64_encode((const guchar *)claims, strlen(claims));
    for (char *cursor = encoded; *cursor; cursor++) {
        if (*cursor == '+') *cursor = '-';
        else if (*cursor == '/') *cursor = '_';
    }
    char *padding = strchr(encoded, '=');
    if (padding) *padding = '\0';
    char *token = g_strdup_printf("header.%s.signature", encoded);
    g_free(encoded);
    return token;
}

static void test_gemini_parser(void) {
    const char *quota =
        "{\"buckets\":["
        "{\"modelId\":\"gemini-2.5-pro\",\"remainingFraction\":0.8,\"resetTime\":\"2026-08-02T00:00:00Z\"},"
        "{\"modelId\":\"gemini-3-pro\",\"remainingFraction\":0.4,\"resetTime\":\"2026-08-03T00:00:00Z\"},"
        "{\"modelId\":\"gemini-2.5-flash\",\"remainingFraction\":0.7},"
        "{\"modelId\":\"gemini-2.5-flash-lite\",\"remainingFraction\":0.9}]}";
    const char *assist =
        "{\"currentTier\":{\"id\":\"free-tier\"},\"paidTier\":{\"name\":\"Google AI Pro\"}}";
    char *id_token = make_id_token();
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_gemini_parse_quota(
        quota, strlen(quota), id_token, assist, strlen(assist), 1234, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->account, ==, "user@example.com");
    g_assert_cmpstr(provider->plan, ==, "Google AI Pro");
    g_assert_cmpuint(provider->quota_windows->len, ==, 3);
    g_assert_cmpfloat_with_epsilon(codexbar_provider_quota_window(provider, 0)->used_percent, 60, 0.0001);
    g_assert_cmpstr(codexbar_provider_quota_window(provider, 0)->detail, ==, "gemini-3-pro");
    g_assert_cmpfloat_with_epsilon(codexbar_provider_quota_window(provider, 1)->used_percent, 30, 0.0001);
    g_assert_cmpfloat_with_epsilon(codexbar_provider_quota_window(provider, 2)->used_percent, 10, 0.0001);
    codexbar_provider_free(provider);
    g_free(id_token);

    provider = codexbar_gemini_parse_quota("{} trailing", 11, NULL, NULL, 0, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_gemini_transport_unauthorized_and_cancellation(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_gemini_fetch_access_token_with_transport_and_cancellable(
        "bad\ntoken", NULL, unexpected_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    reset_fixture(FIXTURE_GEMINI);
    provider = codexbar_gemini_fetch_access_token_with_transport_and_cancellable(
        "oauth-token", NULL, stub_transport, NULL, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 2);
    g_assert_cmpstr(provider->plan, ==, "Paid");
    codexbar_provider_free(provider);

    reset_fixture(FIXTURE_GEMINI_UNAUTHORIZED);
    provider = codexbar_gemini_fetch_access_token_with_transport_and_cancellable(
        "oauth-token", NULL, stub_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    reset_fixture(FIXTURE_GEMINI);
    fixture.cancellable = g_cancellable_new();
    fixture.cancel_after_first = TRUE;
    provider = codexbar_gemini_fetch_access_token_with_transport_and_cancellable(
        "oauth-token", NULL, stub_transport, fixture.cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_clear_object(&fixture.cancellable);
}

static void test_ollama_parser_transport_and_security(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_ollama_parse_api_tags("{\"models\":[]}", 13, 5, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->identity->login_method, ==, "API key");
    g_assert_cmpuint(provider->quota_windows->len, ==, 0);
    codexbar_provider_free(provider);

    provider = codexbar_ollama_parse_api_tags("{\"models\":{}}", 13, 5, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);

    CodexBarProviderConfig config = {.api_key = "ollama-test"};
    reset_fixture(FIXTURE_OLLAMA);
    provider = codexbar_ollama_fetch_with_transport(&config, stub_transport, 7, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 2);
    g_assert_cmpuint(provider->quota_windows->len, ==, 0);
    codexbar_provider_free(provider);

    reset_fixture(FIXTURE_OLLAMA_UNAUTHORIZED);
    provider = codexbar_ollama_fetch_with_transport(&config, stub_transport, 7, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    reset_fixture(FIXTURE_NONE);
    provider = codexbar_ollama_fetch_endpoints_with_transport_and_cancellable(
        &config, "http://example.com/api/tags", "http://example.com/api/web_search", unexpected_transport,
        NULL, 1, &error);
    g_assert_null(provider);
    g_assert_nonnull(error);
    g_clear_error(&error);
    provider = codexbar_ollama_fetch_endpoints_with_transport_and_cancellable(
        &config, "https://ollama.com/api/tags", "https://evil.example/api/web_search", unexpected_transport,
        NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);

    reset_fixture(FIXTURE_OLLAMA_LOOPBACK);
    provider = codexbar_ollama_fetch_endpoints_with_transport_and_cancellable(
        &config, "http://127.0.0.1:11434/api/tags", "http://127.0.0.1:11434/api/web_search", stub_transport,
        NULL, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 2);
    codexbar_provider_free(provider);
}

static void test_ollama_web_parser_and_source_routing(void) {
    const char *html =
        "<span>Cloud Usage</span><span>free</span>"
        "<h2 id=\"header-email\">user@example.com</h2>"
        "<span>Hourly usage</span><span>2.5% Used</span>"
        "<div data-time=\"2026-08-02T01:02:03Z\"></div>"
        "<span>Weekly usage</span><span style=\"width: 4.2%\"></span>"
        "<div data-time=\"2026-08-08T00:00:00.123Z\"></div>";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_ollama_parse_settings_html(html, strlen(html), 7, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->plan, ==, "free");
    g_assert_cmpstr(provider->account, ==, "user@example.com");
    g_assert_cmpstr(provider->identity->login_method, ==, "free");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    CodexBarQuotaWindow *hourly = g_ptr_array_index(provider->quota_windows, 0);
    CodexBarQuotaWindow *weekly = g_ptr_array_index(provider->quota_windows, 1);
    g_assert_cmpfloat_with_epsilon(hourly->used_percent, 2.5, 0.001);
    g_assert_false(hourly->has_window_minutes);
    g_assert_true(hourly->has_resets_at);
    g_assert_cmpfloat_with_epsilon(weekly->used_percent, 4.2, 0.001);
    g_assert_cmpint(weekly->window_minutes, ==, 10080);
    codexbar_provider_free(provider);

    const char *signed_out =
        "<h1>Sign in to Ollama</h1><form action=\"/signin\"><input type=\"email\"><input type=\"password\"></form>";
    provider = codexbar_ollama_parse_settings_html(signed_out, strlen(signed_out), 7, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    json_object *raw = json_object_new_object();
    json_object_object_add(raw, "cookieHeader", json_object_new_string("__Secure-session=test-session"));
    CodexBarProviderConfig config = {.api_key = "ollama-test", .raw = raw};
    reset_fixture(FIXTURE_OLLAMA_WEB);
    provider = codexbar_ollama_fetch_for_source_with_transport_and_cancellable(
        &config, "web", stub_transport, NULL, 7, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->source, ==, "web");
    g_assert_cmpfloat_with_epsilon(
        ((CodexBarQuotaWindow *)g_ptr_array_index(provider->quota_windows, 0))->used_percent, 12.5, 0.001);
    codexbar_provider_free(provider);

    reset_fixture(FIXTURE_OLLAMA_WEB);
    provider = codexbar_ollama_fetch_for_source_with_transport_and_cancellable(
        &config, "auto", stub_transport, NULL, 7, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 1);
    codexbar_provider_free(provider);
    json_object_put(raw);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/api-providers4/factory/parsers", test_factory_parsers);
    g_test_add_func("/api-providers4/factory/transport", test_factory_transport_and_credentials);
    g_test_add_func("/api-providers4/gemini/parser", test_gemini_parser);
    g_test_add_func("/api-providers4/gemini/transport", test_gemini_transport_unauthorized_and_cancellation);
    g_test_add_func("/api-providers4/ollama/api", test_ollama_parser_transport_and_security);
    g_test_add_func("/api-providers4/ollama/web", test_ollama_web_parser_and_source_routing);
    return g_test_run();
}
