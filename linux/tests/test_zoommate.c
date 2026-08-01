#include "zoommate.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

typedef struct {
    const char *urls[4];
    const char *cookies[4];
    long statuses[4];
    const char *bodies[4];
    gboolean network_failure[4];
    gboolean cancel_after[4];
    const char *authorization;
    GCancellable *cancellable;
    guint count;
} TransportFixture;

static TransportFixture fixture;

static const char *canonical_body(void) {
    return "{\"status_code\":200,\"data\":{\"credit_status\":{"
           "\"budget_cap\":200,\"used_credit\":50,\"remaining_credit\":150,"
           "\"overage_credit\":2.5,\"allow_overage\":true,"
           "\"cycle_start_date\":1700000000000,\"cycle_end_date\":1702592000000,"
           "\"is_quota_available\":true,\"is_unlimited\":false}}}";
}

static CodexBarHttpResponse *make_response(long status, const char *body) {
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = status;
    response->body = g_strdup(body ? body : "{}");
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
    g_assert_cmpuint(index, <, G_N_ELEMENTS(fixture.urls));
    g_assert_cmpstr(request->url, ==, fixture.urls[index]);
    g_assert_cmpstr(request->method, ==, "GET");
    g_assert_cmpstr(request_header(request, "Authorization"), ==, fixture.authorization);
    g_assert_cmpstr(request_header(request, "Cookie"), ==, fixture.cookies[index]);
    g_assert_cmpstr(request_header(request, "Origin"), ==, "https://zoommate.zoom.us");
    g_assert_cmpstr(request_header(request, "Referer"), ==, "https://zoommate.zoom.us");
    g_assert_nonnull(request_header(request, "Accept"));
    g_assert_nonnull(request_header(request, "User-Agent"));
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_DENY);
    g_assert_true(request->cancellable == fixture.cancellable);
    if (fixture.cancel_after[index] && fixture.cancellable) g_cancellable_cancel(fixture.cancellable);
    if (fixture.network_failure[index]) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED, "offline");
        return NULL;
    }
    return make_response(fixture.statuses[index], fixture.bodies[index]);
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static void reset_fixture(const char *authorization) {
    memset(&fixture, 0, sizeof(fixture));
    fixture.authorization = authorization;
}

static CodexBarProviderConfig manual_config(const char *capture) {
    CodexBarProviderConfig config = {0};
    config.raw = json_object_new_object();
    json_object_object_add(config.raw, "cookieSource", json_object_new_string("manual"));
    json_object_object_add(config.raw, "cookieHeader", json_object_new_string(capture));
    return config;
}

static void clear_config(CodexBarProviderConfig *config) {
    if (config->raw) json_object_put(config->raw);
    config->raw = NULL;
}

static void test_status_mapping(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_zoommate_parse_usage(canonical_body(), 123000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->provider, ==, "zoommate");
    g_assert_cmpstr(provider->source, ==, "web");
    g_assert_true(provider->explicit_quota_slots);
    g_assert_cmpint(provider->updated_at_ms, ==, 123000);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    const CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpstr(window->id, ==, "primary");
    g_assert_cmpstr(window->title, ==, "Credits");
    g_assert_cmpfloat(window->used_percent, ==, 25.0);
    g_assert_true(window->has_resets_at);
    g_assert_cmpint(window->resets_at_ms, ==, 1702592000000);
    g_assert_cmpstr(window->reset_description, ==, "Credits");
    g_assert_cmpfloat(json_object_get_double(json_object_object_get(provider->usage_extensions, "remainingCredit")),
                      ==,
                      150.0);
    g_assert_true(json_object_get_boolean(json_object_object_get(provider->usage_extensions, "allowOverage")));
    codexbar_provider_free(provider);
}

static CodexBarProvider *parse_or_fail(const char *body) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_zoommate_parse_usage(body, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    return provider;
}

static void test_sparse_unlimited_and_clamping(void) {
    CodexBarProvider *provider = parse_or_fail("{\"data\":{\"credit_status\":{}}}");
    const CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat(window->used_percent, ==, 0.0);
    g_assert_false(window->has_resets_at);
    codexbar_provider_free(provider);

    provider = parse_or_fail(
        "{\"data\":{\"credit_status\":{\"budget_cap\":10,\"used_credit\":20,"
        "\"cycle_end_date\":99,\"is_unlimited\":true}}}");
    window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat(window->used_percent, ==, 0.0);
    g_assert_false(window->has_resets_at);
    codexbar_provider_free(provider);

    provider = parse_or_fail(
        "{\"data\":{\"credit_status\":{\"budget_cap\":10,\"used_credit\":20,"
        "\"cycle_end_date\":99}}}");
    window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat(window->used_percent, ==, 100.0);
    g_assert_true(window->has_resets_at);
    codexbar_provider_free(provider);
}

