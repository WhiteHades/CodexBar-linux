#include "vertex.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-c/json.h>
#include <string.h>
#include <unistd.h>

typedef enum {
    FLOW_USER,
    FLOW_SERVICE,
    FLOW_UNAUTHORIZED,
    FLOW_REDIRECT,
    FLOW_CANCEL,
} Flow;

static Flow flow;
static guint request_count;
static GCancellable *expected_cancellable;

static CodexBarHttpResponse *response(long status, const char *body, const char *url) {
    CodexBarHttpResponse *value = g_new0(CodexBarHttpResponse, 1);
    value->status = status;
    value->body = g_strdup(body);
    value->body_length = strlen(body);
    value->headers = g_ptr_array_new();
    value->effective_url = g_strdup(url);
    return value;
}

static const char *header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static void assert_policy(const CodexBarHttpRequest *request) {
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == expected_cancellable);
}

static CodexBarHttpResponse *transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    request_count++;
    assert_policy(request);
    if (flow == FLOW_CANCEL) g_cancellable_cancel(request->cancellable);
    if (flow == FLOW_REDIRECT) return response(200, "{}", "https://attacker.example/token");
    if (g_str_has_prefix(request->url, "https://oauth2.googleapis.com/token")) {
        g_assert_cmpstr(request->method, ==, "POST");
        g_assert_cmpstr(header(request, "Content-Type"), ==, "application/x-www-form-urlencoded");
        g_assert_nonnull(strstr(request->body, "client_id=client-id"));
        g_assert_nonnull(strstr(request->body, "client_secret=secret%2Fvalue"));
        g_assert_nonnull(strstr(request->body, "refresh_token=refresh-token"));
        return response(200,
                        "{\"access_token\":\"ya29.refreshed\",\"expires_in\":3600,"
                        "\"id_token\":\"x.eyJlbWFpbCI6InVzZXJAZXhhbXBsZS5jb20ifQ.y\"}",
                        request->url);
    }
    g_assert_cmpstr(request->method, ==, "GET");
    g_assert_nonnull(strstr(request->url, "https://monitoring.googleapis.com/v3/projects/test-project/timeSeries?"));
    g_assert_nonnull(strstr(request->url, "aiplatform.googleapis.com"));
    g_assert_cmpint(request->timeout_seconds, ==, 30);
    g_assert_cmpstr(header(request, "Authorization"), ==,
                    flow == FLOW_SERVICE ? "Bearer ya29.service" : "Bearer ya29.refreshed");
    if (flow == FLOW_UNAUTHORIZED) return response(401, "{}", request->url);
    return response(200,
                    request_count % 2 == 0
                        ? "{\"timeSeries\":[]}"
                        : "{\"timeSeries\":[{\"metric\":{},\"resource\":{},"
                          "\"points\":[{\"value\":{\"doubleValue\":1}}]}]}",
                    request->url);
}

static char *token_runner(GCancellable *cancellable, GError **error) {
    (void)error;
    g_assert_true(cancellable == expected_cancellable);
    return g_strdup("ya29.service");
}

typedef struct {
    char *directory;
    char *credentials;
    char *project;
} Files;

static Files make_files(const char *credentials_json, const char *project_text) {
    char *current = g_get_current_dir();
    char *base = g_build_filename(current, ".tmp", NULL);
    g_free(current);
    g_assert_cmpint(g_mkdir_with_parents(base, 0700), ==, 0);
    char *pattern = g_strdup_printf("%s/vertex-%ld-XXXXXX", base, (long)getpid());
    g_free(base);
    g_assert_nonnull(g_mkdtemp(pattern));
    Files files = {
        .directory = pattern,
        .credentials = g_build_filename(pattern, "adc.json", NULL),
        .project = g_build_filename(pattern, "config_default", NULL),
    };
    g_assert_true(g_file_set_contents(files.credentials, credentials_json, -1, NULL));
    if (project_text) g_assert_true(g_file_set_contents(files.project, project_text, -1, NULL));
    return files;
}

static void clear_files(Files *files) {
    g_remove(files->credentials);
    g_remove(files->project);
    g_rmdir(files->directory);
    g_free(files->credentials);
    g_free(files->project);
    g_free(files->directory);
    *files = (Files){0};
}

