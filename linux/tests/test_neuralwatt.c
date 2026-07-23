#include "neuralwatt.h"

#include "render.h"

#include <curl/curl.h>
#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

typedef struct {
    const char *expected_url;
    const char *expected_authorization;
    long statuses[3];
    const char *bodies[3];
    const char *retry_after[3];
    gboolean network_failure[3];
    int curl_error[3];
    gboolean cancel_after[3];
    GCancellable *cancellable;
    guint count;
} NeuralWattFixture;

static NeuralWattFixture fixture;

static gpointer delayed_cancel(gpointer data) {
    g_usleep(20000);
    g_cancellable_cancel(data);
    return NULL;
}

static void response_header_free(CodexBarHttpResponseHeader *header) {
    if (!header) return;
    g_free(header->name);
    g_free(header->value);
    g_free(header);
}

static void add_response_header(GPtrArray *headers, const char *name, const char *value) {
    CodexBarHttpResponseHeader *header = g_new0(CodexBarHttpResponseHeader, 1);
    header->name = g_strdup(name);
    header->value = g_strdup(value);
    g_ptr_array_add(headers, header);
}

static const char *canonical_body(void) {
    return "{"
           "\"snapshot_at\":\"2026-04-16T18:30:00Z\","
           "\"balance\":{\"credits_remaining_usd\":32.6774,\"total_credits_usd\":52.34,"
           "\"credits_used_usd\":19.6626,\"accounting_method\":\"energy\"},"
           "\"usage\":{\"lifetime\":{\"cost_usd\":243.9145,\"requests\":37801,"
           "\"tokens\":1235477176,\"energy_kwh\":15.6009},"
           "\"current_month\":{\"cost_usd\":160.1463,\"requests\":23902,"
           "\"tokens\":1116658995,\"energy_kwh\":9.7278}},"
           "\"limits\":{\"overage_limit_usd\":null,\"rate_limit_tier\":\"standard\"},"
           "\"subscription\":{\"plan\":\"standard\",\"status\":\"active\","
           "\"billing_interval\":\"month\",\"current_period_start\":\"2026-04-11T05:05:25Z\","
           "\"current_period_end\":\"2026-05-11T05:05:25Z\",\"auto_renew\":true,"
           "\"kwh_included\":20,\"kwh_used\":13.9023,\"kwh_remaining\":6.0977,"
           "\"in_overage\":false},"
           "\"key\":{\"name\":\"my-production-key\",\"allowance\":{\"limit_usd\":50,"
           "\"period\":\"monthly\",\"spent_usd\":12.5,\"remaining_usd\":37.5,"
           "\"blocked\":false}}}";
}

static const char *minimal_body(void) {
    return "{\"balance\":{\"credits_remaining_usd\":5},\"subscription\":null,"
           "\"key\":{\"name\":\"retry\",\"allowance\":null}}";
}

static void reset_fixture(void) {
    memset(&fixture, 0, sizeof(fixture));
    fixture.expected_url = "https://api.neuralwatt.test/v1/quota";
    fixture.expected_authorization = "Bearer config-key";
}

