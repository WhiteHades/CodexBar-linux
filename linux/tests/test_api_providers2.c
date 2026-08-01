#include "api_providers2.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

typedef struct {
    const char *provider;
    const char *authorization;
    const char *bodies[4];
    long statuses[4];
    guint response_count;
    guint count;
    GCancellable *cancellable;
    gboolean cancel_after_first;
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
    guint index = fixture.count++;
    g_assert_cmpuint(index, <, fixture.response_count);
    g_assert_cmpstr(request_header(request, "Authorization"), ==, fixture.authorization);
    g_assert_cmpstr(request_header(request, "Accept"), ==, "application/json");
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == fixture.cancellable);
    if (g_str_equal(fixture.provider, "synthetic")) {
        g_assert_cmpstr(request->url, ==, "https://api.synthetic.new/v2/quotas");
        g_assert_cmpstr(request->method, ==, "GET");
        g_assert_null(request->body);
    } else if (g_str_equal(fixture.provider, "warp")) {
        g_assert_cmpstr(request->url, ==, "https://app.warp.dev/graphql/v2?op=GetRequestLimitInfo");
        g_assert_cmpstr(request->method, ==, "POST");
        g_assert_cmpstr(request_header(request, "Content-Type"), ==, "application/json");
        g_assert_cmpstr(request_header(request, "x-warp-client-id"), ==, "warp-app");
        g_assert_cmpstr(request_header(request, "x-warp-os-category"), ==, "Linux");
        g_assert_cmpstr(request_header(request, "User-Agent"), ==, "Warp/1.0");
        g_assert_nonnull(request->body);
        json_object *body = json_tokener_parse(request->body);
        g_assert_nonnull(body);
        g_assert_cmpstr(json_object_get_string(json_object_object_get(body, "operationName")),
                        ==,
                        "GetRequestLimitInfo");
        json_object_put(body);
    } else {
        g_assert_cmpstr(request->method, ==, "GET");
        g_assert_true(g_str_has_prefix(request->url,
                                      "https://metrics.groq.test/custom/metrics/prometheus/api/v1/query?query="));
        g_assert_nonnull(strstr(request->url, "%3A"));
    }
    CodexBarHttpResponse *response = make_response(fixture.statuses[index], fixture.bodies[index]);
    if (fixture.cancel_after_first && index == 0) g_cancellable_cancel(fixture.cancellable);
    return response;
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static void reset_fixture(const char *provider, const char *authorization, guint response_count) {
    memset(&fixture, 0, sizeof(fixture));
    fixture.provider = provider;
    fixture.authorization = authorization;
    fixture.response_count = response_count;
}

static void test_synthetic_known_slots_and_cost(void) {
    const char *json =
        "{\"plan\":\"Starter\","
        "\"weeklyTokenLimit\":{\"percentRemaining\":98.0,\"nextRegenAt\":\"2026-04-17T05:19:30Z\","
        "\"maxCredits\":\"$36.00\",\"remainingCredits\":\"$35.30\",\"nextRegenCredits\":\"$0.72\"},"
        "\"search\":{\"hourly\":{\"limit\":250,\"requests\":2,"
        "\"renewsAt\":\"2026-04-17T04:30:01.494Z\"}}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_synthetic_parse_usage(json, 123000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->provider, ==, "synthetic");
    g_assert_cmpstr(provider->identity->login_method, ==, "Starter");
    g_assert_true(provider->explicit_quota_slots);
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    CodexBarQuotaWindow *secondary = codexbar_provider_quota_window(provider, 0);
    CodexBarQuotaWindow *tertiary = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpstr(secondary->id, ==, "secondary");
    g_assert_cmpfloat_with_epsilon(secondary->used_percent, 2, 0.0001);
    g_assert_true(secondary->has_resets_at);
    g_assert_cmpstr(tertiary->id, ==, "tertiary");
    g_assert_cmpfloat_with_epsilon(tertiary->used_percent, 0.8, 0.0001);
    g_assert_nonnull(provider->provider_cost);
    g_assert_cmpfloat(provider->provider_cost->limit, ==, 36);
    g_assert_cmpfloat_with_epsilon(provider->provider_cost->used, 0.7, 0.0001);
    g_assert_true(provider->provider_cost->has_next_regen);
    g_assert_cmpfloat(provider->provider_cost->next_regen, ==, 0.72);
    codexbar_provider_free(provider);
}

