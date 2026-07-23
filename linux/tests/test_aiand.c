#include "aiand.h"
#include "render.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

typedef struct {
    const char *bodies[10];
    long statuses[10];
    guint response_count;
    guint count;
    const char *authorization;
    GCancellable *cancellable;
    gboolean cancel_after[10];
    gboolean network_failure[10];
    GPtrArray *urls;
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
    guint index = fixture.count++;
    g_assert_cmpuint(index, <, fixture.response_count);
    g_assert_cmpstr(request->method, ==, "GET");
    g_assert_cmpstr(request_header(request, "Authorization"), ==, fixture.authorization);
    g_assert_cmpstr(request_header(request, "Accept"), ==, "application/json");
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 4U * 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == fixture.cancellable);
    g_ptr_array_add(fixture.urls, g_strdup(request->url));
    if (fixture.cancel_after[index]) g_cancellable_cancel(fixture.cancellable);
    if (fixture.network_failure[index]) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED, "connection refused");
        return NULL;
    }
    return make_response(fixture.statuses[index], fixture.bodies[index]);
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static void reset_fixture(const char *authorization, guint responses) {
    if (fixture.urls) g_ptr_array_unref(fixture.urls);
    memset(&fixture, 0, sizeof(fixture));
    fixture.authorization = authorization;
    fixture.response_count = responses;
    fixture.urls = g_ptr_array_new_with_free_func(g_free);
}

static const char *confidence(const CodexBarProvider *provider) {
    json_object *value = NULL;
    g_assert_true(json_object_object_get_ex(provider->usage_extensions, "dataConfidence", &value));
    return json_object_get_string(value);
}

static const char *final_page(void) {
    return "{\"data\":["
           "{\"cost\":\"7.02344000\",\"currency\":\"jpy\"},"
           "{\"cost\":\"1.10000000\",\"currency\":\"jpy\"},"
           "{\"cost\":null,\"currency\":\"jpy\"}],\"has_more\":false}";
}

static const char *first_page(void) {
    return "{\"data\":[{\"cost\":\"12.00000000\",\"currency\":\"jpy\"},"
           "{\"cost\":\"0.50000000\",\"currency\":\"jpy\"}],\"has_more\":true,"
           "\"next_after\":\"2026-07-17 10:24:30.094374+00\","
           "\"next_after_id\":\"912bf992-0000-4000-8000-000000000002\"}";
}

static void test_single_page_mapping(void) {
    g_setenv("AIAND_API_KEY", "environment", TRUE);
    reset_fixture("Bearer fixture-key", 1);
    fixture.statuses[0] = 200;
    fixture.bodies[0] = final_page();
    CodexBarProviderConfig config = {.api_key = " 'fixture-key' "};
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_aiand_fetch_with_transport(&config, stub_transport, 1800000000000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->provider, ==, "aiand");
    g_assert_cmpstr(provider->source, ==, "api");
    g_assert_cmpuint(provider->quota_windows->len, ==, 0);
    g_assert_null(provider->identity);
    g_assert_cmpstr(confidence(provider), ==, "exact");
    g_assert_nonnull(provider->provider_cost);
    g_assert_cmpfloat_with_epsilon(provider->provider_cost->used, 8.12344, 0.0000001);
    g_assert_cmpfloat(provider->provider_cost->limit, ==, 0.0);
    g_assert_cmpstr(provider->provider_cost->currency, ==, "JPY");
    g_assert_cmpstr(provider->provider_cost->period, ==, "Last 30 days");
    g_assert_cmpstr(g_ptr_array_index(fixture.urls, 0), ==,
                    "https://api.aiand.com/logs?range=30days&limit=100");

    CodexBarSnapshot *snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    g_ptr_array_add(snapshot->providers, provider);
    char *rendered = codexbar_render_usage_json(snapshot, FALSE);
    g_assert_nonnull(strstr(rendered, "\"providerCost\":{\"used\":8.12344"));
    g_assert_nonnull(strstr(rendered, "\"dataConfidence\":\"exact\""));
    g_free(rendered);
    char *text = codexbar_render_usage_text(snapshot);
    g_assert_nonnull(strstr(text, "API spend: 8.12 JPY · Last 30 days"));
    g_free(text);
    char *waybar = codexbar_render_waybar(snapshot);
    g_assert_nonnull(strstr(waybar, "API spend"));
    g_free(waybar);
    codexbar_snapshot_free(snapshot);
    g_unsetenv("AIAND_API_KEY");
}

