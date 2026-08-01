#include "render.h"
#include "xai.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

typedef struct {
    const char *bodies[2];
    long statuses[2];
    guint count;
    guint expected_count;
    const char *authorization;
    GCancellable *cancellable;
    gboolean network_failure[2];
    gboolean cancel_after[2];
    GPtrArray *urls;
} TransportFixture;

static TransportFixture fixture;

static const char *balance_fixture =
    "{\"changes\":[],\"total\":{\"val\":\"-1000\"}}";

static const char *usage_fixture =
    "{\"timeSeries\":["
    "{\"dataPoints\":["
    "{\"timestamp\":\"2027-01-13T00:00:00Z\",\"values\":[0.75973725]},"
    "{\"timestamp\":\"2027-01-14T00:00:00Z\",\"values\":[0.5]},"
    "{\"timestamp\":\"2027-01-15T00:00:00Z\",\"values\":[0]}]},"
    "{\"dataPoints\":["
    "{\"timestamp\":\"2027-01-13T05:00:00+05:00\",\"values\":[0.5]},"
    "{\"timestamp\":\"2027-01-14T00:00:00.123Z\",\"values\":[0]},"
    "{\"timestamp\":\"2027-01-15T00:00:00Z\",\"values\":[]}]}],"
    "\"limitReached\":false}";

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

static void assert_usage_body(const CodexBarHttpRequest *request) {
    json_object *root = json_tokener_parse(request->body);
    g_assert_nonnull(root);
    json_object *analytics = NULL;
    json_object *range = NULL;
    g_assert_true(json_object_object_get_ex(root, "analyticsRequest", &analytics));
    g_assert_true(json_object_object_get_ex(analytics, "timeRange", &range));
    g_assert_cmpstr(json_object_get_string(json_object_object_get(range, "startTime")),
                    ==,
                    "2026-12-17 00:00:00");
    g_assert_cmpstr(json_object_get_string(json_object_object_get(range, "endTime")),
                    ==,
                    "2027-01-15 08:00:00");
    g_assert_cmpstr(json_object_get_string(json_object_object_get(range, "timezone")), ==, "Etc/GMT");
    g_assert_cmpstr(json_object_get_string(json_object_object_get(analytics, "timeUnit")),
                    ==,
                    "TIME_UNIT_DAY");
    json_object *values = json_object_object_get(analytics, "values");
    g_assert_cmpuint(json_object_array_length(values), ==, 1);
    json_object *value = json_object_array_get_idx(values, 0);
    g_assert_cmpstr(json_object_get_string(json_object_object_get(value, "name")), ==, "usd");
    g_assert_cmpstr(json_object_get_string(json_object_object_get(value, "aggregation")),
                    ==,
                    "AGGREGATION_SUM");
    g_assert_cmpuint(json_object_array_length(json_object_object_get(analytics, "groupBy")), ==, 0);
    g_assert_cmpuint(json_object_array_length(json_object_object_get(analytics, "filters")), ==, 0);
    json_object_put(root);
}

static CodexBarHttpResponse *stub_transport(const CodexBarHttpRequest *request, GError **error) {
    guint index = fixture.count++;
    g_assert_cmpuint(index, <, fixture.expected_count);
    g_assert_cmpstr(request_header(request, "Authorization"), ==, fixture.authorization);
    g_assert_cmpstr(request_header(request, "Accept"), ==, "application/json");
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == fixture.cancellable);
    g_assert_true(g_str_has_prefix(request->url, "https://management-api.x.ai/v1/billing/teams/"));
    g_assert_null(strstr(request->url, "fixture-management-key"));
    g_ptr_array_add(fixture.urls, g_strdup(request->url));
    if (index == 0) {
        g_assert_cmpstr(request->method, ==, "GET");
        g_assert_null(request->body);
        g_assert_null(request_header(request, "Content-Type"));
    } else {
        g_assert_cmpstr(request->method, ==, "POST");
        g_assert_cmpstr(request_header(request, "Content-Type"), ==, "application/json");
        g_assert_cmpuint(request->body_length, ==, strlen(request->body));
        assert_usage_body(request);
    }
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

