#include "wayfinder.h"

#include "provider_registry.h"
#include "render.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

static const char *health_ok = "{\"status\":\"ok\",\"models\":[\"cloud\",\"local\"],\"offline\":false}";
static const char *models =
    "{\"models\":[{\"name\":\"local\"},{\"name\":\"cloud\"}],\"dry_run\":false}";
static const char *savings =
    "{\"priced\":true,\"requests\":14,\"tokens\":1028,\"realized\":0.003558,"
    "\"baseline\":0.009252,\"saved\":0.005694,\"saved_pct\":61.5,\"by_route\":{"
    "\"cloud\":{\"requests\":4,\"saved\":0.0,\"tokens\":366},"
    "\"local\":{\"requests\":10,\"saved\":0.005694,\"tokens\":662}}}";
static const char *metrics =
    "wayfinder_router_decision_latency_seconds_sum 0.00112602\n"
    "wayfinder_router_decision_latency_seconds_count 14\n";

typedef struct {
    char *base;
    guint count;
    long failure_status;
    guint fail_at;
    gboolean redirect;
    gboolean cancel;
    GCancellable *cancellable;
    const char *body_override;
    size_t body_override_length;
    guint body_override_at;
} WayfinderFixture;

static WayfinderFixture fixture;

static void reset_fixture(void) {
    memset(&fixture, 0, sizeof(fixture));
    fixture.base = "http://127.0.0.1:8088";
}

static CodexBarHttpResponse *stub_transport(const CodexBarHttpRequest *request, GError **error) {
    static const char *paths[] = {"/healthz", "/router/models", "/v1/savings?period=30d", "/metrics"};
    static const char *bodies[] = {NULL, NULL, NULL, NULL};
    guint index = fixture.count++;
    g_assert_cmpuint(index, <, G_N_ELEMENTS(paths));
    char *expected = g_strdup_printf("%s%s", fixture.base, paths[index]);
    g_assert_cmpstr(request->url, ==, expected);
    g_free(expected);
    g_assert_cmpstr(request->method, ==, "GET");
    g_assert_cmpuint(request->timeout_seconds, ==, 5);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpuint(request->header_count, ==, 0);
    g_assert_null(request->body);
    g_assert_true(request->cancellable == fixture.cancellable);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP);
    GUri *base_uri = g_uri_parse(fixture.base, G_URI_FLAGS_NONE, NULL);
    gboolean https = base_uri && g_ascii_strcasecmp(g_uri_get_scheme(base_uri), "https") == 0;
    if (base_uri) g_uri_unref(base_uri);
    g_assert_cmpint(request->redirect_policy,
                    ==,
                    https ? CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN : CODEXBAR_HTTP_REDIRECT_DENY);
    if (fixture.cancel && index == fixture.fail_at) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
        return NULL;
    }
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = fixture.fail_at == index && fixture.failure_status > 0 ? fixture.failure_status : 200;
    const char *body = bodies[index];
    if (!body) body = index == 0 ? health_ok : index == 1 ? models : index == 2 ? savings : metrics;
    if (fixture.body_override && fixture.body_override_at == index) {
        response->body_length = fixture.body_override_length;
        response->body = g_malloc(response->body_length + 1);
        memcpy(response->body, fixture.body_override, response->body_length);
        response->body[response->body_length] = '\0';
    } else {
        response->body = g_strdup(body);
        response->body_length = strlen(body);
    }
    response->effective_url = fixture.redirect ? g_strdup("http://attacker.test/healthz") : g_strdup(request->url);
    return response;
}

static json_object *wayfinder_usage(const CodexBarProvider *provider) {
    json_object *usage = NULL;
    g_assert_nonnull(provider->usage_extensions);
    g_assert_true(json_object_object_get_ex(provider->usage_extensions, "wayfinderUsage", &usage));
    return usage;
}