static void test_pagination(void) {
    reset_fixture("Bearer key", 2);
    fixture.statuses[0] = 200;
    fixture.statuses[1] = 200;
    fixture.bodies[0] = first_page();
    fixture.bodies[1] = final_page();
    CodexBarProviderConfig config = {.api_key = "key"};
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_aiand_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(fixture.urls, 1), ==,
                    "https://api.aiand.com/logs?range=30days&limit=100&after="
                    "2026-07-17%2010:24:30.094374%2B00&after_id="
                    "912bf992-0000-4000-8000-000000000002");
    g_assert_cmpfloat_with_epsilon(provider->provider_cost->used, 20.62344, 0.0000001);
    g_assert_cmpstr(confidence(provider), ==, "exact");
    codexbar_provider_free(provider);

    reset_fixture("Bearer key", 2);
    fixture.statuses[0] = 200;
    fixture.statuses[1] = 200;
    fixture.bodies[0] = "{\"data\":[],\"has_more\":true,"
                        "\"next_after\":\"\\u96ea +00&x\",\"next_after_id\":\"id/\\u96ea\"}";
    fixture.bodies[1] = final_page();
    provider = codexbar_aiand_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(g_ptr_array_index(fixture.urls, 1), ==,
                    "https://api.aiand.com/logs?range=30days&limit=100&after="
                    "%E9%9B%AA%20%2B00%26x&after_id=id%2F%E9%9B%AA");
    codexbar_provider_free(provider);
}

static void test_partial_results(void) {
    CodexBarProviderConfig config = {.api_key = "key"};
    GError *error = NULL;
    reset_fixture("Bearer key", 1);
    fixture.statuses[0] = 200;
    fixture.bodies[0] = "{\"data\":[{\"cost\":\"2.5\",\"currency\":\"jpy\"}],\"has_more\":true}";
    CodexBarProvider *provider = codexbar_aiand_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->provider_cost->period, ==, "Last 30 days (partial)");
    g_assert_cmpstr(confidence(provider), ==, "estimated");
    codexbar_provider_free(provider);

    reset_fixture("Bearer key", 10);
    for (guint index = 0; index < 10; index++) {
        fixture.statuses[index] = 200;
        fixture.bodies[index] = first_page();
    }
    provider = codexbar_aiand_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 10);
    g_assert_cmpfloat_with_epsilon(provider->provider_cost->used, 125.0, 0.0000001);
    g_assert_cmpstr(confidence(provider), ==, "estimated");
    codexbar_provider_free(provider);
}

static void test_currency_and_decimal_aggregation(void) {
    CodexBarProviderConfig config = {.api_key = "key"};
    GError *error = NULL;
    reset_fixture("Bearer key", 1);
    fixture.statuses[0] = 200;
    fixture.bodies[0] = "{\"data\":["
                        "{\"cost\":\"5\",\"currency\":\"jpy\\u0000bad\"},"
                        "{\"cost\":\"0.1\",\"currency\":\" jpy \"},"
                        "{\"cost\":\"0.1\",\"currency\":\"JPY\"},"
                        "{\"cost\":\"0.1\",\"currency\":\"jpy\"},"
                        "{\"cost\":\"9.5\",\"currency\":\"usd\"},"
                        "{\"cost\":\"bad\",\"currency\":\"jpy\"},"
                        "{\"cost\":\"4.2\",\"currency\":null}],\"has_more\":false}";
    CodexBarProvider *provider = codexbar_aiand_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 0.3);
    g_assert_cmpstr(provider->provider_cost->currency, ==, "JPY");
    codexbar_provider_free(provider);

    reset_fixture("Bearer key", 1);
    fixture.statuses[0] = 200;
    fixture.bodies[0] = "{\"data\":["
                        "{\"cost\":\"100000000000000000000\",\"currency\":\"jpy\"},"
                        "{\"cost\":\"0.1\",\"currency\":\"jpy\"},"
                        "{\"cost\":\"-100000000000000000000\",\"currency\":\"jpy\"}],"
                        "\"has_more\":false}";
    provider = codexbar_aiand_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 0.1);
    codexbar_provider_free(provider);

    reset_fixture("Bearer key", 1);
    fixture.statuses[0] = 200;
    fixture.bodies[0] = "{\"data\":[{\"cost\":\"4.2\",\"currency\":null},"
                        "{\"cost\":null,\"currency\":\"jpy\"}],\"has_more\":false}";
    provider = codexbar_aiand_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_null(provider->provider_cost);
    g_assert_cmpstr(confidence(provider), ==, "exact");
    codexbar_provider_free(provider);
}

