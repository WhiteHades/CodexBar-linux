#include "azure_openai.h"

#include "render.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

typedef struct {
    const char *url;
    const char *api_key;
    const char *deployment;
    gboolean v1;
    long status;
    const char *body;
    GError *transport_error;
    GCancellable *cancellable;
    gboolean cancel_after;
    guint count;
} AzureFixture;

static AzureFixture fixture;

static void reset_fixture(void) {
    g_clear_error(&fixture.transport_error);
    memset(&fixture, 0, sizeof(fixture));
    fixture.status = 200;
    fixture.body = "{}";
}

static const char *header_value(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static CodexBarHttpResponse *stub_transport(const CodexBarHttpRequest *request, GError **error) {
    fixture.count++;
    g_assert_cmpstr(request->method, ==, "POST");
    g_assert_cmpstr(request->url, ==, fixture.url);
    g_assert_cmpuint(request->timeout_seconds, ==, 20);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == fixture.cancellable);
    g_assert_cmpstr(header_value(request, "api-key"), ==, fixture.api_key);
    g_assert_cmpstr(header_value(request, "Accept"), ==, "application/json");
    g_assert_cmpstr(header_value(request, "Content-Type"), ==, "application/json");

    json_object *payload = json_tokener_parse(request->body);
    g_assert_nonnull(payload);
    json_object *messages = NULL;
    g_assert_true(json_object_object_get_ex(payload, "messages", &messages));
    g_assert_true(json_object_is_type(messages, json_type_array));
    g_assert_cmpuint(json_object_array_length(messages), ==, 1);
    json_object *message = json_object_array_get_idx(messages, 0);
    g_assert_cmpstr(json_object_get_string(json_object_object_get(message, "role")), ==, "user");
    g_assert_cmpstr(json_object_get_string(json_object_object_get(message, "content")), ==, "ping");
    json_object *value = NULL;
    g_assert_false(json_object_object_get_ex(payload, "temperature", &value));
    if (fixture.v1) {
        g_assert_cmpstr(json_object_get_string(json_object_object_get(payload, "model")), ==, fixture.deployment);
        g_assert_cmpint(json_object_get_int(json_object_object_get(payload, "max_completion_tokens")), ==, 1);
        g_assert_false(json_object_object_get_ex(payload, "max_tokens", &value));
    } else {
        g_assert_cmpint(json_object_get_int(json_object_object_get(payload, "max_tokens")), ==, 1);
        g_assert_false(json_object_object_get_ex(payload, "model", &value));
        g_assert_false(json_object_object_get_ex(payload, "max_completion_tokens", &value));
    }
    json_object_put(payload);

    if (fixture.transport_error) {
        if (fixture.cancel_after && fixture.cancellable) g_cancellable_cancel(fixture.cancellable);
        g_propagate_error(error, g_error_copy(fixture.transport_error));
        return NULL;
    }
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = fixture.status;
    response->body = g_strdup(fixture.body);
    response->body_length = strlen(fixture.body);
    response->headers = g_ptr_array_new();
    if (fixture.cancel_after && fixture.cancellable) g_cancellable_cancel(fixture.cancellable);
    return response;
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static void assert_endpoint(const char *raw, const char *expected) {
    GError *error = NULL;
    char *endpoint = codexbar_azure_openai_endpoint_for_testing(raw, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(endpoint, ==, expected);
    g_free(endpoint);
}

static void test_endpoint_security(void) {
    assert_endpoint("https://proxy.example.com/base", "https://proxy.example.com/base");
    assert_endpoint(" resource.openai.azure.com ", "https://resource.openai.azure.com");
    assert_endpoint("'localhost:8443/openai'", "https://localhost:8443/openai");

    const char *invalid[] = {
        "http://localhost",
        "https://user@example.com",
        "https://user:password@example.com",
        "https://proxy.example.com%2f.attacker.test",
        "https://proxy.example.com%5c.attacker.test",
        "https://proxy.example.com%20.attacker.test",
        "https://proxy.example.com\\attacker.test",
        "bad\nhost",
    };
    for (guint index = 0; index < G_N_ELEMENTS(invalid); index++) {
        GError *error = NULL;
        char *endpoint = codexbar_azure_openai_endpoint_for_testing(invalid[index], &error);
        g_assert_null(endpoint);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_assert_nonnull(strstr(error->message, "AZURE_OPENAI_ENDPOINT"));
        g_clear_error(&error);
    }
}

static void assert_url(const char *endpoint,
                       const char *deployment,
                       const char *api_version,
                       const char *expected) {
    GError *error = NULL;
    char *url = codexbar_azure_openai_chat_url_for_testing(endpoint, deployment, api_version, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, expected);
    g_free(url);
}

static void test_request_urls(void) {
    assert_url("https://resource.openai.azure.com",
               "chat-prod",
               "2024-10-21",
               "https://resource.openai.azure.com/openai/deployments/chat-prod/chat/completions?api-version=2024-10-21");
    assert_url("https://proxy.example.com/base",
               "chat prod",
               "2024 10",
               "https://proxy.example.com/base/openai/deployments/chat%20prod/chat/completions?api-version=2024%2010");
    assert_url("https://proxy.example.com/base/openai",
               "chat-prod",
               "2024-10-21",
               "https://proxy.example.com/base/openai/deployments/chat-prod/chat/completions?api-version=2024-10-21");
    assert_url("https://resource.openai.azure.com/openai/v1",
               "chat-prod",
               " V1 ",
               "https://resource.openai.azure.com/openai/v1/chat/completions");
    assert_url("https://proxy.example.com/base%2Ftenant",
               "chat-prod",
               "2024-10-21",
               "https://proxy.example.com/base%2Ftenant/openai/deployments/chat-prod/chat/completions?api-version=2024-10-21");
    assert_url("https://proxy.example.com/base/openai/v1?sig=a%26b#frag%2Fment",
               "chat-prod",
               "v1",
               "https://proxy.example.com/base/openai/v1/chat/completions?sig=a%26b#frag%2Fment");

    const char *dot_segments[] = {".", ".."};
    for (guint index = 0; index < G_N_ELEMENTS(dot_segments); index++) {
        GError *error = NULL;
        char *url = codexbar_azure_openai_chat_url_for_testing(
            "https://resource.openai.azure.com", dot_segments[index], "2024-10-21", &error);
        g_assert_null(url);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_clear_error(&error);
    }
}

static void test_legacy_request_and_mapping(void) {
    g_setenv("AZURE_OPENAI_API_KEY", " environment-key ", TRUE);
    g_setenv("AZURE_OPENAI_ENDPOINT", "https://environment.example.com", TRUE);
    g_setenv("AZURE_OPENAI_DEPLOYMENT_NAME", "environment-deployment", TRUE);
    g_unsetenv("AZURE_OPENAI_API_VERSION");
    CodexBarProviderConfig config = {
        .api_key = " config-key ",
        .workspace_id = " chat-prod ",
        .enterprise_host = " resource.openai.azure.com ",
    };
    reset_fixture();
    fixture.url = "https://resource.openai.azure.com/openai/deployments/chat-prod/chat/completions?api-version=2024-10-21";
    fixture.api_key = "config-key";
    fixture.deployment = "chat-prod";
    fixture.body = "{\"id\":\"cmpl-1\",\"model\":\"gpt-4o-mini\"}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_azure_openai_fetch_with_transport(&config, stub_transport, 1800000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 1);
    g_assert_cmpstr(provider->provider, ==, "azureopenai");
    g_assert_cmpstr(provider->source, ==, "deployment");
    g_assert_true(provider->explicit_quota_slots);
    g_assert_true(provider->has_updated_at);
    g_assert_cmpint(provider->updated_at_ms, ==, G_GINT64_CONSTANT(1800000000000));
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    const CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpstr(window->id, ==, "primary");
    g_assert_true(window->usage_known);
    g_assert_cmpfloat(window->used_percent, ==, 0.0);
    g_assert_cmpstr(window->detail, ==, "Deployment: chat-prod · Model: gpt-4o-mini");
    g_assert_cmpstr(provider->identity->organization, ==, "resource.openai.azure.com");
    g_assert_cmpstr(provider->identity->login_method, ==, "Deployment: chat-prod");
    g_assert_cmpstr(json_object_get_string(json_object_object_get(provider->usage_extensions, "dataConfidence")),
                    ==,
                    "unknown");

    CodexBarSnapshot snapshot = {.providers = g_ptr_array_new()};
    g_ptr_array_add(snapshot.providers, provider);
    char *rendered = codexbar_render_usage_json(&snapshot, FALSE);
    g_assert_nonnull(strstr(rendered, "\"resetDescription\":\"Deployment: chat-prod · Model: gpt-4o-mini\""));
    g_assert_nonnull(strstr(rendered, "\"accountOrganization\":\"resource.openai.azure.com\""));
    g_free(rendered);
    g_ptr_array_free(snapshot.providers, TRUE);
    codexbar_provider_free(provider);
    g_unsetenv("AZURE_OPENAI_API_KEY");
    g_unsetenv("AZURE_OPENAI_ENDPOINT");
    g_unsetenv("AZURE_OPENAI_DEPLOYMENT_NAME");
}

static void test_v1_environment_request(void) {
    g_setenv("AZURE_OPENAI_API_KEY", " 'environment-key' ", TRUE);
    g_setenv("AZURE_OPENAI_ENDPOINT", " \"https://proxy.example.com/base/openai/v1\" ", TRUE);
    g_setenv("AZURE_OPENAI_DEPLOYMENT_NAME", " 'chat-prod' ", TRUE);
    g_setenv("AZURE_OPENAI_API_VERSION", " V1 ", TRUE);
    CodexBarProviderConfig config = {0};
    reset_fixture();
    fixture.url = "https://proxy.example.com/base/openai/v1/chat/completions";
    fixture.api_key = "environment-key";
    fixture.deployment = "chat-prod";
    fixture.v1 = TRUE;
    fixture.body = "{\"model\":null}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_azure_openai_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(codexbar_provider_quota_window(provider, 0)->detail, ==, "Deployment: chat-prod");
    codexbar_provider_free(provider);
    g_unsetenv("AZURE_OPENAI_API_KEY");
    g_unsetenv("AZURE_OPENAI_ENDPOINT");
    g_unsetenv("AZURE_OPENAI_DEPLOYMENT_NAME");
    g_unsetenv("AZURE_OPENAI_API_VERSION");
}

static void test_validation_errors_do_not_send_credentials(void) {
    CodexBarProviderConfig config = {0};
    GError *error = NULL;
    g_unsetenv("AZURE_OPENAI_API_KEY");
    g_unsetenv("AZURE_OPENAI_ENDPOINT");
    g_unsetenv("AZURE_OPENAI_DEPLOYMENT_NAME");
    CodexBarProvider *provider =
        codexbar_azure_openai_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_assert_nonnull(strstr(error->message, "API key not configured"));
    g_clear_error(&error);

    GCancellable *cancelled = g_cancellable_new();
    g_cancellable_cancel(cancelled);
    provider = codexbar_azure_openai_fetch_with_transport_and_cancellable(
        &config, unexpected_transport, cancelled, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    g_object_unref(cancelled);

    config.api_key = "key";
    provider = codexbar_azure_openai_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_assert_nonnull(strstr(error->message, "endpoint not configured"));
    g_clear_error(&error);

    config.enterprise_host = "http://127.0.0.1:31337";
    config.workspace_id = "deployment";
    provider = codexbar_azure_openai_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_assert_nonnull(strstr(error->message, "AZURE_OPENAI_ENDPOINT"));
    g_clear_error(&error);

    config.enterprise_host = "https://resource.openai.azure.com";
    config.workspace_id = NULL;
    provider = codexbar_azure_openai_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_assert_nonnull(strstr(error->message, "deployment not configured"));
    g_clear_error(&error);

    config.workspace_id = "bad\x1B" "deployment";
    provider = codexbar_azure_openai_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
}

static void test_errors_no_retry_and_cancellation(void) {
    CodexBarProviderConfig config = {
        .api_key = "key",
        .workspace_id = "deployment",
        .enterprise_host = "https://resource.openai.azure.com",
    };
    reset_fixture();
    fixture.url = "https://resource.openai.azure.com/openai/deployments/deployment/chat/completions?api-version=2024-10-21";
    fixture.api_key = "key";
    fixture.deployment = "deployment";
    fixture.status = 429;
    fixture.body = "  too\n   many\trequests  ";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_azure_openai_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr(error->message, "HTTP 429: too many requests"));
    g_assert_cmpuint(fixture.count, ==, 1);
    g_clear_error(&error);

    char *long_body = g_strnfill(242, 'x');
    long_body[120] = '\x1B';
    reset_fixture();
    fixture.url = "https://resource.openai.azure.com/openai/deployments/deployment/chat/completions?api-version=2024-10-21";
    fixture.api_key = "key";
    fixture.deployment = "deployment";
    fixture.status = 500;
    fixture.body = long_body;
    provider = codexbar_azure_openai_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr(error->message, "… [truncated]"));
    g_assert_null(strstr(error->message, "\x1B"));
    g_clear_error(&error);
    g_free(long_body);

    reset_fixture();
    fixture.url = "https://resource.openai.azure.com/openai/deployments/deployment/chat/completions?api-version=2024-10-21";
    fixture.api_key = "key";
    fixture.deployment = "deployment";
    fixture.body = "{\"model\":1}";
    provider = codexbar_azure_openai_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_nonnull(strstr(error->message, "response parse error"));
    g_clear_error(&error);

    reset_fixture();
    fixture.url = "https://resource.openai.azure.com/openai/deployments/deployment/chat/completions?api-version=2024-10-21";
    fixture.api_key = "key";
    fixture.deployment = "deployment";
    fixture.body = "{\"model\":\"bad\\u001bmodel\"}";
    provider = codexbar_azure_openai_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(codexbar_provider_quota_window(provider, 0)->detail, ==, "Deployment: deployment");
    codexbar_provider_free(provider);

    reset_fixture();
    fixture.url = "https://resource.openai.azure.com/openai/deployments/deployment/chat/completions?api-version=2024-10-21";
    fixture.api_key = "key";
    fixture.deployment = "deployment";
    fixture.transport_error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CONNECTION_REFUSED, "connection refused");
    provider = codexbar_azure_openai_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr(error->message, "Azure OpenAI network error: connection refused"));
    g_clear_error(&error);

    GCancellable *cancellable = g_cancellable_new();
    reset_fixture();
    fixture.url = "https://resource.openai.azure.com/openai/deployments/deployment/chat/completions?api-version=2024-10-21";
    fixture.api_key = "key";
    fixture.deployment = "deployment";
    fixture.cancellable = cancellable;
    fixture.cancel_after = TRUE;
    fixture.transport_error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
    provider = codexbar_azure_openai_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(cancellable);

    cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);
    provider = codexbar_azure_openai_fetch_with_transport_and_cancellable(
        &config, unexpected_transport, cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(cancellable);

    cancellable = g_cancellable_new();
    reset_fixture();
    fixture.url = "https://resource.openai.azure.com/openai/deployments/deployment/chat/completions?api-version=2024-10-21";
    fixture.api_key = "key";
    fixture.deployment = "deployment";
    fixture.cancellable = cancellable;
    fixture.cancel_after = TRUE;
    fixture.body = "{}";
    provider = codexbar_azure_openai_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    codexbar_provider_free(provider);
    g_object_unref(cancellable);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/azure-openai/endpoint-security", test_endpoint_security);
    g_test_add_func("/azure-openai/request-urls", test_request_urls);
    g_test_add_func("/azure-openai/legacy-request-mapping", test_legacy_request_and_mapping);
    g_test_add_func("/azure-openai/v1-environment-request", test_v1_environment_request);
    g_test_add_func("/azure-openai/validation-errors", test_validation_errors_do_not_send_credentials);
    g_test_add_func("/azure-openai/errors-no-retry-cancellation", test_errors_no_retry_and_cancellation);
    int result = g_test_run();
    reset_fixture();
    return result;
}