static CodexBarHttpResponse *stub_transport(const CodexBarHttpRequest *request, GError **error) {
    guint index = fixture.count++;
    g_assert_cmpuint(index, <, G_N_ELEMENTS(fixture.statuses));
    g_assert_cmpstr(request->method, ==, "GET");
    g_assert_cmpstr(request->url, ==, fixture.expected_url);
    g_assert_cmpuint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == fixture.cancellable);
    g_assert_cmpuint(request->header_count, ==, 2);
    g_assert_cmpstr(request->headers[0].name, ==, "Authorization");
    g_assert_cmpstr(request->headers[0].value, ==, fixture.expected_authorization);
    g_assert_cmpstr(request->headers[1].name, ==, "Accept");
    g_assert_cmpstr(request->headers[1].value, ==, "application/json");
    if (fixture.network_failure[index]) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED, "connection refused");
        return NULL;
    }
    if (fixture.curl_error[index] != 0) {
        const char *message = fixture.curl_error[index] == CURLE_WRITE_ERROR
                                  ? "HTTP request failed: HTTP response exceeded the configured size limit"
                                  : "HTTP request failed: Server returned nothing";
        g_set_error_literal(error,
                            g_quark_from_static_string("codexbar-http-error"),
                            fixture.curl_error[index],
                            message);
        return NULL;
    }
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = fixture.statuses[index];
    response->body = g_strdup(fixture.bodies[index] ? fixture.bodies[index] : "{}");
    response->body_length = strlen(response->body);
    response->headers = g_ptr_array_new_with_free_func((GDestroyNotify)response_header_free);
    if (fixture.retry_after[index]) add_response_header(response->headers, "Retry-After", fixture.retry_after[index]);
    if (fixture.cancel_after[index] && fixture.cancellable) g_cancellable_cancel(fixture.cancellable);
    return response;
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static void test_mapping(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_neuralwatt_parse_usage(canonical_body(), 1000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->provider, ==, "neuralwatt");
    g_assert_cmpstr(provider->source, ==, "api");
    g_assert_true(provider->explicit_quota_slots);
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    const CodexBarQuotaWindow *primary = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpstr(primary->id, ==, "primary");
    g_assert_cmpstr(primary->title, ==, "Subscription");
    g_assert_cmpfloat_with_epsilon(primary->used_percent, 13.9023 / 20.0 * 100.0, 1e-9);
    g_assert_cmpstr(primary->detail, ==, "13.90 / 20 kWh");
    g_assert_true(primary->has_window_minutes);
    g_assert_cmpint(primary->window_minutes, ==, 43200);
    g_assert_true(primary->has_resets_at);
    g_assert_true(provider->has_subscription_renews_at);
    const CodexBarQuotaWindow *allowance = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpstr(allowance->id, ==, "key-allowance");
    g_assert_cmpstr(allowance->output_id, ==, "key-allowance");
    g_assert_cmpstr(allowance->title, ==, "Key Monthly");
    g_assert_cmpfloat(allowance->used_percent, ==, 25.0);
    g_assert_nonnull(provider->provider_cost);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 32.6774);
    g_assert_cmpfloat(provider->provider_cost->limit, ==, 0.0);
    g_assert_cmpstr(provider->provider_cost->period, ==, "Neuralwatt prepaid balance");
    g_assert_cmpstr(provider->identity->login_method, ==, "Standard plan");
    g_assert_cmpstr(json_object_get_string(json_object_object_get(provider->usage_extensions, "dataConfidence")),
                    ==,
                    "exact");

    CodexBarSnapshot snapshot = {.providers = g_ptr_array_new()};
    g_ptr_array_add(snapshot.providers, provider);
    char *json = codexbar_render_usage_json(&snapshot, FALSE);
    g_assert_nonnull(strstr(json, "\"resetDescription\":\"13.90 \\/ 20 kWh\""));
    g_assert_nonnull(strstr(json, "\"subscriptionRenewsAt\":"));
    json_object *rendered = json_tokener_parse(json);
    json_object *rendered_provider = json_object_array_get_idx(rendered, 0);
    json_object *usage = json_object_object_get(rendered_provider, "usage");
    json_object *cost = json_object_object_get(usage, "providerCost");
    g_assert_cmpfloat_with_epsilon(json_object_get_double(json_object_object_get(cost, "used")), 32.6774, 1e-9);
    json_object_put(rendered);
    g_free(json);
    char *text = codexbar_render_usage_text(&snapshot);
    g_assert_nonnull(strstr(text, "Standard plan"));
    g_assert_nonnull(strstr(text, "Pay-as-you-go: Balance: $32.68"));
    g_assert_null(strstr(text, "API spend"));
    g_free(text);
    char *waybar = codexbar_render_waybar(&snapshot);
    g_assert_nonnull(strstr(waybar, "Pay-as-you-go"));
    g_assert_nonnull(strstr(waybar, "Balance: $32.68"));
    g_assert_null(strstr(waybar, "API spend"));
    g_free(waybar);
    g_ptr_array_free(snapshot.providers, TRUE);
    codexbar_provider_free(provider);
}

static CodexBarProvider *parse_or_fail(const char *body) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_neuralwatt_parse_usage(body, 2000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    return provider;
}

