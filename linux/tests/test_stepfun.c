#include "stepfun.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

static const char *const rolling =
    "{\"status\":1,\"five_hour_usage_left_rate\":1,"
    "\"five_hour_usage_reset_time\":\"1777528800\","
    "\"weekly_usage_left_rate\":0.6,\"weekly_usage_reset_time\":1777899600}";
static const char *const credit =
    "{\"status\":1,\"five_hour_usage_left_rate\":0,\"five_hour_usage_reset_time\":\"0\","
    "\"weekly_usage_left_rate\":0,\"weekly_usage_reset_time\":\"0\",\"plan_family\":2,"
    "\"plan_credit_rate_limit\":{\"subscription_credit_reset_time\":\"1786288293\","
    "\"credit_buckets\":[{\"credit_total\":\"1000\",\"credit_residual\":\"750\"}]}}";

static void response_header_free(gpointer data) {
    CodexBarHttpResponseHeader *header = data;
    g_free(header->name);
    g_free(header->value);
    g_free(header);
}

static CodexBarHttpResponse *make_response(long status, const char *body, const char *url) {
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = status;
    response->body = g_strdup(body);
    response->body_length = strlen(body);
    response->headers = g_ptr_array_new_with_free_func(response_header_free);
    response->effective_url = g_strdup(url);
    return response;
}

static void add_header(CodexBarHttpResponse *response, const char *name, const char *value) {
    CodexBarHttpResponseHeader *header = g_new0(CodexBarHttpResponseHeader, 1);
    header->name = g_strdup(name);
    header->value = g_strdup(value);
    g_ptr_array_add(response->headers, header);
}

static const char *request_header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static void test_parse_rolling(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_stepfun_parse_usage(rolling, strlen(rolling), 1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->provider, ==, "stepfun");
    g_assert_cmpstr(provider->source, ==, "web");
    g_assert_cmpstr(provider->identity->login_method, ==, "password");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    CodexBarQuotaWindow *five = codexbar_provider_quota_window(provider, 0);
    CodexBarQuotaWindow *weekly = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpfloat(five->used_percent, ==, 0);
    g_assert_cmpint(five->window_minutes, ==, 300);
    g_assert_cmpfloat_with_epsilon(weekly->used_percent, 40, 0.0001);
    g_assert_cmpint(weekly->window_minutes, ==, 10080);
    g_assert_true(five->has_resets_at);
    codexbar_provider_free(provider);
}

