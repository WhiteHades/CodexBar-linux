#include "deepinfra.h"
#include "render.h"

#include <curl/curl.h>
#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

typedef struct {
    const char *urls[4];
    long statuses[4];
    const char *bodies[4];
    const char *retry_after[4];
    gboolean cancelled[4];
    gboolean network_failure[4];
    gboolean curl_failure[4];
    int network_error_codes[4];
    gboolean cancel_after[4];
    gboolean cancel_during_retry[4];
    guint count;
    const char *authorization;
    GCancellable *cancellable;
    GThread *cancel_thread;
} TransportFixture;

static TransportFixture fixture;

static void response_header_free(gpointer data) {
    CodexBarHttpResponseHeader *header = data;
    g_free(header->name);
    g_free(header->value);
    g_free(header);
}

static CodexBarHttpResponse *make_response(long status, const char *body, const char *retry_after) {
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = status;
    response->body = g_strdup(body ? body : "");
    response->body_length = strlen(response->body);
    response->headers = g_ptr_array_new_with_free_func(response_header_free);
    if (retry_after) {
        CodexBarHttpResponseHeader *header = g_new0(CodexBarHttpResponseHeader, 1);
        header->name = g_strdup("Retry-After");
        header->value = g_strdup(retry_after);
        g_ptr_array_add(response->headers, header);
    }
    return response;
}

static const char *request_header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static gpointer delayed_cancel(gpointer data) {
    g_usleep(50000);
    g_cancellable_cancel(data);
    return NULL;
}

static CodexBarHttpResponse *stub_transport(const CodexBarHttpRequest *request, GError **error) {
    guint index = fixture.count++;
    g_assert_cmpuint(index, <, G_N_ELEMENTS(fixture.urls));
    g_assert_cmpstr(request->url, ==, fixture.urls[index]);
    g_assert_cmpstr(request->method, ==, "GET");
    g_assert_cmpstr(request_header(request, "Authorization"), ==, fixture.authorization);
    g_assert_cmpstr(request_header(request, "Accept"), ==, "application/json");
    g_assert_cmpint(request->timeout_seconds, ==, 30);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == fixture.cancellable);
    if (fixture.cancelled[index]) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
        return NULL;
    }
    if (fixture.network_failure[index]) {
        g_set_error_literal(error, G_IO_ERROR, fixture.network_error_codes[index], "connection failed");
        return NULL;
    }
    if (fixture.curl_failure[index]) {
        g_set_error_literal(
            error, g_quark_from_static_string("codexbar-http-error"), fixture.network_error_codes[index], "partial response");
        return NULL;
    }
    CodexBarHttpResponse *response =
        make_response(fixture.statuses[index], fixture.bodies[index], fixture.retry_after[index]);
    if (fixture.cancel_after[index]) g_cancellable_cancel(fixture.cancellable);
    if (fixture.cancel_during_retry[index]) {
        fixture.cancel_thread = g_thread_new("deepinfra-retry-cancel", delayed_cancel, fixture.cancellable);
    }
    return response;
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static const char *confidence(const CodexBarProvider *provider) {
    json_object *value = NULL;
    g_assert_true(json_object_object_get_ex(provider->usage_extensions, "dataConfidence", &value));
    return json_object_get_string(value);
}

static const char *canonical_checklist(void) {
    return "{\"stripe_balance\":-99.75,\"recent\":3.94,\"limit\":20,"
           "\"suspended\":false,\"suspend_reason\":null}";
}

static const char *canonical_usage(void) {
    return "{\"months\":[{\"period\":\"2026.07\",\"items\":[],\"total_cost\":394}],"
           "\"initial_month\":\"2026.07\"}";
}

