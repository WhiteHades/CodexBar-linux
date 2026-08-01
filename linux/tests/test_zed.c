#include "zed.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

static const char *const captured =
    "{\"user\":{\"id\":42,\"github_login\":\"octocat\",\"name\":\"Octo Org\"},"
    "\"plan\":{\"plan_v3\":\"zed_pro\",\"subscription_period\":{"
    "\"started_at\":\"2026-07-01T00:00:00Z\",\"ended_at\":\"2026-08-01T00:00:00Z\"},"
    "\"usage\":{\"edit_predictions\":{\"used\":250,\"limit\":1000}},"
    "\"has_overdue_invoices\":true}}";

typedef enum {
    RESPONSE_OK,
    RESPONSE_UNAUTHORIZED,
    RESPONSE_REDIRECT,
    RESPONSE_CANCEL,
    RESPONSE_CUSTOM,
} ResponseKind;

static ResponseKind response_kind;
static GCancellable *expected_cancellable;

static const char *header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static CodexBarHttpResponse *response(long status, const char *body, const char *url) {
    CodexBarHttpResponse *value = g_new0(CodexBarHttpResponse, 1);
    value->status = status;
    value->body = g_strdup(body);
    value->body_length = strlen(body);
    value->headers = g_ptr_array_new();
    value->effective_url = g_strdup(url);
    return value;
}

static CodexBarHttpResponse *transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    g_assert_cmpstr(request->method, ==, "GET");
    g_assert_cmpstr(header(request, "Authorization"), ==, "zed-user zed-token");
    g_assert_cmpstr(header(request, "Accept"), ==, "application/json");
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == expected_cancellable);
    if (response_kind == RESPONSE_CANCEL) g_cancellable_cancel(request->cancellable);
    if (response_kind == RESPONSE_UNAUTHORIZED) return response(401, "{}", request->url);
    if (response_kind == RESPONSE_REDIRECT) return response(200, captured, "https://attacker.example/stolen");
    if (response_kind == RESPONSE_CUSTOM) {
        g_assert_cmpstr(request->url, ==, "https://zed.example/client/users/me");
    } else {
        g_assert_cmpstr(request->url, ==, "https://cloud.zed.dev/client/users/me");
    }
    return response(200, captured, request->url);
}

static CodexBarProviderConfig make_config(void) {
    CodexBarProviderConfig config = {0};
    config.raw = json_object_new_object();
    json_object_object_add(config.raw, "userId", json_object_new_string("zed-user"));
    json_object_object_add(config.raw, "accessToken", json_object_new_string("zed-token"));
    return config;
}

static void clear_config(CodexBarProviderConfig *config) {
    json_object_put(config->raw);
    *config = (CodexBarProviderConfig){0};
}

static void test_parse_limited(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_zed_parse(captured, strlen(captured), 1784203200000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->provider, ==, "zed");
    g_assert_cmpstr(provider->source, ==, "api");
    g_assert_cmpstr(provider->account, ==, "octocat");
    g_assert_cmpstr(provider->plan, ==, "Zed Pro");
    g_assert_cmpstr(provider->identity->organization, ==, "Octo Org");
    g_assert_cmpstr(provider->identity->account_id, ==, "42");
    g_assert_cmpuint(provider->quota_windows->len, ==, 3);
    CodexBarQuotaWindow *predictions = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat(predictions->used_percent, ==, 25);
    g_assert_cmpstr(predictions->detail, ==, "250 / 1000 predictions");
    CodexBarQuotaWindow *cycle = codexbar_provider_quota_window(provider, 1);
    g_assert_true(cycle->has_resets_at);
    g_assert_cmpfloat_with_epsilon(cycle->used_percent, 50.0, 0.001);
    g_assert_cmpstr(cycle->reset_description, ==, "Cycle ends in 15d 12h");
    g_assert_false(codexbar_provider_quota_window(provider, 2)->usage_known);
    codexbar_provider_free(provider);
}

