#include "grok.h"

#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>
#include <unistd.h>

typedef enum {
    RUN_SUCCESS,
    RUN_FAIL,
    RUN_METHOD_MISSING,
} RunnerMode;

static RunnerMode runner_mode;
static GByteArray *web_response;
static gboolean redirect_response;
static long web_status = 200;
static GCancellable *expected_cancellable;

static char *runner(const char *binary, const char *input, GCancellable *cancellable, GError **error) {
    g_assert_cmpstr(binary, ==, "grok-test");
    g_assert_nonnull(strstr(input, "\"method\":\"initialize\""));
    g_assert_nonnull(strstr(input, "\"method\":\"x.ai/billing\""));
    g_assert_null(strstr(input, "x.ai\\/billing"));
    g_assert_true(cancellable == expected_cancellable);
    if (runner_mode == RUN_FAIL) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "grok unavailable");
        return NULL;
    }
    if (runner_mode == RUN_METHOD_MISSING) {
        return g_strdup("{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n"
                        "{\"jsonrpc\":\"2.0\",\"id\":2,\"error\":{\"message\":\"Method not found\"}}\n");
    }
    return g_strdup(
        "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n"
        "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{"
        "\"billingCycle\":{\"billingPeriodStart\":\"2026-05-01T00:00:00Z\","
        "\"billingPeriodEnd\":\"2026-06-01T00:00:00Z\"},"
        "\"monthlyLimit\":{\"val\":99900},\"usage\":{\"totalUsed\":{\"val\":49950}}}}\n");
}

static const char *header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static CodexBarHttpResponse *transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    g_assert_cmpstr(request->url, ==,
                    "https://grok.com/grok_api_v2.GrokBuildBilling/GetGrokCreditsConfig");
    g_assert_cmpstr(request->method, ==, "POST");
    g_assert_cmpstr(header(request, "Authorization"), ==, "Bearer direct-token");
    g_assert_cmpstr(header(request, "Content-Type"), ==, "application/grpc-web+proto");
    g_assert_cmpstr(header(request, "x-grpc-web"), ==, "1");
    g_assert_cmpstr(header(request, "Origin"), ==, "https://grok.com");
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->body_length, ==, 5);
    const guint8 empty_message[5] = {0};
    g_assert_cmpmem(request->body, request->body_length, empty_message, sizeof(empty_message));
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == expected_cancellable);
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = web_status;
    response->body = g_memdup2(web_response->data, web_response->len);
    response->body_length = web_response->len;
    response->headers = g_ptr_array_new();
    response->effective_url = g_strdup(redirect_response ? "https://attacker.example/stolen" : request->url);
    return response;
}

static void append_varint(GByteArray *data, guint64 value) {
    while (value >= 0x80) {
        guint8 byte = (guint8)((value & 0x7f) | 0x80);
        g_byte_array_append(data, &byte, 1);
        value >>= 7;
    }
    guint8 byte = (guint8)value;
    g_byte_array_append(data, &byte, 1);
}

static void append_key(GByteArray *data, guint field, guint wire) {
    append_varint(data, (guint64)field << 3 | wire);
}

static void append_nested(GByteArray *data, guint field, const GByteArray *nested) {
    append_key(data, field, 2);
    append_varint(data, nested->len);
    g_byte_array_append(data, nested->data, nested->len);
}

static GByteArray *make_web_response(float percent, guint64 reset) {
    GByteArray *reset_message = g_byte_array_new();
    append_key(reset_message, 1, 0);
    append_varint(reset_message, reset);
    GByteArray *level_five = g_byte_array_new();
    append_nested(level_five, 5, reset_message);
    GByteArray *payload = g_byte_array_new();
    append_nested(payload, 1, level_five);
    append_key(payload, 1, 5);
    guint32 bits = 0;
    memcpy(&bits, &percent, sizeof(bits));
    guint8 encoded[4] = {
        (guint8)bits, (guint8)(bits >> 8), (guint8)(bits >> 16), (guint8)(bits >> 24),
    };
    g_byte_array_append(payload, encoded, sizeof(encoded));

    GByteArray *framed = g_byte_array_new();
    guint8 header_bytes[5] = {
        0, (guint8)(payload->len >> 24), (guint8)(payload->len >> 16),
        (guint8)(payload->len >> 8), (guint8)payload->len,
    };
    g_byte_array_append(framed, header_bytes, sizeof(header_bytes));
    g_byte_array_append(framed, payload->data, payload->len);
    g_byte_array_unref(reset_message);
    g_byte_array_unref(level_five);
    g_byte_array_unref(payload);
    return framed;
}

typedef struct {
    char *directory;
    char *auth;
} AuthFile;