static void test_prepaid_mapping(void) {
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_deepinfra_parse_usage(canonical_checklist(), canonical_usage(), 1700000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->provider, ==, "deepinfra");
    g_assert_cmpstr(provider->source, ==, "api");
    g_assert_nonnull(provider->identity);
    g_assert_cmpstr(confidence(provider), ==, "exact");
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpstr(window->id, ==, "primary");
    g_assert_cmpfloat(window->used_percent, ==, 0.0);
    g_assert_cmpstr(window->reset_description, ==, "$95.81 available · $3.94 spent this month");
    g_assert_nonnull(provider->provider_cost);
    g_assert_cmpfloat_with_epsilon(provider->provider_cost->used, 3.94, 0.000001);
    g_assert_cmpfloat(provider->provider_cost->limit, ==, 20.0);
    g_assert_cmpstr(provider->provider_cost->currency, ==, "USD");
    g_assert_cmpstr(provider->provider_cost->period, ==, "Billing cycle");
    g_assert_cmpint(provider->provider_cost->updated_at_ms, ==, G_GINT64_CONSTANT(1700000000000));
    CodexBarSnapshot *snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    g_ptr_array_add(snapshot->providers, provider);
    char *rendered = codexbar_render_usage_json(snapshot, FALSE);
    json_object *array = json_tokener_parse(rendered);
    json_object *payload = json_object_array_get_idx(array, 0);
    json_object *usage = NULL;
    json_object *primary = NULL;
    json_object *secondary = NULL;
    json_object *identity = NULL;
    json_object *data_confidence = NULL;
    g_assert_true(json_object_object_get_ex(payload, "usage", &usage));
    g_assert_true(json_object_object_get_ex(usage, "primary", &primary));
    g_assert_true(json_object_is_type(primary, json_type_object));
    g_assert_true(json_object_object_get_ex(usage, "secondary", &secondary));
    g_assert_true(json_object_is_type(secondary, json_type_null));
    g_assert_true(json_object_object_get_ex(usage, "identity", &identity));
    g_assert_cmpstr(json_object_get_string(json_object_object_get(identity, "providerID")), ==, "deepinfra");
    g_assert_true(json_object_object_get_ex(usage, "dataConfidence", &data_confidence));
    g_assert_cmpstr(json_object_get_string(data_confidence), ==, "exact");
    json_object_put(array);
    g_free(rendered);
    codexbar_snapshot_free(snapshot);
}

static void test_owed_suspended_and_empty_months(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_deepinfra_parse_usage(
        "{\"stripe_balance\":2.75,\"recent\":7,\"limit\":-1}",
        "{\"months\":[{\"period\":\"2026.07\",\"total_cost\":650}]}",
        1,
        &error);
    g_assert_no_error(error);
    CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat(window->used_percent, ==, 100.0);
    g_assert_cmpstr(window->reset_description, ==, "$9.75 owed · $6.50 spent this month");
    g_assert_null(provider->provider_cost);
    codexbar_provider_free(provider);

    provider = codexbar_deepinfra_parse_usage(
        "{\"stripe_balance\":-5,\"recent\":1,\"suspended\":true,"
        "\"suspend_reason\":\" Payment review \"}",
        "{\"months\":[]}",
        1,
        &error);
    g_assert_no_error(error);
    window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat(window->used_percent, ==, 100.0);
    g_assert_cmpstr(
        window->reset_description, ==, "Suspended: Payment review · $4.00 available · $1.00 spent this month");
    codexbar_provider_free(provider);

    provider = codexbar_deepinfra_parse_usage(
        "{\"stripe_balance\":-5,\"recent\":1,\"suspended\":true,\"suspend_reason\":\"  \"}",
        "{\"months\":[]}",
        1,
        &error);
    g_assert_no_error(error);
    window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpstr(window->reset_description, ==, "Suspended · $4.00 available · $1.00 spent this month");
    codexbar_provider_free(provider);

    provider = codexbar_deepinfra_parse_usage(
        "{\"stripe_balance\":0,\"recent\":0}", "{\"months\":[]}", 1, &error);
    g_assert_no_error(error);
    window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpstr(window->reset_description, ==, "$0.00 available · $0.00 spent this month");
    codexbar_provider_free(provider);

    provider = codexbar_deepinfra_parse_usage(
        "{\"stripe_balance\":-5,\"recent\":1}",
        "{\"months\":[{\"period\":\"2026.07\",\"total_cost\":0}]}",
        1,
        &error);
    g_assert_no_error(error);
    window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpstr(window->reset_description, ==, "$4.00 available · $0.00 spent this month");
    codexbar_provider_free(provider);
}

