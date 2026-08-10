#include "fireworks.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

typedef struct {
    const char *url;
    const char *authorization;
    long status;
    const char *body;
    GCancellable *cancellable;
    gboolean cancel_after_response;
    guint calls;
} TransportFixture;

static TransportFixture fixture;

static CodexBarHttpResponse *make_response(long status, const char *body) {
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = status;
    response->body = g_strdup(body ? body : "");
    response->body_length = strlen(response->body);
    response->headers = g_ptr_array_new();
    return response;
}

static const char *request_header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static CodexBarHttpResponse *stub_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    fixture.calls++;
    g_assert_cmpstr(request->url, ==, fixture.url);
    g_assert_cmpstr(request->method, ==, "GET");
    g_assert_cmpstr(request_header(request, "Authorization"), ==, fixture.authorization);
    g_assert_cmpstr(request_header(request, "Accept"), ==, "application/json");
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_DENY);
    g_assert_true(request->cancellable == fixture.cancellable);
    CodexBarHttpResponse *response = make_response(fixture.status, fixture.body);
    if (fixture.cancel_after_response) g_cancellable_cancel(fixture.cancellable);
    return response;
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static json_object *raw_config(const char *slug) {
    json_object *raw = json_object_new_object();
    if (slug) json_object_object_add(raw, "accountSlug", json_object_new_string(slug));
    return raw;
}

static void reset_fixture(const char *url, const char *authorization, long status, const char *body) {
    fixture = (TransportFixture){
        .url = url,
        .authorization = authorization,
        .status = status,
        .body = body,
    };
}