static void test_sparse_and_derived_values(void) {
    CodexBarProvider *provider = parse_or_fail(
        "{\"balance\":{\"credits_remaining_usd\":4.5,\"total_credits_usd\":5,"
        "\"credits_used_usd\":0.5,\"accounting_method\":\"energy\"},"
        "\"usage\":{\"lifetime\":{},\"current_month\":{}},\"limits\":{},"
        "\"subscription\":null,\"key\":{\"name\":\"trial\",\"allowance\":null}}" );
    g_assert_cmpuint(provider->quota_windows->len, ==, 0);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 4.5);
    g_assert_cmpstr(provider->identity->login_method, ==, "Energy");
    codexbar_provider_free(provider);

    provider = parse_or_fail(
        "{\"balance\":{\"total_credits_usd\":100,\"credits_used_usd\":70},"
        "\"subscription\":null}" );
    g_assert_cmpfloat(provider->provider_cost->used, ==, 30.0);
    codexbar_provider_free(provider);

    provider = parse_or_fail(
        "{\"balance\":{\"credits_remaining_usd\":0,\"total_credits_usd\":0},"
        "\"subscription\":null}" );
    g_assert_nonnull(provider->provider_cost);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 0.0);
    codexbar_provider_free(provider);

    provider = parse_or_fail(
        "{\"balance\":{\"credits_remaining_usd\":1,\"accounting_method\":\"prepaid_energy\"},"
        "\"subscription\":null}" );
    g_assert_cmpstr(provider->identity->login_method, ==, "Prepaid_energy");
    codexbar_provider_free(provider);
}

static void test_subscription_variants(void) {
    CodexBarProvider *provider = parse_or_fail(
        "{\"balance\":{\"credits_remaining_usd\":0},\"subscription\":{"
        "\"plan\":\"pro_energy\",\"current_period_start\":\"2026-04-01T00:00:00.123Z\","
        "\"current_period_end\":\"2026-05-01T00:00:00.456Z\",\"auto_renew\":false,"
        "\"kwh_included\":10,\"kwh_used\":2.5,\"kwh_remaining\":7.5},"
        "\"key\":{\"allowance\":{\"blocked\":true,\"period\":\"monthly\"}}}" );
    const CodexBarQuotaWindow *primary = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat(primary->used_percent, ==, 25.0);
    g_assert_cmpstr(primary->detail, ==, "2.50 / 10 kWh");
    g_assert_false(provider->has_subscription_renews_at);
    g_assert_cmpstr(provider->identity->login_method, ==, "Pro Energy plan");
    const CodexBarQuotaWindow *allowance = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpfloat(allowance->used_percent, ==, 100.0);
    codexbar_provider_free(provider);

    provider = parse_or_fail(
        "{\"balance\":{\"credits_remaining_usd\":1},\"subscription\":{"
        "\"kwh_used\":2,\"kwh_remaining\":6},\"key\":{\"allowance\":{"
        "\"limit_usd\":20,\"spent_usd\":5,\"period\":\"per_key\"}}}" );
    primary = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat(primary->used_percent, ==, 25.0);
    g_assert_cmpstr(primary->detail, ==, "2 / 8 kWh");
    allowance = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpstr(allowance->title, ==, "Key Per_key");
    codexbar_provider_free(provider);

    provider = parse_or_fail(
        "{\"balance\":{\"credits_remaining_usd\":1},\"subscription\":{"
        "\"kwh_included\":1.7976931348623157e308,\"kwh_used\":1.7976931348623157e308}}" );
    primary = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpuint(strlen(primary->detail), >, 300);
    codexbar_provider_free(provider);
}

static void assert_malformed_bytes(const char *body, size_t length) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_neuralwatt_parse_usage_bytes(body, length, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull(strstr(error->message, "parse Neuralwatt"));
    g_clear_error(&error);
}