static void assert_parse_error_bytes(const char *body, size_t length) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_zoommate_parse_usage_bytes(body, length, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void assert_parse_error(const char *body) {
    assert_parse_error_bytes(body, strlen(body));
}

static void test_malformed_status(void) {
    const char *const bodies[] = {
        "{}",
        "{\"data\":null}",
        "{\"data\":{}}",
        "{\"data\":{\"credit_status\":null}}",
        "{\"data\":{\"credit_status\":[]}}",
        "{\"data\":{\"credit_status\":{\"budget_cap\":\"10\"}}}",
        "{\"data\":{\"credit_status\":{\"used_credit\":false}}}",
        "{\"data\":{\"credit_status\":{\"cycle_end_date\":1.5}}}",
        "{\"data\":{\"credit_status\":{\"is_unlimited\":1}}}",
        "{\"data\":{\"credit_status\":{}}} trailing",
    };
    for (guint index = 0; index < G_N_ELEMENTS(bodies); index++) assert_parse_error(bodies[index]);
    const char embedded_nul[] = "{\"data\":{\"credit_status\":{}}}\0tail";
    assert_parse_error_bytes(embedded_nul, sizeof(embedded_nul) - 1);
    const char invalid_utf8[] = "{\"data\":{\"credit_status\":{}}}\xFF";
    assert_parse_error_bytes(invalid_utf8, sizeof(invalid_utf8) - 1);
}

static void test_capture_boundaries(void) {
    const char *const valid[] = {
        "curl 'https://ai.zoom.us/ai-computer/api/v1/credits/status' -H 'Authorization: token'",
        "/usr/bin/curl --url=https://zoommate.zoom.us/ai-computer/api/v1/credits/status "
        "--request GET --header='authorization: Bearer token'",
    };
    for (guint index = 0; index < G_N_ELEMENTS(valid); index++)
        g_assert_true(codexbar_zoommate_capture_is_valid_for_testing(valid[index]));

    const char *const invalid[] = {
        "curl 'http://ai.zoom.us/ai-computer/api/v1/credits/status' -H 'Authorization: token'",
        "curl 'https://evil.example/ai-computer/api/v1/credits/status' -H 'Authorization: token'",
        "curl 'https://notai.zoom.us/ai-computer/api/v1/credits/status' -H 'Authorization: token'",
        "curl 'https://ai.zoom.us:443/ai-computer/api/v1/credits/status' -H 'Authorization: token'",
        "curl 'https://user@ai.zoom.us/ai-computer/api/v1/credits/status' -H 'Authorization: token'",
        "curl 'https://ai.zoom.us/ai-computer/api/v1/credits/status/' -H 'Authorization: token'",
        "curl 'https://ai.zoom.us/ai-computer/api/v1/credits/status?x=1' -H 'Authorization: token'",
        "curl 'https://ai.zoom.us/ai-computer/api/v1/credits/status#x' -H 'Authorization: token'",
        "curl 'https://ai.zoom.us/ai-computer/api/v1/credits/%73tatus' -H 'Authorization: token'",
        "curl 'https://ai.zoom.us/ai-computer/api/v1/credits/status'",
        "curl 'https://ai.zoom.us/ai-computer/api/v1/credits/status' -X POST -H 'Authorization: token'",
        "wget 'https://ai.zoom.us/ai-computer/api/v1/credits/status' -H 'Authorization: token'",
        "curl 'https://ai.zoom.us/ai-computer/api/v1/credits/status' -H 'Authorization: token\nInjected: yes'",
    };
    for (guint index = 0; index < G_N_ELEMENTS(invalid); index++) {
        g_test_message("reject capture %u: %s", index, invalid[index]);
        g_assert_false(codexbar_zoommate_capture_is_valid_for_testing(invalid[index]));
    }
}

static void test_manual_capture_failover_and_cookie_scope(void) {
    reset_fixture("Bearer captured-token");
    fixture.urls[0] = "https://zoommate.zoom.us/ai-computer/api/v1/credits/status";
    fixture.urls[1] = "https://ai.zoom.us/ai-computer/api/v1/credits/status";
    fixture.cookies[0] = "parent=session; mate=only";
    fixture.cookies[1] = NULL;
    fixture.statuses[0] = 503;
    fixture.statuses[1] = 200;
    fixture.bodies[0] = "{}";
    fixture.bodies[1] = canonical_body();
    CodexBarProviderConfig config = manual_config(
        "curl 'https://zoommate.zoom.us/ai-computer/api/v1/credits/status' "
        "-H 'Authorization: captured-token' -H 'Cookie: parent=session; mate=only' "
        "-H 'Origin: https://evil.example' -H 'Referer: https://evil.example/path'");
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_zoommate_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 2);
    codexbar_provider_free(provider);
    clear_config(&config);
}