static void test_summary_parsing(void) {
    const char *json =
        "{\"lineItems\":["
        "{\"totalCost\":{\"currencyCode\":\"USD\",\"nanos\":492256016,\"units\":\"0\"}},"
        "{\"totalCost\":{\"currencyCode\":\"EUR\",\"nanos\":900000000,\"units\":\"9\"}},"
        "{\"totalCost\":{\"currencyCode\":\"USD\",\"nanos\":33292280,\"units\":\"1\"}},"
        "{\"totalCost\":{\"currencyCode\":\"USD\",\"nanos\":1,\"units\":\"bad\"}}]}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_fireworks_parse_summary(json, 1700000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->provider, ==, "fireworks");
    g_assert_cmpstr(provider->source, ==, "api");
    g_assert_cmpuint(provider->quota_windows->len, ==, 0);
    g_assert_nonnull(provider->provider_cost);
    g_assert_cmpfloat_with_epsilon(provider->provider_cost->used, 1.525548296, 0.000000001);
    g_assert_cmpfloat(provider->provider_cost->limit, ==, 0.0);
    g_assert_cmpstr(provider->provider_cost->currency, ==, "USD");
    g_assert_cmpstr(provider->provider_cost->period, ==, "Last 30 days");
    g_assert_true(provider->provider_cost->has_updated_at);
    g_assert_cmpint(provider->provider_cost->updated_at_ms, ==, G_GINT64_CONSTANT(1700000000000));
    codexbar_provider_free(provider);

    provider = codexbar_fireworks_parse_summary("{}", 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_null(provider->provider_cost);
    g_assert_cmpuint(provider->quota_windows->len, ==, 0);
    codexbar_provider_free(provider);

    provider = codexbar_fireworks_parse_summary("{\"lineItems\":[]}", 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_null(provider->provider_cost);
    codexbar_provider_free(provider);
}

static void assert_parse_error_bytes(const char *json, size_t length) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_fireworks_parse_summary_bytes(json, length, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_summary_errors(void) {
    assert_parse_error_bytes("[]", 2);
    assert_parse_error_bytes("{\"lineItems\":{}}", strlen("{\"lineItems\":{}}"));
    assert_parse_error_bytes("{\"lineItems\":[]}", strlen("{\"lineItems\":[]}") - 1);
    assert_parse_error_bytes("{\"lineItems\":[]} trailing", strlen("{\"lineItems\":[]} trailing"));
    static const char embedded_nul[] = "{\"lineItems\":[]}\0trailing";
    assert_parse_error_bytes(embedded_nul, sizeof(embedded_nul) - 1);
}

static void test_request_and_config_credentials(void) {
    g_setenv("FIREWORKS_API_KEY", "environment-key", TRUE);
    g_setenv("FIREWORKS_KEY", "fallback-key", TRUE);
    g_setenv("FIREWORKS_ACCOUNT_SLUG", "environment-slug", TRUE);
    const char *url = "https://api.fireworks.ai/v1/accounts/acct-1_x.d/billing/summary?"
                      "startTime=2026-07-11T12:34:56Z&endTime=2026-08-10T12:34:56Z";
    reset_fixture(url, "Bearer config-key", 200, "{\"lineItems\":[]}");
    CodexBarProviderConfig config = {.api_key = " 'config-key' ", .raw = raw_config(" acct-1_x.d ")};
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_fireworks_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, G_GINT64_CONSTANT(1786365296000), &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.calls, ==, 1);
    codexbar_provider_free(provider);
    json_object_put(config.raw);

    config = (CodexBarProviderConfig){0};
    url = "https://api.fireworks.ai/v1/accounts/environment-slug/billing/summary?"
          "startTime=2026-07-11T12:34:56Z&endTime=2026-08-10T12:34:56Z";
    reset_fixture(url, "Bearer environment-key", 200, "{}");
    provider = codexbar_fireworks_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, G_GINT64_CONSTANT(1786365296000), &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    codexbar_provider_free(provider);

    g_unsetenv("FIREWORKS_API_KEY");
    reset_fixture(url, "Bearer fallback-key", 200, "{}");
    provider = codexbar_fireworks_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, G_GINT64_CONSTANT(1786365296000), &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    codexbar_provider_free(provider);
    g_unsetenv("FIREWORKS_KEY");
    g_unsetenv("FIREWORKS_ACCOUNT_SLUG");
}

static void test_credentials_and_slug_fail_before_network(void) {
    g_unsetenv("FIREWORKS_API_KEY");
    g_unsetenv("FIREWORKS_KEY");
    g_unsetenv("FIREWORKS_ACCOUNT_SLUG");
    GError *error = NULL;
    CodexBarProviderConfig config = {.raw = raw_config("account")};
    CodexBarProvider *provider = codexbar_fireworks_fetch_with_transport_and_cancellable(
        &config, unexpected_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    json_object_put(config.raw);

    config = (CodexBarProviderConfig){.api_key = "key"};
    provider = codexbar_fireworks_fetch_with_transport_and_cancellable(
        &config, unexpected_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

    const char *invalid[] = {"sp ace", "has/slash", "has?query", "has#fragment", "percent%2F", "col\xC3\xA9on"};
    for (guint index = 0; index < G_N_ELEMENTS(invalid); index++) {
        config.raw = raw_config(invalid[index]);
        provider = codexbar_fireworks_fetch_with_transport_and_cancellable(
            &config, unexpected_transport, NULL, 1, &error);
        g_assert_null(provider);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_clear_error(&error);
        json_object_put(config.raw);
    }
}

static void test_http_errors_and_cancellation(void) {
    CodexBarProviderConfig config = {.api_key = "credential-secret", .raw = raw_config("account")};
    const char *url = "https://api.fireworks.ai/v1/accounts/account/billing/summary?"
                      "startTime=1969-12-02T00:00:00Z&endTime=1970-01-01T00:00:00Z";
    const long statuses[] = {401, 403, 429, 500};
    const int codes[] = {G_IO_ERROR_PERMISSION_DENIED, G_IO_ERROR_PERMISSION_DENIED, G_IO_ERROR_BUSY, G_IO_ERROR_FAILED};
    for (guint index = 0; index < G_N_ELEMENTS(statuses); index++) {
        reset_fixture(url, "Bearer credential-secret", statuses[index], "{\"error\":\"body-secret\"}");
        GError *error = NULL;
        CodexBarProvider *provider = codexbar_fireworks_fetch_with_transport_and_cancellable(
            &config, stub_transport, NULL, 0, &error);
        g_assert_null(provider);
        g_assert_error(error, G_IO_ERROR, codes[index]);
        g_assert_null(strstr(error->message, "credential-secret"));
        g_assert_null(strstr(error->message, "body-secret"));
        g_clear_error(&error);
    }

    GCancellable *cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_fireworks_fetch_with_transport_and_cancellable(
        &config, unexpected_transport, cancellable, 0, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);

    g_cancellable_reset(cancellable);
    reset_fixture(url, "Bearer credential-secret", 200, "{}");
    fixture.cancellable = cancellable;
    fixture.cancel_after_response = TRUE;
    provider = codexbar_fireworks_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 0, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(cancellable);
    json_object_put(config.raw);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/fireworks/summary-parsing", test_summary_parsing);
    g_test_add_func("/fireworks/summary-errors", test_summary_errors);
    g_test_add_func("/fireworks/request-config-credentials", test_request_and_config_credentials);
    g_test_add_func("/fireworks/credentials-slug-validation", test_credentials_and_slug_fail_before_network);
    g_test_add_func("/fireworks/http-errors-cancellation", test_http_errors_and_cancellation);
    return g_test_run();
}