static void test_endpoint_precedence_and_security(void) {
    g_setenv("WAYFINDER_GATEWAY_URL", " https://environment.example.com/wf/ ", TRUE);
    CodexBarProviderConfig config = {.enterprise_host = " 'http://localhost:9099/base' "};
    GError *error = NULL;
    char *base = codexbar_wayfinder_base_url_for_testing(&config, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(base, ==, "http://localhost:9099/base");
    g_free(base);

    config.enterprise_host = NULL;
    base = codexbar_wayfinder_base_url_for_testing(&config, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(base, ==, "https://environment.example.com/wf/");
    g_free(base);
    g_unsetenv("WAYFINDER_GATEWAY_URL");

    base = codexbar_wayfinder_base_url_for_testing(&config, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(base, ==, "http://127.0.0.1:8088");
    g_free(base);

    char *invalid[] = {
        "http://192.168.1.5:8088",
        "http://user@127.0.0.1:8088",
        "http://127.0.0.+1:8088",
        "https://user:password@example.com",
        "https://example.com%2f.attacker.test",
        "https://example.com\\attacker.test",
        "bad\nhost",
    };
    for (guint index = 0; index < G_N_ELEMENTS(invalid); index++) {
        config.enterprise_host = invalid[index];
        base = codexbar_wayfinder_base_url_for_testing(&config, &error);
        g_assert_null(base);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_assert_nonnull(strstr(error->message, "WAYFINDER_GATEWAY_URL"));
        g_clear_error(&error);
    }
}

static void test_endpoint_urls(void) {
    GError *error = NULL;
    char *url = codexbar_wayfinder_endpoint_for_testing("http://127.0.0.1:8088/", "healthz", NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "http://127.0.0.1:8088/healthz");
    g_free(url);

    url = codexbar_wayfinder_endpoint_for_testing(
        "https://wayfinder.example.com/wf?old=1#fragment", "v1/savings", "period=30d", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "https://wayfinder.example.com/wf/v1/savings?period=30d#fragment");
    g_free(url);

    CodexBarProviderConfig config = {.enterprise_host = "https://wayfinder.example.com/wf/?old=1#other"};
    url = codexbar_wayfinder_dashboard_url_for_testing(&config, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "https://wayfinder.example.com/wf/router");
    g_free(url);
}

static void test_metrics_best_effort(void) {
    gboolean present = FALSE;
    double value = codexbar_wayfinder_average_decision_ms_for_testing(metrics, &present);
    g_assert_true(present);
    g_assert_cmpfloat_with_epsilon(value, 0.08043, 0.0001);

    value = codexbar_wayfinder_average_decision_ms_for_testing(
        "wayfinder_router_decision_latency_seconds_sum{route=\"all\"} 2.0\n"
        "wayfinder_router_decision_latency_seconds_count{route=\"all\"} 4\n",
        &present);
    g_assert_true(present);
    g_assert_cmpfloat(value, ==, 500.0);

    value = codexbar_wayfinder_average_decision_ms_for_testing(
        "wayfinder_router_decision_latency_seconds_sum{route=\"local\"} 9\n"
        "wayfinder_router_decision_latency_seconds_count{route=\"cloud\"} 2\n"
        "wayfinder_router_decision_latency_seconds_sum{route=\"all\"} 2\n"
        "wayfinder_router_decision_latency_seconds_count{route=\"all\"} 4\n",
        &present);
    g_assert_true(present);
    g_assert_cmpfloat(value, ==, 500.0);

    value = codexbar_wayfinder_average_decision_ms_for_testing(
        "wayfinder_router_decision_latency_seconds_sum 1e308\n"
        "wayfinder_router_decision_latency_seconds_count 1\n",
        &present);
    g_assert_false(present);
    g_assert_cmpfloat(value, ==, 0.0);

    value = codexbar_wayfinder_average_decision_ms_for_testing(
        "wayfinder_router_decision_latency_seconds_sum 99\n"
        "wayfinder_router_decision_latency_seconds_sum{route=\"all\"} 2\n"
        "wayfinder_router_decision_latency_seconds_count{route=\"all\"} 4\n",
        &present);
    g_assert_true(present);
    g_assert_cmpfloat(value, ==, 500.0);

    value = codexbar_wayfinder_average_decision_ms_for_testing(
        "wayfinder_router_decision_latency_seconds_sum -1\n"
        "wayfinder_router_decision_latency_seconds_count 4\n",
        &present);
    g_assert_false(present);
    g_assert_cmpfloat(value, ==, 0.0);

    value = codexbar_wayfinder_average_decision_ms_for_testing(
        "wayfinder_router_decision_latency_seconds_sum 2.0   \n"
        "wayfinder_router_decision_latency_seconds_count 4\t \n",
        &present);
    g_assert_true(present);
    g_assert_cmpfloat(value, ==, 500.0);

    value = codexbar_wayfinder_average_decision_ms_for_testing("garbage\n", &present);
    g_assert_false(present);
    g_assert_cmpfloat(value, ==, 0.0);
}

static void test_status_and_summary_variants(void) {
    const char *degraded =
        "{\"status\":\"degraded\",\"offline\":false,\"missing_keys\":[\"cloud\"]}";
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_wayfinder_parse_usage(degraded, models, savings, NULL, 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->identity->login_method, ==, "Degraded — 1 key missing");
    codexbar_provider_free(provider);

    const char *zero =
        "{\"priced\":true,\"requests\":0,\"tokens\":0,\"realized\":0,\"baseline\":0,"
        "\"saved\":0,\"saved_pct\":0,\"by_route\":{}}";
    provider = codexbar_wayfinder_parse_usage(health_ok, models, zero, NULL, 1000, &error);
    g_assert_no_error(error);
    char *text = codexbar_render_wayfinder_usage(provider);
    g_assert_null(strstr(text, "Routed:"));
    g_assert_null(strstr(text, "Saved:"));
    g_free(text);
    codexbar_provider_free(provider);

    const char *unpriced =
        "{\"priced\":false,\"requests\":5,\"tokens\":420,\"realized\":1.8,\"baseline\":3,"
        "\"saved\":1.2,\"saved_pct\":40,\"by_route\":{"
        "\"groq-8b\":{\"requests\":4,\"saved\":1.2,\"tokens\":320},"
        "\"openai-o1\":{\"requests\":1,\"saved\":0,\"tokens\":100}}}";
    provider = codexbar_wayfinder_parse_usage(health_ok, models, unpriced, NULL, 1000, &error);
    g_assert_no_error(error);
    text = codexbar_render_wayfinder_usage(provider);
    g_assert_nonnull(strstr(text, "Routed: groq-8b: 4 · openai-o1: 1"));
    g_assert_nonnull(strstr(text, "Saved: 40% vs highest-cost route"));
    g_assert_null(strstr(text, "$"));
    g_free(text);
    codexbar_provider_free(provider);

    const char *offline_models = "{\"models\":[{\"name\":\"local\"}],\"dry_run\":true}";
    const char *offline = "{\"status\":\"ok\",\"offline\":true}";
    provider = codexbar_wayfinder_parse_usage(offline, offline_models, zero, NULL, 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->identity->organization, ==, "1 model · local gateway");
    g_assert_cmpstr(provider->identity->login_method, ==, "Offline mode");
    text = codexbar_render_wayfinder_usage(provider);
    g_assert_nonnull(strstr(text, "Gateway: ok · 1 model · offline · dry run"));
    g_free(text);
    codexbar_provider_free(provider);
}

static void test_snapshot_mapping_and_rendering(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_wayfinder_parse_usage(
        health_ok, models, savings, metrics, g_get_real_time() / 1000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->provider, ==, "wayfinder");
    g_assert_cmpstr(provider->source, ==, "api");
    g_assert_cmpuint(provider->quota_windows->len, ==, 0);
    g_assert_null(provider->provider_cost);
    g_assert_cmpstr(provider->identity->organization, ==, "2 models · local gateway");
    g_assert_cmpstr(provider->identity->login_method, ==, "Local gateway");
    json_object *usage = wayfinder_usage(provider);
    g_assert_cmpint(json_object_get_int(json_object_object_get(usage, "requests")), ==, 14);
    g_assert_cmpfloat(json_object_get_double(json_object_object_get(usage, "savedPct")), ==, 61.5);
    json_object *routes = json_object_object_get(usage, "routes");
    g_assert_cmpstr(json_object_get_string(json_object_object_get(json_object_array_get_idx(routes, 0), "name")),
                    ==,
                    "local");

    CodexBarSnapshot snapshot = {.providers = g_ptr_array_new()};
    g_ptr_array_add(snapshot.providers, provider);
    char *text = codexbar_render_usage_text(&snapshot);
    g_assert_nonnull(strstr(text, "Gateway: ok · 2 models"));
    g_assert_nonnull(strstr(text, "Routed: local: 10 · cloud: 4"));
    g_assert_nonnull(strstr(text, "Saved: <$0.01 · 61.5% vs highest-cost route"));
    g_assert_nonnull(strstr(text, "Avg decision: 0.1 ms"));
    g_assert_null(strstr(text, "Cost:"));
    g_free(text);

    char *waybar = codexbar_render_waybar(&snapshot);
    json_object *waybar_json = json_tokener_parse(waybar);
    g_assert_nonnull(waybar_json);
    const char *tooltip = json_object_get_string(json_object_object_get(waybar_json, "tooltip"));
    const char *waybar_text = json_object_get_string(json_object_object_get(waybar_json, "text"));
    g_assert_null(strstr(waybar_text, "%"));
    g_assert_nonnull(strstr(tooltip, "updated just now\n  Gateway: ok · 2 models"));
    json_object_put(waybar_json);
    g_free(waybar);

    char *json = codexbar_render_usage_json(&snapshot, FALSE);
    g_assert_nonnull(strstr(json, "\"wayfinderUsage\""));
    g_assert_nonnull(strstr(json, "\"dataConfidence\":\"exact\""));
    CodexBarSnapshot *decoded = codexbar_snapshot_parse(json, &error);
    g_assert_no_error(error);
    g_assert_nonnull(decoded);
    g_assert_cmpuint(decoded->providers->len, ==, 1);
    g_assert_nonnull(wayfinder_usage(g_ptr_array_index(decoded->providers, 0)));
    codexbar_snapshot_free(decoded);
    g_free(json);
    g_ptr_array_free(snapshot.providers, TRUE);
    codexbar_provider_free(provider);
}

static void test_fetch_sequence_optional_metrics_and_errors(void) {
    reset_fixture();
    CodexBarProviderConfig config = {0};
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_wayfinder_fetch_with_transport(&config, stub_transport, 1000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 4);
    g_assert_cmpstr(provider->dashboard_url, ==, "http://127.0.0.1:8088/router");
    codexbar_provider_free(provider);

    reset_fixture();
    fixture.base = "https://wayfinder.example.com/base";
    config.enterprise_host = "Https://wayfinder.example.com/base";
    provider = codexbar_wayfinder_fetch_with_transport(&config, stub_transport, 1000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->dashboard_url, ==, "https://wayfinder.example.com/base/router");
    codexbar_provider_free(provider);
    config.enterprise_host = NULL;

    reset_fixture();
    fixture.fail_at = 3;
    fixture.failure_status = 500;
    provider = codexbar_wayfinder_fetch_with_transport(&config, stub_transport, 1000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_null(json_object_object_get(wayfinder_usage(provider), "avgDecisionMs"));
    codexbar_provider_free(provider);

    reset_fixture();
    fixture.fail_at = 0;
    fixture.failure_status = 500;
    provider = codexbar_wayfinder_fetch_with_transport(&config, stub_transport, 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr(error->message, "HTTP 500"));
    g_clear_error(&error);

    reset_fixture();
    const char embedded_nul[] = {'{', '}', '\0', '{', '}'};
    fixture.body_override = embedded_nul;
    fixture.body_override_length = sizeof(embedded_nul);
    fixture.body_override_at = 0;
    provider = codexbar_wayfinder_fetch_with_transport(&config, stub_transport, 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);

    reset_fixture();
    fixture.redirect = TRUE;
    provider = codexbar_wayfinder_fetch_with_transport(&config, stub_transport, 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
}

static void test_cancellation_and_registry(void) {
    reset_fixture();
    CodexBarProviderConfig config = {0};
    GCancellable *cancellable = g_cancellable_new();
    fixture.cancellable = cancellable;
    fixture.cancel = TRUE;
    fixture.fail_at = 3;
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_wayfinder_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);

    reset_fixture();
    fixture.cancellable = cancellable;
    fixture.cancel = TRUE;
    fixture.fail_at = 0;
    provider = codexbar_wayfinder_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(cancellable);

    const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find("wayfinder-router");
    g_assert_nonnull(descriptor);
    g_assert_cmpint(descriptor->native_provider, ==, CODEXBAR_NATIVE_WAYFINDER);
    g_assert_true(codexbar_provider_supports_source(descriptor, "auto"));
    g_assert_true(codexbar_provider_supports_source(descriptor, "api"));
    g_assert_false(codexbar_provider_supports_config_api_key(descriptor));
}

static void test_malformed_payloads(void) {
    const char *invalid[] = {
        "{}",
        "{\"status\":\"ok\",\"offline\":0}",
        "{\"status\":\"bad\\u001bstatus\",\"offline\":false}",
    };
    for (guint index = 0; index < G_N_ELEMENTS(invalid); index++) {
        GError *error = NULL;
        CodexBarProvider *provider =
            codexbar_wayfinder_parse_usage(invalid[index], models, savings, metrics, 1000, &error);
        g_assert_null(provider);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&error);
    }

    GError *error = NULL;
    CodexBarProvider *provider = codexbar_wayfinder_parse_usage(
        health_ok, "{\"models\":[],\"dry_run\":0}", savings, metrics, 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);

    provider = codexbar_wayfinder_parse_usage(
        health_ok, models, "{\"priced\":true}", metrics, 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);

    provider = codexbar_wayfinder_parse_usage(
        health_ok,
        models,
        "{\"priced\":true,\"requests\":-9223372036854775808,\"tokens\":0,\"realized\":0,"
        "\"baseline\":0,\"saved\":0,\"saved_pct\":0,\"by_route\":{}}",
        metrics,
        1000,
        &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/wayfinder/endpoint-precedence-security", test_endpoint_precedence_and_security);
    g_test_add_func("/wayfinder/endpoint-urls", test_endpoint_urls);
    g_test_add_func("/wayfinder/metrics-best-effort", test_metrics_best_effort);
    g_test_add_func("/wayfinder/status-summary-variants", test_status_and_summary_variants);
    g_test_add_func("/wayfinder/snapshot-mapping-rendering", test_snapshot_mapping_and_rendering);
    g_test_add_func("/wayfinder/fetch-errors", test_fetch_sequence_optional_metrics_and_errors);
    g_test_add_func("/wayfinder/cancellation-registry", test_cancellation_and_registry);
    g_test_add_func("/wayfinder/malformed-payloads", test_malformed_payloads);
    return g_test_run();
}