static void test_token_env_and_failover(void) {
    g_unsetenv("ZOOMMATE_CAPTURE");
    g_setenv("ZOOMMATE_TOKEN", " 'environment-token' ", TRUE);
    g_unsetenv("ZOOMMATE_BEARER_TOKEN");
    reset_fixture("Bearer environment-token");
    fixture.urls[0] = "https://ai.zoom.us/ai-computer/api/v1/credits/status";
    fixture.urls[1] = "https://zoommate.zoom.us/ai-computer/api/v1/credits/status";
    fixture.network_failure[0] = TRUE;
    fixture.statuses[1] = 200;
    fixture.bodies[1] = canonical_body();
    CodexBarProviderConfig config = {0};
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_zoommate_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 2);
    codexbar_provider_free(provider);
    g_unsetenv("ZOOMMATE_TOKEN");
}

static void test_auth_parse_and_missing_errors_stop_failover(void) {
    g_unsetenv("ZOOMMATE_CAPTURE");
    g_unsetenv("ZOOMMATE_TOKEN");
    g_unsetenv("ZOOMMATE_BEARER_TOKEN");
    CodexBarProviderConfig config = {0};
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_zoommate_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

    config = manual_config("bad token with spaces");
    provider = codexbar_zoommate_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    clear_config(&config);

    config = manual_config("token");
    reset_fixture("Bearer token");
    fixture.urls[0] = "https://ai.zoom.us/ai-computer/api/v1/credits/status";
    fixture.statuses[0] = 401;
    fixture.bodies[0] = "{}";
    provider = codexbar_zoommate_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_cmpuint(fixture.count, ==, 1);
    g_clear_error(&error);

    reset_fixture("Bearer token");
    fixture.urls[0] = "https://ai.zoom.us/ai-computer/api/v1/credits/status";
    fixture.statuses[0] = 200;
    fixture.bodies[0] = "{}";
    provider = codexbar_zoommate_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_cmpuint(fixture.count, ==, 1);
    g_clear_error(&error);
    clear_config(&config);
}

static void test_cancellation(void) {
    CodexBarProviderConfig config = manual_config("token");
    GCancellable *cancellable = g_cancellable_new();
    reset_fixture("Bearer token");
    fixture.cancellable = cancellable;
    fixture.urls[0] = "https://ai.zoom.us/ai-computer/api/v1/credits/status";
    fixture.statuses[0] = 200;
    fixture.bodies[0] = canonical_body();
    fixture.cancel_after[0] = TRUE;
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_zoommate_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpuint(fixture.count, ==, 1);
    g_clear_error(&error);
    g_object_unref(cancellable);
    clear_config(&config);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/zoommate/status-mapping", test_status_mapping);
    g_test_add_func("/zoommate/sparse-unlimited-clamping", test_sparse_unlimited_and_clamping);
    g_test_add_func("/zoommate/malformed-status", test_malformed_status);
    g_test_add_func("/zoommate/capture-boundaries", test_capture_boundaries);
    g_test_add_func("/zoommate/manual-failover-cookie-scope", test_manual_capture_failover_and_cookie_scope);
    g_test_add_func("/zoommate/token-env-failover", test_token_env_and_failover);
    g_test_add_func("/zoommate/auth-parse-stop", test_auth_parse_and_missing_errors_stop_failover);
    g_test_add_func("/zoommate/cancellation", test_cancellation);
    return g_test_run();
}