static void test_parse_credit_and_classification(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_stepfun_parse_usage(credit, strlen(credit), 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    CodexBarQuotaWindow *credit_window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat(credit_window->used_percent, ==, 25);
    g_assert_true(credit_window->has_resets_at);
    g_assert_true(credit_window->has_window_minutes);
    g_assert_cmpint(credit_window->window_minutes, ==, 43200);
    const char *plan = "{\"status\":1,\"subscription\":{\"name\":\"Mini Plan\"}}";
    g_assert_true(codexbar_stepfun_apply_plan(provider, plan, strlen(plan)));
    g_assert_cmpstr(provider->plan, ==, "Mini Plan");
    g_assert_cmpstr(provider->identity->login_method, ==, "Mini Plan");
    codexbar_provider_free(provider);

    const char *live =
        "{\"status\":1,\"five_hour_usage_left_rate\":0.8,\"five_hour_usage_reset_time\":1,"
        "\"weekly_usage_left_rate\":0.6,\"weekly_usage_reset_time\":2,\"plan_family\":2,"
        "\"plan_credit_rate_limit\":{\"subscription_credit_left_rate\":1}}";
    provider = codexbar_stepfun_parse_usage(live, strlen(live), 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    codexbar_provider_free(provider);

    const char *zero_reset =
        "{\"status\":1,\"plan_family\":2,\"plan_credit_rate_limit\":{"
        "\"subscription_credit_left_rate\":0.5,\"subscription_credit_reset_time\":\"0\"}}";
    provider = codexbar_stepfun_parse_usage(zero_reset, strlen(zero_reset), 1, &error);
    g_assert_no_error(error);
    credit_window = codexbar_provider_quota_window(provider, 0);
    g_assert_false(credit_window->has_resets_at);
    g_assert_false(credit_window->has_window_minutes);
    codexbar_provider_free(provider);
}

static void test_parse_errors(void) {
    GError *error = NULL;
    const char *trailing = "{} trailing";
    CodexBarProvider *provider = codexbar_stepfun_parse_usage(trailing, strlen(trailing), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    const char *denied = "{\"status\":0,\"message\":\"Unauthorized\"}";
    provider = codexbar_stepfun_parse_usage(denied, strlen(denied), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    const char *missing = "{\"status\":1}";
    provider = codexbar_stepfun_parse_usage(missing, strlen(missing), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

typedef enum { FLOW_DIRECT, FLOW_LOGIN, FLOW_REFRESH, FLOW_REDIRECT, FLOW_CANCEL } Flow;
static Flow flow;
static guint calls;
static GCancellable *expected_cancellable;

static CodexBarHttpResponse *transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    calls++;
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == expected_cancellable);
    g_assert_cmpstr(request_header(request, "oasis-appid"), ==, "10300");
    if (flow == FLOW_CANCEL) {
        g_cancellable_cancel(request->cancellable);
        return NULL;
    }
    if (flow == FLOW_REDIRECT) return make_response(200, rolling, "https://attacker.example/stolen");
    if (g_str_equal(request->url, "https://platform.stepfun.com")) {
        g_assert_cmpstr(request->method, ==, "GET");
        CodexBarHttpResponse *response = make_response(200, "", request->url);
        add_header(response, "Set-Cookie", "INGRESSCOOKIE=ingress; Path=/; Secure");
        return response;
    }
    if (g_str_has_suffix(request->url, "/RegisterDevice")) {
        g_assert_nonnull(strstr(request_header(request, "Cookie"), "INGRESSCOOKIE=ingress"));
        return make_response(200,
                             "{\"accessToken\":{\"raw\":\"anon\"},\"refreshToken\":{\"raw\":\"anon-r\"}}",
                             request->url);
    }
    if (g_str_has_suffix(request->url, "/SignInByPassword")) {
        g_assert_nonnull(strstr(request_header(request, "Cookie"), "Oasis-Token=anon...anon-r"));
        g_assert_nonnull(strstr((const char *)request->body, "user@example.com"));
        return make_response(200,
                             "{\"accessToken\":{\"raw\":\"access\"},\"refreshToken\":{\"raw\":\"refresh\"}}",
                             request->url);
    }
    if (g_str_has_suffix(request->url, "/RefreshToken")) {
        g_assert_nonnull(strstr(request_header(request, "Cookie"), "Oasis-Token=stale"));
        return make_response(200,
                             "{\"accessToken\":{\"raw\":\"fresh\"},\"refreshToken\":{\"raw\":\"fresh-r\"}}",
                             request->url);
    }
    if (g_str_has_suffix(request->url, "/QueryStepPlanRateLimit")) {
        const char *cookie = request_header(request, "Cookie");
        if (flow == FLOW_REFRESH && strstr(cookie, "Oasis-Token=stale")) {
            return make_response(401, "{}", request->url);
        }
        if (flow == FLOW_DIRECT) g_assert_nonnull(strstr(cookie, "Oasis-Token=direct-token"));
        if (flow == FLOW_LOGIN) g_assert_nonnull(strstr(cookie, "Oasis-Token=access...refresh"));
        if (flow == FLOW_REFRESH) g_assert_nonnull(strstr(cookie, "Oasis-Token=fresh...fresh-r"));
        return make_response(200, rolling, request->url);
    }
    if (g_str_has_suffix(request->url, "/GetStepPlanStatus")) {
        return make_response(200, "{\"status\":1,\"subscription\":{\"name\":\"Coding Plan\"}}", request->url);
    }
    g_assert_not_reached();
}

static CodexBarProviderConfig config_with(const char *json) {
    CodexBarProviderConfig config = {0};
    config.raw = json_tokener_parse(json);
    return config;
}

static void clear_config(CodexBarProviderConfig *config) {
    json_object_put(config->raw);
    *config = (CodexBarProviderConfig){0};
}

static void test_fetch_flows(void) {
    GError *error = NULL;
    flow = FLOW_DIRECT;
    calls = 0;
    CodexBarProviderConfig config = config_with("{\"manualToken\":\"Oasis-Token=direct-token; x=y\"}");
    CodexBarProvider *provider = codexbar_stepfun_fetch_with_transport_and_cancellable(
        &config, transport, NULL, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(calls, ==, 2);
    g_assert_cmpstr(provider->plan, ==, "Coding Plan");
    codexbar_provider_free(provider);
    clear_config(&config);

    flow = FLOW_LOGIN;
    calls = 0;
    config = config_with("{\"username\":\"user@example.com\",\"password\":\"secret\"}");
    provider = codexbar_stepfun_fetch_with_transport_and_cancellable(&config, transport, NULL, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(calls, ==, 5);
    codexbar_provider_free(provider);
    clear_config(&config);

    flow = FLOW_REFRESH;
    calls = 0;
    config = config_with("{\"token\":\"stale\"}");
    provider = codexbar_stepfun_fetch_with_transport_and_cancellable(&config, transport, NULL, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(calls, ==, 4);
    codexbar_provider_free(provider);
    clear_config(&config);
}

static void test_security_credentials_and_cancellation(void) {
    GError *error = NULL;
    CodexBarProviderConfig config = config_with("{}");
    CodexBarProvider *provider = codexbar_stepfun_fetch_with_transport_and_cancellable(
        &config, transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    clear_config(&config);

    flow = FLOW_REDIRECT;
    config = config_with("{\"token\":\"direct-token\"}");
    provider = codexbar_stepfun_fetch_with_transport_and_cancellable(&config, transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    flow = FLOW_CANCEL;
    expected_cancellable = g_cancellable_new();
    provider = codexbar_stepfun_fetch_with_transport_and_cancellable(
        &config, transport, expected_cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(expected_cancellable);
    expected_cancellable = NULL;
    clear_config(&config);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/stepfun/parse-rolling", test_parse_rolling);
    g_test_add_func("/stepfun/parse-credit", test_parse_credit_and_classification);
    g_test_add_func("/stepfun/parse-errors", test_parse_errors);
    g_test_add_func("/stepfun/fetch-flows", test_fetch_flows);
    g_test_add_func("/stepfun/security-cancellation", test_security_credentials_and_cancellation);
    return g_test_run();
}