static void assert_parse_error(const char *checklist, const char *usage) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_deepinfra_parse_usage(checklist, usage, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void assert_parse_bytes_error(const char *checklist, size_t checklist_length) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_deepinfra_parse_usage_bytes(
        checklist, checklist_length, canonical_usage(), strlen(canonical_usage()), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_parse_errors(void) {
    assert_parse_error("{}", canonical_usage());
    assert_parse_error("{\"stripe_balance\":-1,\"recent\":\"1\"}", canonical_usage());
    assert_parse_error("{\"stripe_balance\":-1,\"recent\":1,\"suspended\":1}", canonical_usage());
    assert_parse_error("{\"stripe_balance\":-1,\"recent\":1,\"limit\":\"20\"}", canonical_usage());
    assert_parse_error(
        "{\"stripe_balance\":-1,\"recent\":1,\"suspend_reason\":1}", canonical_usage());
    assert_parse_error(canonical_checklist(), "{}");
    assert_parse_error(canonical_checklist(), "{\"months\":[{\"period\":1,\"total_cost\":1}]}");
    assert_parse_error(canonical_checklist(),
                       "{\"months\":[{\"period\":\"bad\"},{\"period\":\"ok\",\"total_cost\":1}]}");
    assert_parse_error(canonical_checklist(), "{\"months\":[],\"initial_month\":1}");
    assert_parse_error(canonical_checklist(), "{\"months\":[]} trailing");
    static const char embedded_nul[] = "{\"stripe_balance\":-1,\"recent\":1}\0trailing";
    assert_parse_bytes_error(embedded_nul, sizeof(embedded_nul) - 1);
    static const char vertical_tab[] = "{\"stripe_balance\":-1,\"recent\":1}\v";
    assert_parse_bytes_error(vertical_tab, sizeof(vertical_tab) - 1);
    static const char form_feed[] = "{\"stripe_balance\":-1,\"recent\":1}\f";
    assert_parse_bytes_error(form_feed, sizeof(form_feed) - 1);
}

static void reset_fixture(const char *authorization) {
    memset(&fixture, 0, sizeof(fixture));
    fixture.authorization = authorization;
}

static void test_fetch_sequence_and_credentials(void) {
    g_setenv("DEEPINFRA_API_KEY", "environment", TRUE);
    g_setenv("DEEPINFRA_TOKEN", "alternate", TRUE);
    reset_fixture("Bearer config-key");
    fixture.urls[0] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.urls[1] = "https://api.deepinfra.com/payment/usage?from=current";
    fixture.statuses[0] = 200;
    fixture.statuses[1] = 200;
    fixture.bodies[0] = canonical_checklist();
    fixture.bodies[1] = canonical_usage();
    CodexBarProviderConfig config = {.api_key = " 'config-key' "};
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_deepinfra_fetch_with_transport(&config, stub_transport, 123000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 2);
    codexbar_provider_free(provider);

    config.api_key = NULL;
    g_setenv("DEEPINFRA_API_KEY", " 'primary' ", TRUE);
    reset_fixture("Bearer primary");
    fixture.urls[0] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.urls[1] = "https://api.deepinfra.com/payment/usage?from=current";
    fixture.statuses[0] = 200;
    fixture.statuses[1] = 200;
    fixture.bodies[0] = canonical_checklist();
    fixture.bodies[1] = canonical_usage();
    provider = codexbar_deepinfra_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    codexbar_provider_free(provider);

    g_unsetenv("DEEPINFRA_API_KEY");
    reset_fixture("Bearer alternate");
    fixture.urls[0] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.urls[1] = "https://api.deepinfra.com/payment/usage?from=current";
    fixture.statuses[0] = 200;
    fixture.statuses[1] = 200;
    fixture.bodies[0] = canonical_checklist();
    fixture.bodies[1] = canonical_usage();
    provider = codexbar_deepinfra_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    codexbar_provider_free(provider);
    g_unsetenv("DEEPINFRA_TOKEN");
}

static void test_fetch_errors_and_retry(void) {
    g_unsetenv("DEEPINFRA_API_KEY");
    g_unsetenv("DEEPINFRA_TOKEN");
    CodexBarProviderConfig config = {0};
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_deepinfra_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

    config.api_key = "key";
    reset_fixture("Bearer key");
    fixture.urls[0] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.statuses[0] = 401;
    fixture.bodies[0] = "{}";
    provider = codexbar_deepinfra_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_cmpuint(fixture.count, ==, 1);
    g_clear_error(&error);

    reset_fixture("Bearer key");
    fixture.urls[0] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.statuses[0] = 403;
    fixture.bodies[0] = "{}";
    provider = codexbar_deepinfra_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_nonnull(strstr(error->message, "billing data"));
    g_clear_error(&error);

    reset_fixture("Bearer key");
    fixture.urls[0] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.statuses[0] = 418;
    fixture.bodies[0] = "{}";
    provider = codexbar_deepinfra_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr(error->message, "HTTP 418"));
    g_clear_error(&error);

    reset_fixture("Bearer key");
    fixture.urls[0] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.urls[1] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.urls[2] = "https://api.deepinfra.com/payment/usage?from=current";
    fixture.statuses[0] = 503;
    fixture.statuses[1] = 200;
    fixture.statuses[2] = 200;
    fixture.bodies[0] = "{}";
    fixture.bodies[1] = canonical_checklist();
    fixture.bodies[2] = canonical_usage();
    fixture.retry_after[0] = "0";
    provider = codexbar_deepinfra_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 3);
    codexbar_provider_free(provider);

    reset_fixture("Bearer key");
    fixture.urls[0] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.urls[1] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.urls[2] = "https://api.deepinfra.com/payment/usage?from=current";
    fixture.curl_failure[0] = TRUE;
    fixture.network_error_codes[0] = CURLE_PARTIAL_FILE;
    fixture.statuses[1] = 200;
    fixture.statuses[2] = 200;
    fixture.bodies[1] = canonical_checklist();
    fixture.bodies[2] = canonical_usage();
    provider = codexbar_deepinfra_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 3);
    codexbar_provider_free(provider);

    reset_fixture("Bearer key");
    fixture.urls[0] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.cancelled[0] = TRUE;
    provider = codexbar_deepinfra_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);

    reset_fixture("Bearer key");
    fixture.urls[0] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.network_failure[0] = TRUE;
    fixture.network_error_codes[0] = G_IO_ERROR_FAILED;
    provider = codexbar_deepinfra_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr(error->message, "DeepInfra network error"));
    g_clear_error(&error);

    reset_fixture("Bearer key");
    fixture.urls[0] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.urls[1] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.urls[2] = "https://api.deepinfra.com/payment/usage?from=current";
    fixture.network_failure[0] = TRUE;
    fixture.network_error_codes[0] = G_IO_ERROR_CONNECTION_REFUSED;
    fixture.statuses[1] = 200;
    fixture.statuses[2] = 200;
    fixture.bodies[1] = canonical_checklist();
    fixture.bodies[2] = canonical_usage();
    provider = codexbar_deepinfra_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 3);
    codexbar_provider_free(provider);

    reset_fixture("Bearer key");
    fixture.urls[0] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.urls[1] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.statuses[0] = 503;
    fixture.statuses[1] = 503;
    fixture.bodies[0] = "{}";
    fixture.bodies[1] = "{}";
    fixture.retry_after[0] = "0";
    provider = codexbar_deepinfra_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_cmpuint(fixture.count, ==, 2);
    g_clear_error(&error);

    CodexBarHttpResponse *response = make_response(503, "{}", NULL);
    g_assert_cmpfloat(codexbar_deepinfra_retry_delay_for_testing(response), ==, 1.0);
    codexbar_http_response_free(response);
    response = make_response(503, "{}", "60");
    g_assert_cmpfloat(codexbar_deepinfra_retry_delay_for_testing(response), ==, 10.0);
    codexbar_http_response_free(response);
}