static void test_malformed_responses(void) {
    const char *bodies[] = {
        "{}",
        "{\"balance\":null}",
        "{\"balance\":{}}",
        "{\"balance\":{\"credits_remaining_usd\":-1}}",
        "{\"balance\":{\"credits_remaining_usd\":\"5\"}}",
        "{\"balance\":{\"credits_remaining_usd\":1},\"usage\":[]}",
        "{\"balance\":{\"credits_remaining_usd\":1},\"usage\":{\"lifetime\":{\"requests\":1.5}}}",
        "{\"balance\":{\"credits_remaining_usd\":1},\"usage\":{\"lifetime\":{\"requests\":9223372036854775808}}}",
        "{\"balance\":{\"credits_remaining_usd\":1},\"limits\":[]}",
        "{\"balance\":{\"credits_remaining_usd\":1},\"subscription\":{\"auto_renew\":1}}",
        "{\"balance\":{\"credits_remaining_usd\":1},\"subscription\":{\"current_period_end\":\"bad\"}}",
        "{\"balance\":{\"credits_remaining_usd\":1},\"subscription\":{\"current_period_end\":\"2026-05-01t00:00:00z\"}}",
        "{\"balance\":{\"credits_remaining_usd\":1},\"key\":{\"allowance\":[]}}",
        "{\"balance\":{\"credits_remaining_usd\":1}} trailing",
    };
    for (guint index = 0; index < G_N_ELEMENTS(bodies); index++) {
        assert_malformed_bytes(bodies[index], strlen(bodies[index]));
    }
    const char embedded_nul[] = "{\"balance\":{\"credits_remaining_usd\":1,\"accounting_method\":\"a\\u0000b\"}}";
    assert_malformed_bytes(embedded_nul, strlen(embedded_nul));
    const char invalid_utf8[] = "{\"balance\":{\"credits_remaining_usd\":1}}\xFF";
    assert_malformed_bytes(invalid_utf8, sizeof(invalid_utf8) - 1);
}

static void test_credentials_endpoint_and_request(void) {
    g_unsetenv("NEURALWATT_API_KEY");
    g_unsetenv("NEURALWATT_API_URL");
    CodexBarProviderConfig config = {0};
    GError *error = NULL;
    g_assert_false(codexbar_neuralwatt_has_api_key(&config));
    CodexBarProvider *provider =
        codexbar_neuralwatt_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

    g_setenv("NEURALWATT_API_KEY", " environment-key ", TRUE);
    config.api_key = "\xC2\xA0'config-key'\xC2\xA0";
    g_setenv("NEURALWATT_API_URL", " 'api.neuralwatt.test/v1' ", TRUE);
    reset_fixture();
    fixture.statuses[0] = 200;
    fixture.bodies[0] = minimal_body();
    provider = codexbar_neuralwatt_fetch_with_transport(&config, stub_transport, 123, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 1);
    codexbar_provider_free(provider);

    char *url = codexbar_neuralwatt_quota_url_for_testing("https://proxy.test/prefix", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "https://proxy.test/prefix/v1/quota");
    g_free(url);
    url = codexbar_neuralwatt_quota_url_for_testing("https://proxy.test/", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "https://proxy.test/v1/quota");
    g_free(url);

    const char *invalid[] = {
        "http://localhost",
        "https://user@example.com",
        "https://exa%2fmple.com",
        "https://api%20.neuralwatt.test",
        "https://api .neuralwatt.test",
        "https://api\\attacker.test",
        "bad\nhost",
    };
    for (guint index = 0; index < G_N_ELEMENTS(invalid); index++) {
        url = codexbar_neuralwatt_quota_url_for_testing(invalid[index], &error);
        g_assert_null(url);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_assert_nonnull(strstr(error->message, "NEURALWATT_API_URL"));
        g_clear_error(&error);
    }
    g_setenv("NEURALWATT_API_URL", "https://user@example.com", TRUE);
    provider = codexbar_neuralwatt_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    g_unsetenv("NEURALWATT_API_KEY");
    g_unsetenv("NEURALWATT_API_URL");
}