static void assert_malformed(const char *body) {
    reset_fixture("Bearer key", 1);
    fixture.statuses[0] = 200;
    fixture.bodies[0] = body;
    CodexBarProviderConfig config = {.api_key = "key"};
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_aiand_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_malformed_pages(void) {
    assert_malformed("{}");
    assert_malformed("{\"data\":{}}");
    assert_malformed("{\"data\":[{\"cost\":1,\"currency\":\"jpy\"}]}");
    assert_malformed("{\"data\":[null]}");
    assert_malformed("{\"data\":[{\"cost\":\"1\",\"currency\":1}]}");
    assert_malformed("{\"data\":[],\"has_more\":1}");
    assert_malformed("{\"data\":[],\"next_after\":1}");
    assert_malformed("{\"data\":[],\"next_after_id\":1}");
    assert_malformed("{\"data\":[]} trailing");
    assert_malformed("{\"data\":[]}\v");
}

static void test_credentials_errors_and_cancellation(void) {
    g_unsetenv("AIAND_API_KEY");
    CodexBarProviderConfig config = {0};
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_aiand_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    g_assert_false(codexbar_aiand_has_api_key(&config));
    g_setenv("AIAND_API_KEY", "\xE2\x80\x83", TRUE);
    g_assert_false(codexbar_aiand_has_api_key(&config));
    g_setenv("AIAND_API_KEY", " environment ", TRUE);
    g_assert_true(codexbar_aiand_has_api_key(&config));
    g_unsetenv("AIAND_API_KEY");

    config.api_key = "\xC2\xA0'key'\xC2\xA0";
    reset_fixture("Bearer key", 1);
    fixture.statuses[0] = 200;
    fixture.bodies[0] = final_page();
    provider = codexbar_aiand_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    codexbar_provider_free(provider);

    config.api_key = "key";
    const long statuses[] = {401, 402, 429, 500};
    const int codes[] = {G_IO_ERROR_PERMISSION_DENIED, G_IO_ERROR_NO_SPACE, G_IO_ERROR_BUSY, G_IO_ERROR_FAILED};
    for (guint index = 0; index < G_N_ELEMENTS(statuses); index++) {
        reset_fixture("Bearer key", 1);
        fixture.statuses[0] = statuses[index];
        fixture.bodies[0] = "{}";
        provider = codexbar_aiand_fetch_with_transport(&config, stub_transport, 1, &error);
        g_assert_null(provider);
        g_assert_error(error, G_IO_ERROR, codes[index]);
        if (statuses[index] == 401) g_assert_nonnull(strstr(error->message, "console.aiand.com"));
        if (statuses[index] == 402) g_assert_nonnull(strstr(error->message, "credits"));
        if (statuses[index] == 429) g_assert_nonnull(strstr(error->message, "rate limit"));
        if (statuses[index] == 500) g_assert_nonnull(strstr(error->message, "500"));
        g_clear_error(&error);
    }

    reset_fixture("Bearer key", 1);
    fixture.network_failure[0] = TRUE;
    provider = codexbar_aiand_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED);
    g_clear_error(&error);

    GCancellable *cancellable = g_cancellable_new();
    reset_fixture("Bearer key", 1);
    fixture.cancellable = cancellable;
    fixture.statuses[0] = 200;
    fixture.bodies[0] = first_page();
    fixture.cancel_after[0] = TRUE;
    provider = codexbar_aiand_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpuint(fixture.count, ==, 1);
    g_clear_error(&error);
    g_object_unref(cancellable);

    cancellable = g_cancellable_new();
    reset_fixture("Bearer key", 1);
    fixture.cancellable = cancellable;
    fixture.statuses[0] = 200;
    fixture.bodies[0] = final_page();
    fixture.cancel_after[0] = TRUE;
    provider = codexbar_aiand_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(cancellable);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/aiand/single-page-mapping", test_single_page_mapping);
    g_test_add_func("/aiand/pagination", test_pagination);
    g_test_add_func("/aiand/partial-results", test_partial_results);
    g_test_add_func("/aiand/currency-decimal", test_currency_and_decimal_aggregation);
    g_test_add_func("/aiand/malformed-pages", test_malformed_pages);
    g_test_add_func("/aiand/credentials-errors-cancellation", test_credentials_errors_and_cancellation);
    int result = g_test_run();
    if (fixture.urls) g_ptr_array_unref(fixture.urls);
    return result;
}