static void reset_fixture(const char *authorization, guint expected_count) {
    if (fixture.urls) g_ptr_array_unref(fixture.urls);
    memset(&fixture, 0, sizeof(fixture));
    fixture.authorization = authorization;
    fixture.expected_count = expected_count;
    fixture.urls = g_ptr_array_new_with_free_func(g_free);
}

static CodexBarProvider *fetch(const char *balance_body,
                              long balance_status,
                              const char *usage_body,
                              long usage_status,
                              GError **error) {
    reset_fixture("Bearer fixture-management-key", 2);
    fixture.bodies[0] = balance_body;
    fixture.statuses[0] = balance_status;
    fixture.bodies[1] = usage_body;
    fixture.statuses[1] = usage_status;
    CodexBarProviderConfig config = {
        .api_key = "fixture-management-key",
        .workspace_id = "team-1234",
    };
    return codexbar_xai_fetch_with_transport(&config, stub_transport, 1800000000000, error);
}

static json_object *xai_usage(const CodexBarProvider *provider) {
    json_object *usage = NULL;
    g_assert_nonnull(provider->usage_extensions);
    g_assert_true(json_object_object_get_ex(provider->usage_extensions, "xaiUsage", &usage));
    return usage;
}

static void test_request_shape_and_model_mapping(void) {
    GError *error = NULL;
    CodexBarProvider *provider = fetch(balance_fixture, 200, usage_fixture, 200, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(fixture.count, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(fixture.urls, 0),
                    ==,
                    "https://management-api.x.ai/v1/billing/teams/team-1234/prepaid/balance");
    g_assert_cmpstr(g_ptr_array_index(fixture.urls, 1),
                    ==,
                    "https://management-api.x.ai/v1/billing/teams/team-1234/usage");
    g_assert_cmpstr(provider->provider, ==, "xai");
    g_assert_cmpstr(provider->source, ==, "api");
    g_assert_cmpuint(provider->quota_windows->len, ==, 0);
    g_assert_cmpstr(provider->identity->login_method, ==, "Management API");
    g_assert_cmpfloat(provider->provider_cost->used, ==, 10.0);
    g_assert_cmpfloat(provider->provider_cost->limit, ==, 0.0);
    g_assert_cmpstr(provider->provider_cost->currency, ==, "USD");
    g_assert_cmpstr(provider->provider_cost->period, ==, "Prepaid credits");

    json_object *usage = xai_usage(provider);
    g_assert_cmpfloat(json_object_get_double(json_object_object_get(usage, "balanceUSD")), ==, 10.0);
    g_assert_cmpint(json_object_get_int(json_object_object_get(usage, "historyDays")), ==, 30);
    g_assert_false(json_object_get_boolean(json_object_object_get(usage, "limitReached")));
    g_assert_cmpstr(json_object_get_string(json_object_object_get(usage, "updatedAt")),
                    ==,
                    "2027-01-15T08:00:00Z");
    json_object *daily = json_object_object_get(usage, "daily");
    g_assert_cmpuint(json_object_array_length(daily), ==, 3);
    json_object *first = json_object_array_get_idx(daily, 0);
    g_assert_cmpstr(json_object_get_string(json_object_object_get(first, "day")), ==, "2027-01-13");
    g_assert_cmpfloat_with_epsilon(json_object_get_double(json_object_object_get(first, "costUSD")),
                                  1.25973725,
                                  1e-9);
    g_assert_nonnull(provider->token_cost);
    g_assert_cmpfloat(provider->token_cost->today_cost, ==, 0.0);
    g_assert_cmpfloat_with_epsilon(provider->token_cost->last_days_cost, 1.75973725, 1e-9);
    g_assert_cmpstr(json_object_get_string(json_object_object_get(provider->usage_extensions, "dataConfidence")),
                    ==,
                    "exact");

    CodexBarSnapshot *snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    g_ptr_array_add(snapshot->providers, provider);
    char *rendered = codexbar_render_usage_json(snapshot, FALSE);
    g_assert_nonnull(strstr(rendered, "\"xaiUsage\":{\"balanceUSD\":10.0"));
    g_assert_nonnull(strstr(rendered, "\"providerID\":\"xai\",\"loginMethod\":\"Management API\""));
    g_assert_nonnull(strstr(rendered, "\"period\":\"Prepaid credits\""));
    g_free(rendered);
    codexbar_snapshot_free(snapshot);
}

static void test_config_environment_and_team_encoding(void) {
    g_setenv("XAI_MANAGEMENT_API_KEY", " environment-key ", TRUE);
    g_setenv("XAI_TEAM_ID", " environment-team ", TRUE);
    CodexBarProviderConfig config = {0};
    g_assert_true(codexbar_xai_has_api_key(&config));
    g_assert_true(codexbar_xai_has_credentials(&config));

    reset_fixture("Bearer fixture-management-key", 2);
    fixture.bodies[0] = balance_fixture;
    fixture.statuses[0] = 200;
    fixture.bodies[1] = usage_fixture;
    fixture.statuses[1] = 200;
    config.api_key = "\xE2\x80\x83'fixture-management-key'\xE2\x80\x83";
    config.workspace_id = " \"team one\" ";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_xai_fetch_with_transport(&config, stub_transport, 1800000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_nonnull(strstr(g_ptr_array_index(fixture.urls, 0), "/team%20one/"));
    g_assert_null(strstr(g_ptr_array_index(fixture.urls, 0), "team one"));
    codexbar_provider_free(provider);
    g_unsetenv("XAI_MANAGEMENT_API_KEY");
    g_unsetenv("XAI_TEAM_ID");
    g_assert_false(codexbar_xai_has_api_key(NULL));

    const char *invalid[] = {".", "..", "team/../other", "team\\other"};
    for (guint index = 0; index < G_N_ELEMENTS(invalid); index++) {
        config.api_key = "key";
        config.workspace_id = (char *)invalid[index];
        provider = codexbar_xai_fetch_with_transport(&config, unexpected_transport, 1, &error);
        g_assert_null(provider);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_clear_error(&error);
    }
}

static void test_balance_mapping_and_parse_errors(void) {
    const char *bodies[] = {
        "{\"total\":{\"val\":\"2500\"}}",
        "{\"total\":{\"val\":\"0\"}}",
        "{\"total\":{\"val\":\"-333\"}}",
    };
    const double expected[] = {-25.0, 0.0, 3.33};
    for (guint index = 0; index < G_N_ELEMENTS(bodies); index++) {
        GError *error = NULL;
        CodexBarProvider *provider = fetch(bodies[index], 200, usage_fixture, 200, &error);
        g_assert_no_error(error);
        g_assert_cmpfloat_with_epsilon(provider->provider_cost->used, expected[index], 1e-12);
        codexbar_provider_free(provider);
    }

    const char *malformed[] = {
        "{}",
        "{\"total\":{}}",
        "{\"total\":{\"val\":\"\"}}",
        "{\"total\":{\"val\":\"n/a\"}}",
        "{\"total\":{\"val\":\"12abc\"}}",
        "{\"total\":{\"val\":\"+12\"}}",
        "{\"error\":\"forbidden\"}",
    };
    for (guint index = 0; index < G_N_ELEMENTS(malformed); index++) {
        reset_fixture("Bearer fixture-management-key", 1);
        fixture.bodies[0] = malformed[index];
        fixture.statuses[0] = 200;
        CodexBarProviderConfig config = {.api_key = "fixture-management-key", .workspace_id = "team-1234"};
        GError *error = NULL;
        CodexBarProvider *provider = codexbar_xai_fetch_with_transport(&config, stub_transport, 1, &error);
        g_assert_null(provider);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
        g_clear_error(&error);
    }
}

static void test_best_effort_history_and_partial_mapping(void) {
    GError *error = NULL;
    CodexBarProvider *provider = fetch(balance_fixture, 200, "{\"error\":\"unavailable\"}", 500, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 10.0);
    g_assert_cmpuint(json_object_array_length(json_object_object_get(xai_usage(provider), "daily")), ==, 0);
    g_assert_null(provider->token_cost);
    codexbar_provider_free(provider);

    provider = fetch(balance_fixture, 200, "{\"object\":\"list\"}", 200, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(json_object_array_length(json_object_object_get(xai_usage(provider), "daily")), ==, 0);
    codexbar_provider_free(provider);

    const char *partial =
        "{\"timeSeries\":[{\"dataPoints\":[{\"timestamp\":\"2027-01-15T00:00:00Z\","
        "\"values\":[0.5]}]}],\"limitReached\":true}";
    provider = fetch(balance_fixture, 200, partial, 200, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(json_object_get_string(json_object_object_get(provider->usage_extensions, "dataConfidence")),
                    ==,
                    "estimated");
    g_assert_cmpstr(provider->token_cost->history_label, ==, "Last 30 days (partial)");
    codexbar_provider_free(provider);

    reset_fixture("Bearer fixture-management-key", 2);
    fixture.bodies[0] = balance_fixture;
    fixture.statuses[0] = 200;
    fixture.network_failure[1] = TRUE;
    CodexBarProviderConfig config = {.api_key = "fixture-management-key", .workspace_id = "team-1234"};
    provider = codexbar_xai_fetch_with_transport(&config, stub_transport, 1800000000000, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 10.0);
    codexbar_provider_free(provider);
}

static void test_errors_and_cancellation(void) {
    g_unsetenv("XAI_MANAGEMENT_API_KEY");
    g_unsetenv("XAI_TEAM_ID");
    CodexBarProviderConfig config = {0};
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_xai_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_assert_nonnull(strstr(error->message, "XAI_MANAGEMENT_API_KEY"));
    g_clear_error(&error);
    config.api_key = "key";
    provider = codexbar_xai_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_assert_nonnull(strstr(error->message, "XAI_TEAM_ID"));
    g_clear_error(&error);

    const long statuses[] = {401, 403, 404, 429, 503};
    const int codes[] = {
        G_IO_ERROR_PERMISSION_DENIED,
        G_IO_ERROR_PERMISSION_DENIED,
        G_IO_ERROR_NOT_FOUND,
        G_IO_ERROR_BUSY,
        G_IO_ERROR_FAILED,
    };
    config.workspace_id = "team";
    for (guint index = 0; index < G_N_ELEMENTS(statuses); index++) {
        reset_fixture("Bearer key", 1);
        fixture.statuses[0] = statuses[index];
        fixture.bodies[0] = "{}";
        provider = codexbar_xai_fetch_with_transport(&config, stub_transport, 1, &error);
        g_assert_null(provider);
        g_assert_error(error, G_IO_ERROR, codes[index]);
        g_clear_error(&error);
    }

    reset_fixture("Bearer key", 2);
    fixture.statuses[0] = 200;
    fixture.bodies[0] = balance_fixture;
    fixture.statuses[1] = 401;
    fixture.bodies[1] = "{}";
    provider = codexbar_xai_fetch_with_transport(&config, stub_transport, 1800000000000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    GCancellable *cancellable = g_cancellable_new();
    reset_fixture("Bearer key", 2);
    fixture.cancellable = cancellable;
    fixture.statuses[0] = 200;
    fixture.bodies[0] = balance_fixture;
    fixture.statuses[1] = 200;
    fixture.bodies[1] = usage_fixture;
    fixture.cancel_after[1] = TRUE;
    provider = codexbar_xai_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 1800000000000, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(cancellable);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/xai/request-shape-and-model-mapping", test_request_shape_and_model_mapping);
    g_test_add_func("/xai/config-environment-and-team-encoding", test_config_environment_and_team_encoding);
    g_test_add_func("/xai/balance-mapping-and-parse-errors", test_balance_mapping_and_parse_errors);
    g_test_add_func("/xai/best-effort-history-and-partial-mapping", test_best_effort_history_and_partial_mapping);
    g_test_add_func("/xai/errors-and-cancellation", test_errors_and_cancellation);
    int result = g_test_run();
    if (fixture.urls) g_ptr_array_unref(fixture.urls);
    return result;
}