static void test_status_retry_and_cancellation(void) {
    CodexBarProviderConfig config = {.api_key = "config-key"};
    GError *error = NULL;
    g_setenv("NEURALWATT_API_URL", "https://api.neuralwatt.test", TRUE);
    const long statuses[] = {401, 403, 400, 500};
    const int codes[] = {G_IO_ERROR_PERMISSION_DENIED, G_IO_ERROR_PERMISSION_DENIED, G_IO_ERROR_FAILED, G_IO_ERROR_FAILED};
    for (guint index = 0; index < G_N_ELEMENTS(statuses); index++) {
        reset_fixture();
        fixture.statuses[0] = statuses[index];
        fixture.statuses[1] = statuses[index];
        CodexBarProvider *provider = codexbar_neuralwatt_fetch_with_transport(&config, stub_transport, 1, &error);
        g_assert_null(provider);
        g_assert_error(error, G_IO_ERROR, codes[index]);
        g_assert_cmpuint(fixture.count, ==, statuses[index] == 500 ? 2 : 1);
        g_clear_error(&error);
    }

    reset_fixture();
    fixture.statuses[0] = 503;
    fixture.statuses[1] = 200;
    fixture.retry_after[0] = "0";
    fixture.bodies[1] = minimal_body();
    CodexBarProvider *provider = codexbar_neuralwatt_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 2);
    codexbar_provider_free(provider);

    reset_fixture();
    fixture.network_failure[0] = TRUE;
    fixture.network_failure[1] = TRUE;
    provider = codexbar_neuralwatt_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr(error->message, "Neuralwatt network error"));
    g_clear_error(&error);

    reset_fixture();
    fixture.curl_error[0] = CURLE_GOT_NOTHING;
    fixture.statuses[1] = 200;
    fixture.bodies[1] = minimal_body();
    provider = codexbar_neuralwatt_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 2);
    codexbar_provider_free(provider);

    reset_fixture();
    fixture.curl_error[0] = CURLE_WRITE_ERROR;
    provider = codexbar_neuralwatt_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull(strstr(error->message, "parse Neuralwatt"));
    g_clear_error(&error);

    GCancellable *cancellable = g_cancellable_new();
    reset_fixture();
    fixture.cancellable = cancellable;
    fixture.statuses[0] = 200;
    fixture.bodies[0] = minimal_body();
    fixture.cancel_after[0] = TRUE;
    provider = codexbar_neuralwatt_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(cancellable);

    cancellable = g_cancellable_new();
    reset_fixture();
    fixture.cancellable = cancellable;
    fixture.statuses[0] = 503;
    fixture.retry_after[0] = "10";
    GThread *cancel_thread = g_thread_new("neuralwatt-retry-cancel", delayed_cancel, cancellable);
    provider = codexbar_neuralwatt_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 1, &error);
    g_thread_join(cancel_thread);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpuint(fixture.count, ==, 1);
    g_clear_error(&error);
    g_object_unref(cancellable);
}

static void test_retry_delay(void) {
    CodexBarHttpResponse response = {
        .headers = g_ptr_array_new_with_free_func((GDestroyNotify)response_header_free),
    };
    g_assert_cmpfloat(codexbar_neuralwatt_retry_delay_for_testing(&response), ==, 1.0);
    add_response_header(response.headers, "Retry-After", "20");
    g_assert_cmpfloat(codexbar_neuralwatt_retry_delay_for_testing(&response), ==, 10.0);
    g_ptr_array_set_size(response.headers, 0);
    add_response_header(response.headers, "Retry-After", "date");
    g_assert_cmpfloat(codexbar_neuralwatt_retry_delay_for_testing(&response), ==, 1.0);
    g_ptr_array_free(response.headers, TRUE);
    response.headers = NULL;
    g_assert_cmpfloat(codexbar_neuralwatt_retry_delay_for_testing(&response), ==, 1.0);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/neuralwatt/mapping", test_mapping);
    g_test_add_func("/neuralwatt/sparse-derived", test_sparse_and_derived_values);
    g_test_add_func("/neuralwatt/subscription-variants", test_subscription_variants);
    g_test_add_func("/neuralwatt/malformed", test_malformed_responses);
    g_test_add_func("/neuralwatt/credentials-endpoint-request", test_credentials_endpoint_and_request);
    g_test_add_func("/neuralwatt/status-retry-cancellation", test_status_retry_and_cancellation);
    g_test_add_func("/neuralwatt/retry-delay", test_retry_delay);
    return g_test_run();
}