static void test_synthetic_fallback_and_malformed(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_synthetic_parse_usage(
        "{\"subscription\":{\"rateLimit\":{\"messages\":1000,\"requests\":250,\"period\":\"5hr\"}}}",
        1,
        &error);
    g_assert_no_error(error);
    CodexBarQuotaWindow *primary = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat(primary->used_percent, ==, 25);
    g_assert_true(primary->has_window_minutes);
    g_assert_cmpint(primary->window_minutes, ==, 300);
    g_assert_cmpstr(primary->reset_description, ==, "5 hours window");
    codexbar_provider_free(provider);

    provider = codexbar_synthetic_parse_usage("{} trailing", 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    static const char embedded_nul[] = "{\"quotas\":[{\"limit\":1,\"used\":0}]}\0junk";
    provider = codexbar_synthetic_parse_usage_bytes(embedded_nul, sizeof(embedded_nul) - 1, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static const char *warp_response(void) {
    return "{\"data\":{\"user\":{\"__typename\":\"UserOutput\",\"user\":{"
           "\"requestLimitInfo\":{\"isUnlimited\":false,\"nextRefreshTime\":\"2026-02-28T19:16:33.462988Z\","
           "\"requestLimit\":\"1500\",\"requestsUsedSinceLastRefresh\":\"5\"},"
           "\"bonusGrants\":[{\"requestCreditsGranted\":20,\"requestCreditsRemaining\":10,"
           "\"expiration\":\"2026-03-01T10:00:00Z\"}],"
           "\"workspaces\":[{\"bonusGrantsInfo\":{\"grants\":[{\"requestCreditsGranted\":\"15\","
           "\"requestCreditsRemaining\":\"5\",\"expiration\":\"2026-03-15T10:00:00Z\"}]}}]}}}}";
}

static void test_warp_parser(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_warp_parse_usage(warp_response(), 777, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->provider, ==, "warp");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    CodexBarQuotaWindow *primary = codexbar_provider_quota_window(provider, 0);
    CodexBarQuotaWindow *bonus = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpfloat_with_epsilon(primary->used_percent, 5.0 / 15.0, 0.0001);
    g_assert_cmpstr(primary->reset_description, ==, "5/1500 credits");
    g_assert_true(primary->has_resets_at);
    g_assert_cmpfloat_with_epsilon(bonus->used_percent, 20.0 / 35.0 * 100, 0.0001);
    g_assert_nonnull(strstr(bonus->reset_description, "10 credits expire"));
    codexbar_provider_free(provider);

    provider = codexbar_warp_parse_usage("{\"errors\":[{\"message\":\"Unauthorized\"}]}", 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr(error->message, "Unauthorized"));
    g_clear_error(&error);
    provider = codexbar_warp_parse_usage(
        "{\"data\":{\"user\":{\"__typename\":\"AuthError\"}}}", 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull(strstr(error->message, "AuthError"));
    g_clear_error(&error);
}

static const char *groq_scalar(void) {
    return "{\"status\":\"success\",\"data\":{\"result\":[{\"value\":[1710000000,\"2.5\"]},"
           "{\"value\":[1710000000,1.5]}]}}";
}

static void test_groq_parser(void) {
    GError *error = NULL;
    double value = 0;
    g_assert_true(codexbar_groq_parse_scalar(groq_scalar(), &value, &error));
    g_assert_no_error(error);
    g_assert_cmpfloat(value, ==, 4);
    CodexBarProvider *provider = codexbar_groq_parse_usage(
        groq_scalar(), groq_scalar(), groq_scalar(), groq_scalar(), 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 3);
    g_assert_cmpstr(codexbar_provider_quota_window(provider, 0)->reset_description, ==, "240 req/min");
    g_assert_cmpstr(codexbar_provider_quota_window(provider, 1)->reset_description, ==, "480 tok/min");
    g_assert_cmpstr(codexbar_provider_quota_window(provider, 2)->reset_description, ==, "240 cache/min");
    codexbar_provider_free(provider);

    g_assert_false(codexbar_groq_parse_scalar("{\"status\":\"error\",\"error\":\"bad query\"}",
                                               &value,
                                               &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&error);
    g_assert_false(codexbar_groq_parse_scalar("{\"status\":\"success\",\"data\":{}}", &value, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_transports_and_credentials(void) {
    g_unsetenv("SYNTHETIC_API_KEY");
    g_unsetenv("WARP_API_KEY");
    g_unsetenv("WARP_TOKEN");
    g_unsetenv("GROQ_API_KEY");
    g_unsetenv("GROQ_API_URL");
    CodexBarProviderConfig config = {0};
    GError *error = NULL;
    g_assert_false(codexbar_synthetic_has_api_key(&config));
    CodexBarProvider *provider = codexbar_synthetic_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

    config.api_key = " 'fixture-key' ";
    reset_fixture("synthetic", "Bearer fixture-key", 1);
    fixture.statuses[0] = 200;
    fixture.bodies[0] = "{\"quotas\":[{\"limit\":100,\"used\":25}]}";
    provider = codexbar_synthetic_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    codexbar_provider_free(provider);

    config.api_key = NULL;
    g_setenv("WARP_TOKEN", "warp-key", TRUE);
    reset_fixture("warp", "Bearer warp-key", 1);
    fixture.statuses[0] = 200;
    fixture.bodies[0] = warp_response();
    provider = codexbar_warp_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    codexbar_provider_free(provider);
    g_unsetenv("WARP_TOKEN");

    config.api_key = "groq-key";
    g_setenv("GROQ_API_URL", "metrics.groq.test/custom", TRUE);
    reset_fixture("groq", "Bearer groq-key", 4);
    for (size_t index = 0; index < 4; index++) {
        fixture.statuses[index] = 200;
        fixture.bodies[index] = groq_scalar();
    }
    provider = codexbar_groq_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 4);
    codexbar_provider_free(provider);

    g_setenv("GROQ_API_URL", "http://metrics.groq.test", TRUE);
    provider = codexbar_groq_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_setenv("GROQ_API_URL", "https://user:secret@metrics.groq.test", TRUE);
    provider = codexbar_groq_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_unsetenv("GROQ_API_URL");

    config.api_key = "line\nbreak";
    g_assert_false(codexbar_groq_has_api_key(&config));
    provider = codexbar_groq_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
}

static void test_cancellation(void) {
    CodexBarProviderConfig config = {.api_key = "key"};
    GError *error = NULL;
    GCancellable *cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);
    fixture.cancellable = cancellable;
    CodexBarProvider *provider = codexbar_synthetic_fetch_with_transport_and_cancellable(
        &config, unexpected_transport, cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(cancellable);

    cancellable = g_cancellable_new();
    reset_fixture("groq", "Bearer key", 1);
    fixture.cancellable = cancellable;
    fixture.cancel_after_first = TRUE;
    fixture.statuses[0] = 200;
    fixture.bodies[0] = groq_scalar();
    g_setenv("GROQ_API_URL", "metrics.groq.test/custom", TRUE);
    provider = codexbar_groq_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpuint(fixture.count, ==, 1);
    g_clear_error(&error);
    g_object_unref(cancellable);
    g_unsetenv("GROQ_API_URL");
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/api-providers2/synthetic-known", test_synthetic_known_slots_and_cost);
    g_test_add_func("/api-providers2/synthetic-fallback-malformed", test_synthetic_fallback_and_malformed);
    g_test_add_func("/api-providers2/warp-parser", test_warp_parser);
    g_test_add_func("/api-providers2/groq-parser", test_groq_parser);
    g_test_add_func("/api-providers2/transports-security", test_transports_and_credentials);
    g_test_add_func("/api-providers2/cancellation", test_cancellation);
    return g_test_run();
}