static AuthFile make_auth(const char *json) {
    char *current = g_get_current_dir();
    char *base = g_build_filename(current, ".tmp", NULL);
    g_free(current);
    g_assert_cmpint(g_mkdir_with_parents(base, 0700), ==, 0);
    char *pattern = g_strdup_printf("%s/grok-%ld-XXXXXX", base, (long)getpid());
    g_free(base);
    g_assert_nonnull(g_mkdtemp(pattern));
    AuthFile file = {.directory = pattern, .auth = g_build_filename(pattern, "auth.json", NULL)};
    g_assert_true(g_file_set_contents(file.auth, json, -1, NULL));
    return file;
}

static void clear_auth(AuthFile *file) {
    g_remove(file->auth);
    g_rmdir(file->directory);
    g_free(file->auth);
    g_free(file->directory);
    *file = (AuthFile){0};
}

static CodexBarProviderConfig base_config(void) {
    CodexBarProviderConfig config = {0};
    config.raw = json_object_new_object();
    json_object_object_add(config.raw, "binaryPath", json_object_new_string("grok-test"));
    return config;
}

static void test_billing_parser(void) {
    const char *json =
        "{\"billingCycle\":{\"billingPeriodStart\":\"2026-05-01T00:00:00Z\","
        "\"billingPeriodEnd\":\"2026-06-01T00:00:00Z\"},"
        "\"monthlyLimit\":{\"val\":99900},\"usage\":{\"totalUsed\":{\"val\":49950}}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_grok_parse_billing(json, strlen(json), 1, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 50);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 0)->window_minutes, ==, 31 * 24 * 60);
    codexbar_provider_free(provider);

    provider = codexbar_grok_parse_billing("{}", 2, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 0);
    codexbar_provider_free(provider);
}

static void test_cli_and_auth(void) {
    AuthFile file = make_auth(
        "{\"https://accounts.x.ai/sign-in\":{\"key\":\"legacy\",\"email\":\"old@example.com\"},"
        "\"https://auth.x.ai::client\":{\"key\":\"preferred\",\"auth_mode\":\"oidc\","
        "\"email\":\"user@example.com\",\"team_id\":\"team-123\"}}" );
    CodexBarProviderConfig config = base_config();
    json_object_object_add(config.raw, "authPath", json_object_new_string(file.auth));
    runner_mode = RUN_SUCCESS;
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_grok_fetch_with_adapters(
        &config, "cli", transport, runner, NULL, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->source, ==, "cli");
    g_assert_cmpstr(provider->account, ==, "user@example.com");
    g_assert_cmpstr(provider->identity->organization, ==, "team-123");
    g_assert_cmpstr(provider->identity->login_method, ==, "SuperGrok");
    codexbar_provider_free(provider);
    json_object_put(config.raw);
    clear_auth(&file);
}

static void test_web_and_auto_fallback(void) {
    web_response = make_web_response(37.5f, 1800000000);
    CodexBarProviderConfig config = base_config();
    config.api_key = g_strdup("direct-token");
    runner_mode = RUN_FAIL;
    web_status = 200;
    redirect_response = FALSE;
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_grok_fetch_with_adapters(
        &config, "auto", transport, runner, NULL, 1785542400000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->source, ==, "web");
    g_assert_cmpfloat_with_epsilon(codexbar_provider_quota_window(provider, 0)->used_percent, 37.5, 0.001);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 0)->resets_at_ms, ==, 1800000000000);
    codexbar_provider_free(provider);

    redirect_response = TRUE;
    provider = codexbar_grok_fetch_with_adapters(&config, "web", transport, runner, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    redirect_response = FALSE;

    web_status = 401;
    provider = codexbar_grok_fetch_with_adapters(&config, "web", transport, runner, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    web_status = 200;
    g_free(config.api_key);
    json_object_put(config.raw);
    g_byte_array_unref(web_response);
    web_response = NULL;
}

static void test_team_identity_fallback(void) {
    AuthFile file = make_auth(
        "{\"https://auth.x.ai::client\":{\"key\":\"token\",\"email\":\"team@example.com\","
        "\"team_id\":\"team-123\",\"principal_type\":\" Team \"}}" );
    CodexBarProviderConfig config = base_config();
    json_object_object_add(config.raw, "authPath", json_object_new_string(file.auth));
    runner_mode = RUN_METHOD_MISSING;
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_grok_fetch_with_adapters(
        &config, "cli", transport, runner, NULL, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->account, ==, "team@example.com");
    g_assert_nonnull(strstr(provider->note, "team usage is unavailable"));
    codexbar_provider_free(provider);
    json_object_put(config.raw);
    clear_auth(&file);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/grok/billing-parser", test_billing_parser);
    g_test_add_func("/grok/cli-auth", test_cli_and_auth);
    g_test_add_func("/grok/web-auto", test_web_and_auto_fallback);
    g_test_add_func("/grok/team-fallback", test_team_identity_fallback);
    return g_test_run();
}