static void test_real_cancellation(void) {
    CodexBarProviderConfig config = {.api_key = "key"};
    GCancellable *cancellable = g_cancellable_new();
    reset_fixture("Bearer key");
    fixture.cancellable = cancellable;
    fixture.urls[0] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.statuses[0] = 200;
    fixture.bodies[0] = canonical_checklist();
    fixture.cancel_after[0] = TRUE;
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_deepinfra_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpuint(fixture.count, ==, 1);
    g_clear_error(&error);

    g_cancellable_reset(cancellable);
    reset_fixture("Bearer key");
    fixture.cancellable = cancellable;
    fixture.urls[0] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.statuses[0] = 503;
    fixture.bodies[0] = "{}";
    fixture.retry_after[0] = "10";
    fixture.cancel_during_retry[0] = TRUE;
    gint64 started = g_get_monotonic_time();
    provider = codexbar_deepinfra_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 1, &error);
    gint64 elapsed = g_get_monotonic_time() - started;
    g_thread_join(fixture.cancel_thread);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpuint(fixture.count, ==, 1);
    g_assert_cmpint(elapsed, <, G_USEC_PER_SEC);
    g_clear_error(&error);
    g_object_unref(cancellable);
}

static void test_malformed_checklist_still_fetches_usage(void) {
    reset_fixture("Bearer key");
    fixture.urls[0] = "https://api.deepinfra.com/payment/checklist?compute_owed=true";
    fixture.urls[1] = "https://api.deepinfra.com/payment/usage?from=current";
    fixture.statuses[0] = 200;
    fixture.statuses[1] = 200;
    fixture.bodies[0] = "{}";
    fixture.bodies[1] = canonical_usage();
    CodexBarProviderConfig config = {.api_key = "key"};
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_deepinfra_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_cmpuint(fixture.count, ==, 2);
    g_clear_error(&error);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/deepinfra/prepaid-mapping", test_prepaid_mapping);
    g_test_add_func("/deepinfra/owed-suspended-empty", test_owed_suspended_and_empty_months);
    g_test_add_func("/deepinfra/parse-errors", test_parse_errors);
    g_test_add_func("/deepinfra/fetch-sequence-credentials", test_fetch_sequence_and_credentials);
    g_test_add_func("/deepinfra/fetch-errors-retry", test_fetch_errors_and_retry);
    g_test_add_func("/deepinfra/real-cancellation", test_real_cancellation);
    g_test_add_func("/deepinfra/malformed-checklist-sequence", test_malformed_checklist_still_fetches_usage);
    return g_test_run();
}