static void test_parse_limits_and_errors(void) {
    const char *unlimited =
        "{\"user\":{\"id\":1,\"github_login\":\"zed\"},\"plan\":{\"plan_v3\":\"zed_student\","
        "\"usage\":{\"edit_predictions\":{\"used\":999,\"limit\":\"unlimited\"}},"
        "\"has_overdue_invoices\":false}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_zed_parse(unlimited, strlen(unlimited), 1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->plan, ==, "Zed Student");
    g_assert_cmpstr(codexbar_provider_quota_window(provider, 0)->detail, ==, "Unlimited");
    codexbar_provider_free(provider);

    const char *object_limit =
        "{\"user\":{\"id\":1,\"github_login\":\"zed\"},\"plan\":{\"plan_v3\":\"future_plan\","
        "\"usage\":{\"edit_predictions\":{\"used\":12,\"limit\":{\"limited\":10}}},"
        "\"has_overdue_invoices\":false}}";
    provider = codexbar_zed_parse(object_limit, strlen(object_limit), 1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->plan, ==, "Future Plan");
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 100);
    codexbar_provider_free(provider);

    provider = codexbar_zed_parse("{} trailing", strlen("{} trailing"), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);

    const char *bad_period =
        "{\"user\":{\"id\":1,\"github_login\":\"zed\"},\"plan\":{\"plan_v3\":\"zed_free\","
        "\"subscription_period\":{\"started_at\":\"invalid\",\"ended_at\":\"2026-08-01T00:00:00Z\"},"
        "\"usage\":{\"edit_predictions\":{\"used\":0,\"limit\":10}},"
        "\"has_overdue_invoices\":false}}";
    provider = codexbar_zed_parse(bad_period, strlen(bad_period), 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_transport_and_security(void) {
    GError *error = NULL;
    CodexBarProviderConfig config = make_config();
    response_kind = RESPONSE_OK;
    CodexBarProvider *provider = codexbar_zed_fetch_with_transport_and_cancellable(
        &config, transport, NULL, 1784203200000, &error);
    g_assert_no_error(error);
    codexbar_provider_free(provider);

    response_kind = RESPONSE_CUSTOM;
    json_object_object_add(config.raw, "serverURL", json_object_new_string("https://zed.example"));
    json_object_object_add(config.raw, "credentialsURL", json_object_new_string("https://zed.example"));
    provider = codexbar_zed_fetch_with_transport_and_cancellable(
        &config, transport, NULL, 1, &error);
    g_assert_no_error(error);
    codexbar_provider_free(provider);

    json_object_object_add(config.raw, "credentialsURL", json_object_new_string("https://evil.example"));
    provider = codexbar_zed_fetch_with_transport_and_cancellable(&config, transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    json_object_object_del(config.raw, "serverURL");
    json_object_object_del(config.raw, "credentialsURL");

    response_kind = RESPONSE_REDIRECT;
    provider = codexbar_zed_fetch_with_transport_and_cancellable(&config, transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    response_kind = RESPONSE_UNAUTHORIZED;
    provider = codexbar_zed_fetch_with_transport_and_cancellable(&config, transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    clear_config(&config);
}

static void test_credentials_and_cancellation(void) {
    GError *error = NULL;
    CodexBarProviderConfig config = {0};
    config.raw = json_object_new_object();
    CodexBarProvider *provider = codexbar_zed_fetch_with_transport_and_cancellable(
        &config, transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    clear_config(&config);

    config = make_config();
    expected_cancellable = g_cancellable_new();
    response_kind = RESPONSE_CANCEL;
    provider = codexbar_zed_fetch_with_transport_and_cancellable(
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
    g_test_add_func("/zed/parse-limited", test_parse_limited);
    g_test_add_func("/zed/parse-limits-errors", test_parse_limits_and_errors);
    g_test_add_func("/zed/transport-security", test_transport_and_security);
    g_test_add_func("/zed/credentials-cancellation", test_credentials_and_cancellation);
    return g_test_run();
}