static CodexBarProviderConfig config_for_files(const Files *files) {
    CodexBarProviderConfig config = {0};
    config.raw = json_object_new_object();
    json_object_object_add(config.raw, "credentialsPath", json_object_new_string(files->credentials));
    json_object_object_add(config.raw, "projectConfigPath", json_object_new_string(files->project));
    return config;
}

static void test_user_credentials(void) {
    Files files = make_files(
        "{\"client_id\":\"client-id\",\"client_secret\":\"secret/value\","
        "\"refresh_token\":\"refresh-token\"}",
        "[core]\nproject = test-project\n");
    CodexBarProviderConfig config = config_for_files(&files);
    GError *error = NULL;
    flow = FLOW_USER;
    request_count = 0;
    CodexBarProvider *provider = codexbar_vertex_fetch_with_adapters(
        &config, transport, token_runner, NULL, 1785542400000, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(request_count, ==, 3);
    g_assert_cmpstr(provider->provider, ==, "vertexai");
    g_assert_cmpstr(provider->source, ==, "oauth");
    g_assert_cmpstr(provider->account, ==, "user@example.com");
    g_assert_cmpstr(provider->identity->organization, ==, "test-project");
    g_assert_cmpstr(provider->identity->login_method, ==, "gcloud");
    codexbar_provider_free(provider);
    json_object_put(config.raw);
    clear_files(&files);
}

static void test_service_account(void) {
    Files files = make_files(
        "{\"type\":\"service_account\",\"project_id\":\"test-project\","
        "\"private_key\":\"-----BEGIN PRIVATE KEY-----\\nabc\\n-----END PRIVATE KEY-----\","
        "\"client_email\":\"service@test.iam.gserviceaccount.com\"}",
        NULL);
    CodexBarProviderConfig config = config_for_files(&files);
    GError *error = NULL;
    flow = FLOW_SERVICE;
    request_count = 0;
    CodexBarProvider *provider = codexbar_vertex_fetch_with_adapters(
        &config, transport, token_runner, NULL, 1785542400000, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(request_count, ==, 2);
    g_assert_cmpstr(provider->account, ==, "service@test.iam.gserviceaccount.com");
    codexbar_provider_free(provider);
    json_object_put(config.raw);
    clear_files(&files);
}

static void test_errors_and_security(void) {
    Files files = make_files(
        "{\"client_id\":\"client-id\",\"client_secret\":\"secret/value\","
        "\"refresh_token\":\"refresh-token\"}",
        "project = test-project\n");
    CodexBarProviderConfig config = config_for_files(&files);
    GError *error = NULL;
    flow = FLOW_UNAUTHORIZED;
    request_count = 0;
    CodexBarProvider *provider = codexbar_vertex_fetch_with_adapters(
        &config, transport, token_runner, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    flow = FLOW_REDIRECT;
    request_count = 0;
    provider = codexbar_vertex_fetch_with_adapters(&config, transport, token_runner, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    json_object_put(config.raw);
    clear_files(&files);

    files = make_files("{}", NULL);
    config = config_for_files(&files);
    provider = codexbar_vertex_fetch_with_adapters(&config, transport, token_runner, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    json_object_put(config.raw);
    clear_files(&files);
}

static void test_cancellation(void) {
    Files files = make_files(
        "{\"client_id\":\"client-id\",\"client_secret\":\"secret/value\","
        "\"refresh_token\":\"refresh-token\"}",
        "project = test-project\n");
    CodexBarProviderConfig config = config_for_files(&files);
    GError *error = NULL;
    flow = FLOW_CANCEL;
    request_count = 0;
    expected_cancellable = g_cancellable_new();
    CodexBarProvider *provider = codexbar_vertex_fetch_with_adapters(
        &config, transport, token_runner, expected_cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(expected_cancellable);
    expected_cancellable = NULL;
    json_object_put(config.raw);
    clear_files(&files);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/vertex/user", test_user_credentials);
    g_test_add_func("/vertex/service", test_service_account);
    g_test_add_func("/vertex/errors-security", test_errors_and_security);
    g_test_add_func("/vertex/cancellation", test_cancellation);
    return g_test_run();
}
