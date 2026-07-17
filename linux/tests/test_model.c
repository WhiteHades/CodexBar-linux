#include "codex.h"
#include "config.h"
#include "http.h"
#include "model.h"
#include "openrouter.h"
#include "simple_providers.h"
#include "render.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-c/json.h>

typedef struct {
    GSocketListener *listener;
    char *request;
} HttpTestServer;

typedef struct {
    GSocketListener *listener;
    guint16 port;
    gboolean invalid_target;
} RedirectTestServer;

static GSocketConnection *accept_http_connection(GSocketListener *listener) {
    GError *error = NULL;
    GSocketConnection *connection = g_socket_listener_accept(listener, NULL, NULL, &error);
    g_assert_no_error(error);
    char buffer[512];
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    gssize bytes = g_input_stream_read(input, buffer, sizeof(buffer), NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpint(bytes, >, 0);
    return connection;
}

static void write_http_response(GSocketConnection *connection, const char *response) {
    GError *error = NULL;
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    gsize written = 0;
    g_assert_true(g_output_stream_write_all(output, response, strlen(response), &written, NULL, &error));
    g_assert_no_error(error);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
}

static gpointer serve_redirect_test(gpointer data) {
    RedirectTestServer *server = data;
    GSocketConnection *connection = accept_http_connection(server->listener);
    char *response = server->invalid_target
                         ? g_strdup_printf("HTTP/1.1 302 Found\r\nLocation: http://user:secret@127.0.0.1:%u/final\r\n"
                                           "Content-Length: 0\r\nConnection: close\r\n\r\n",
                                           server->port)
                         : g_strdup("HTTP/1.1 302 Found\r\nLocation: /final\r\nContent-Length: 0\r\n"
                                    "Connection: close\r\n\r\n");
    write_http_response(connection, response);
    g_free(response);
    if (!server->invalid_target) {
        connection = accept_http_connection(server->listener);
        write_http_response(connection,
                            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nOK");
    }
    return NULL;
}

static gpointer serve_http_test(gpointer data) {
    HttpTestServer *server = data;
    GError *error = NULL;
    GSocketConnection *connection = g_socket_listener_accept(server->listener, NULL, NULL, &error);
    g_assert_no_error(error);
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    GOutputStream *output = g_io_stream_get_output_stream(G_IO_STREAM(connection));
    GByteArray *request = g_byte_array_new();
    char buffer[512];
    while (request->len < 4096) {
        gssize read = g_input_stream_read(input, buffer, sizeof(buffer), NULL, &error);
        g_assert_no_error(error);
        if (read <= 0) break;
        g_byte_array_append(request, (const guint8 *)buffer, (guint)read);
        g_byte_array_append(request, (const guint8 *)"", 1);
        gboolean complete = strstr((const char *)request->data, "\r\n\r\n") &&
                            strstr((const char *)request->data, "{\"ping\":1}");
        request->len--;
        if (complete) break;
    }
    g_byte_array_append(request, (const guint8 *)"", 1);
    server->request = (char *)g_byte_array_free(request, FALSE);
    const char response[] = "HTTP/1.1 100 Continue\r\nX-Test: stale\r\n\r\n"
                            "HTTP/1.1 200 OK\r\nContent-Length: 11\r\nX-Test: captured\r\n"
                            "Connection: close\r\n\r\n{\"ok\":true}";
    gsize written = 0;
    g_assert_true(g_output_stream_write_all(output, response, sizeof(response) - 1, &written, NULL, &error));
    g_assert_no_error(error);
    g_io_stream_close(G_IO_STREAM(connection), NULL, NULL);
    g_object_unref(connection);
    return NULL;
}

static const char *fixture =
    "[{"
    "\"provider\":\"codex\","
    "\"account\":\"dev@example.com\","
    "\"plan\":\"Pro\","
    "\"source\":\"oauth\","
    "\"note\":\"plan expires Jul 31, 2026\","
    "\"usage\":{"
    "\"primary\":{\"label\":\"session\",\"usedPercent\":28,"
    "\"resetDescription\":\"28 / 100 requests\","
    "\"resetsAt\":\"resets Thu, Jul 23 at 10:16\"},"
    "\"secondary\":{\"usedPercent\":71.4,\"resetDescription\":\"Resets Friday\"},"
    "\"tertiary\":null},"
    "\"credits\":{\"label\":\"balance\",\"remaining\":12.5}"
    "},{"
    "\"provider\":\"claude\","
    "\"source\":\"cli\","
    "\"usage\":{\"primary\":{\"usedPercent\":91}},"
    "\"error\":null"
    "}]";

static void test_parse_snapshot(void) {
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(fixture, &error);
    g_assert_no_error(error);
    g_assert_nonnull(snapshot);
    g_assert_cmpuint(snapshot->providers->len, ==, 2);

    CodexBarProvider *codex = g_ptr_array_index(snapshot->providers, 0);
    g_assert_cmpstr(codex->provider, ==, "codex");
    g_assert_cmpstr(codex->account, ==, "dev@example.com");
    g_assert_cmpstr(codex->plan, ==, "Pro");
    g_assert_cmpstr(codex->note, ==, "plan expires Jul 31, 2026");
    g_assert_true(codex->primary.available);
    g_assert_cmpstr(codex->primary.label, ==, "session");
    g_assert_cmpfloat(codex->primary.used_percent, ==, 28.0);
    g_assert_true(codex->has_credits);
    g_assert_cmpstr(codex->credits_label, ==, "balance");
    g_assert_cmpfloat(codex->credits_remaining, ==, 12.5);
    g_assert_cmpfloat(codexbar_snapshot_highest_used(snapshot), ==, 91.0);
    codexbar_snapshot_free(snapshot);
}

static void test_rejects_non_array(void) {
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse("{}", &error);
    g_assert_null(snapshot);
    g_assert_error(error, g_quark_from_static_string("codexbar-model-error"), 2);
    g_clear_error(&error);
}

static void test_endpoint_policy(void) {
    GError *error = NULL;
    char *url = codexbar_http_normalize_endpoint(
        "api.example.com/v1", CODEXBAR_HTTP_HTTPS_ONLY, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "https://api.example.com/v1");
    g_free(url);

    url = codexbar_http_normalize_endpoint(
        "http://127.0.0.1:8080/v1", CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "http://127.0.0.1:8080/v1");
    g_free(url);

    url = codexbar_http_normalize_endpoint(
        "http://api.example.com/v1", CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP, &error);
    g_assert_null(url);
    g_assert_error(error, g_quark_from_static_string("codexbar-http-error"), 1);
    g_clear_error(&error);

    url = codexbar_http_normalize_endpoint(
        "https://user:secret@api.example.com/v1", CODEXBAR_HTTP_HTTPS_ONLY, &error);
    g_assert_null(url);
    g_assert_error(error, g_quark_from_static_string("codexbar-http-error"), 1);
    g_clear_error(&error);

    url = codexbar_http_normalize_endpoint(
        "https://api.example.com%2f.attacker.test/v1", CODEXBAR_HTTP_HTTPS_ONLY, &error);
    g_assert_null(url);
    g_assert_error(error, g_quark_from_static_string("codexbar-http-error"), 1);
    g_clear_error(&error);
}

static void test_http_post_and_response_headers(void) {
    GError *error = NULL;
    HttpTestServer server = {.listener = g_socket_listener_new()};
    guint16 port = g_socket_listener_add_any_inet_port(server.listener, NULL, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(port, >, 0);
    GThread *thread = g_thread_new("http-test-server", serve_http_test, &server);

    char *url = g_strdup_printf("http://127.0.0.1:%u/usage", port);
    const char body[] = "{\"ping\":1}";
    const CodexBarHttpRequestHeader headers[] = {
        {"Authorization", "Bearer test"},
        {"Content-Type", "application/json"},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "POST",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .body = body,
        .body_length = sizeof(body) - 1,
        .timeout_seconds = 2,
        .protocol_policy = CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
    };
    CodexBarHttpResponse *response = codexbar_http_send(&request, &error);
    g_assert_no_error(error);
    g_assert_nonnull(response);
    g_assert_cmpint(response->status, ==, 200);
    g_assert_cmpuint(response->body_length, ==, 11);
    g_assert_cmpstr(response->body, ==, "{\"ok\":true}");
    g_assert_cmpstr(codexbar_http_response_header_first(response, "x-test"), ==, "captured");
    codexbar_http_response_free(response);
    g_thread_join(thread);
    g_assert_nonnull(strstr(server.request, "POST /usage HTTP/1.1"));
    g_assert_nonnull(strstr(server.request, "Authorization: Bearer test"));
    g_assert_nonnull(strstr(server.request, body));
    g_free(server.request);
    g_free(url);
    g_object_unref(server.listener);
}

static void test_http_rejects_malformed_requests(void) {
    GError *error = NULL;
    const CodexBarHttpRequest missing_body = {
        .url = "https://example.com",
        .method = "POST",
        .body_length = 5,
    };
    g_assert_null(codexbar_http_send(&missing_body, &error));
    g_assert_error(error, g_quark_from_static_string("codexbar-http-error"), 3);
    g_clear_error(&error);

    const CodexBarHttpRequest missing_headers = {
        .url = "https://example.com",
        .method = "GET",
        .header_count = 1,
    };
    g_assert_null(codexbar_http_send(&missing_headers, &error));
    g_assert_error(error, g_quark_from_static_string("codexbar-http-error"), 3);
    g_clear_error(&error);

    const CodexBarHttpRequest injected_method = {
        .url = "https://example.com",
        .method = "GET\r\nX-Injected: yes",
    };
    g_assert_null(codexbar_http_send(&injected_method, &error));
    g_assert_error(error, g_quark_from_static_string("codexbar-http-error"), 3);
    g_clear_error(&error);

    const CodexBarHttpRequestHeader invalid_headers[] = {{"Bad Header", "value"}};
    const CodexBarHttpRequest injected_header = {
        .url = "https://example.com",
        .method = "GET",
        .headers = invalid_headers,
        .header_count = G_N_ELEMENTS(invalid_headers),
    };
    g_assert_null(codexbar_http_send(&injected_header, &error));
    g_assert_error(error, g_quark_from_static_string("codexbar-http-error"), 3);
    g_clear_error(&error);
}

static void test_http_redirect_policy(void) {
    for (int invalid = 0; invalid < 2; invalid++) {
        GError *error = NULL;
        RedirectTestServer server = {
            .listener = g_socket_listener_new(),
            .invalid_target = invalid == 1,
        };
        server.port = g_socket_listener_add_any_inet_port(server.listener, NULL, &error);
        g_assert_no_error(error);
        GThread *thread = g_thread_new("redirect-test-server", serve_redirect_test, &server);
        char *url = g_strdup_printf("http://127.0.0.1:%u/start", server.port);
        const CodexBarHttpRequest request = {
            .url = url,
            .method = "GET",
            .timeout_seconds = 2,
            .protocol_policy = CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP,
            .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        };
        CodexBarHttpResponse *response = codexbar_http_send(&request, &error);
        if (server.invalid_target) {
            g_assert_null(response);
            g_assert_nonnull(error);
            g_clear_error(&error);
        } else {
            g_assert_no_error(error);
            g_assert_nonnull(response);
            g_assert_cmpint(response->status, ==, 200);
            g_assert_cmpstr(response->body, ==, "OK");
            codexbar_http_response_free(response);
        }
        g_thread_join(thread);
        g_free(url);
        g_object_unref(server.listener);
    }
}

static void test_extended_provider_config(void) {
    GError *error = NULL;
    char *cwd = g_get_current_dir();
    char *path = g_build_filename(cwd, "codexbar-config-test.json", NULL);
    g_free(cwd);
    const char json[] =
        "{\"providers\":[{\"id\":\"bedrock\",\"enabled\":true,\"source\":\"api\","
        "\"extrasEnabled\":false,\"apiKey\":\" access \",\"secretKey\":\" secret \","
        "\"region\":\"us-east-1\",\"workspaceID\":\"project-1\","
        "\"enterpriseHost\":\"api.example.com\",\"awsProfile\":\"prod\","
        "\"awsAuthMode\":\"profile\"},{\"id\":\"poe\",\"enabled\":true}]}";
    g_assert_true(g_file_set_contents(path, json, -1, &error));
    g_assert_no_error(error);
    const char *previous = g_getenv("CODEXBAR_CONFIG");
    char *saved = previous ? g_strdup(previous) : NULL;
    g_setenv("CODEXBAR_CONFIG", path, TRUE);
    CodexBarConfig *config = codexbar_config_load(&error);
    g_assert_no_error(error);
    g_assert_nonnull(config);
    g_assert_cmpuint(config->providers->len, ==, 2);
    const CodexBarProviderConfig *provider = g_ptr_array_index(config->providers, 0);
    g_assert_true(provider->has_extras_enabled);
    g_assert_false(provider->extras_enabled);
    g_assert_cmpstr(provider->api_key, ==, "access");
    g_assert_cmpstr(provider->secret_key, ==, "secret");
    g_assert_cmpstr(provider->workspace_id, ==, "project-1");
    g_assert_cmpstr(provider->enterprise_host, ==, "api.example.com");
    g_assert_cmpstr(provider->aws_profile, ==, "prod");
    g_assert_cmpstr(provider->aws_auth_mode, ==, "profile");
    provider = g_ptr_array_index(config->providers, 1);
    g_assert_false(provider->has_extras_enabled);
    codexbar_config_free(config);
    if (saved) {
        g_setenv("CODEXBAR_CONFIG", saved, TRUE);
    } else {
        g_unsetenv("CODEXBAR_CONFIG");
    }
    g_free(saved);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_free(path);
}

static void test_waybar_rendering(void) {
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(fixture, &error);
    g_assert_no_error(error);

    char *rendered = codexbar_render_waybar(snapshot);
    json_object *object = json_tokener_parse(rendered);
    g_assert_nonnull(object);

    json_object *class_name = NULL;
    json_object *percentage = NULL;
    json_object *tooltip = NULL;
    g_assert_true(json_object_object_get_ex(object, "class", &class_name));
    g_assert_true(json_object_object_get_ex(object, "percentage", &percentage));
    g_assert_true(json_object_object_get_ex(object, "tooltip", &tooltip));
    g_assert_cmpstr(json_object_get_string(class_name), ==, "critical");
    g_assert_cmpint(json_object_get_int(percentage), ==, 91);
    const char *tooltip_text = json_object_get_string(tooltip);
    g_assert_true(g_str_has_prefix(tooltip_text, "codex · dev@example.com"));
    g_assert_null(strstr(tooltip_text, "CODEXBAR // USAGE"));
    g_assert_nonnull(strstr(tooltip_text, "Pro · oauth"));
    g_assert_nonnull(strstr(tooltip_text, "plan expires Jul 31, 2026"));
    g_assert_nonnull(strstr(tooltip_text, "28% used · 72% left"));
    g_assert_nonnull(strstr(tooltip_text, "███░░░░░░░"));
    g_assert_nonnull(strstr(tooltip_text, "28 / 100 requests"));
    g_assert_nonnull(strstr(tooltip_text, "resets Thu, Jul 23 at 10:16"));
    g_assert_nonnull(strstr(json_object_get_string(tooltip), "claude"));

    json_object_put(object);
    g_free(rendered);
    codexbar_snapshot_free(snapshot);
}

static void test_openrouter_credits(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_openrouter_parse_credits(
        "{\"data\":{\"total_credits\":100,\"total_usage\":27.5}}", &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpfloat_with_epsilon(provider->credits_remaining, 72.5, 0.0001);
    g_assert_cmpfloat_with_epsilon(provider->primary.used_percent, 27.5, 0.0001);
    codexbar_provider_free(provider);
}

static void test_simple_provider_parsers(void) {
    GError *error = NULL;
    CodexBarProvider *deepseek = codexbar_deepseek_parse(
        "{\"is_available\":true,\"balance_infos\":[{\"total_balance\":\"8.25\"}]}", &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(deepseek->credits_remaining, 8.25, 0.0001);
    codexbar_provider_free(deepseek);

    CodexBarProvider *moonshot = codexbar_moonshot_parse(
        "{\"code\":0,\"status\":true,\"data\":{\"available_balance\":12.5}}", &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(moonshot->credits_remaining, 12.5, 0.0001);
    codexbar_provider_free(moonshot);

    CodexBarProvider *elevenlabs = codexbar_elevenlabs_parse(
        "{\"character_count\":250,\"character_limit\":1000}", &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(elevenlabs->primary.used_percent, 25.0, 0.0001);
    codexbar_provider_free(elevenlabs);

    CodexBarProvider *crof = codexbar_crof_parse(
        "{\"credits\":9.9999,\"requests_plan\":1000,\"usable_requests\":998}", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(crof->primary.label, ==, "requests");
    g_assert_cmpfloat_with_epsilon(crof->primary.used_percent, 1.0, 0.0001);
    g_assert_cmpstr(crof->primary.reset_description, ==, "998 requests left");
    g_assert_nonnull(strstr(crof->primary.resets_at, "resets "));
    g_assert_cmpstr(crof->secondary.label, ==, "balance");
    g_assert_cmpstr(crof->secondary.reset_description, ==, "$9.99");
    codexbar_provider_free(crof);

    crof = codexbar_crof_parse("{\"credits\":0,\"requests_plan\":0,\"usable_requests\":0}", &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(crof->primary.used_percent, 100.0, 0.0001);
    codexbar_provider_free(crof);

    crof = codexbar_crof_parse("{\"credits\":0,\"requests_plan\":1000,\"usable_requests\":1200}", &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(crof->primary.used_percent, 0.0, 0.0001);
    g_assert_cmpstr(crof->primary.reset_description, ==, "1200 requests left");
    codexbar_provider_free(crof);

    CodexBarProvider *venice = codexbar_venice_parse(
        "{\"canConsume\":true,\"consumptionCurrency\":\"BUNDLED_CREDITS\","
        "\"balances\":{\"diem\":\"50.0\",\"usd\":10.0},\"diemEpochAllocation\":\"100.0\"}",
        &error);
    g_assert_no_error(error);
    g_assert_cmpstr(venice->primary.label, ==, "balance");
    g_assert_cmpfloat_with_epsilon(venice->primary.used_percent, 50.0, 0.0001);
    g_assert_cmpstr(venice->primary.reset_description, ==, "DIEM 50.00 / 100.00 epoch allocation");
    codexbar_provider_free(venice);

    venice = codexbar_venice_parse(
        "{\"canConsume\":true,\"balances\":{\"diem\":\"not-a-number\",\"usd\":null}}", &error);
    g_assert_null(venice);
    g_assert_error(error, g_quark_from_static_string("codexbar-simple-provider-error"), 8);
    g_clear_error(&error);

    venice = codexbar_venice_parse(
        "{\"canConsume\":true,\"consumptionCurrency\":null,"
        "\"balances\":{\"diem\":\"   \",\"usd\":null},\"diemEpochAllocation\":null}",
        &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(venice->primary.used_percent, 100.0, 0.0001);
    codexbar_provider_free(venice);

    CodexBarProvider *zenmux = codexbar_zenmux_parse_subscription(
        "{\"success\":true,\"data\":{\"plan\":{\"tier\":\"ultra\","
        "\"expires_at\":\"2026-04-12T08:26:56.000Z\"},\"account_status\":\"healthy\","
        "\"quota_5_hour\":{\"usage_percentage\":0.0715,\"resets_at\":\"2026-03-24T08:35:09.000Z\","
        "\"max_flows\":800,\"used_flows\":57.2,\"remaining_flows\":742.8},"
        "\"quota_7_day\":{\"usage_percentage\":0.0673,\"resets_at\":\"2026-03-26T02:15:05.000Z\","
        "\"max_flows\":6182,\"used_flows\":416.11,\"remaining_flows\":5765.89}}}",
        &error);
    g_assert_no_error(error);
    g_assert_cmpstr(zenmux->plan, ==, "Ultra plan");
    g_assert_nonnull(strstr(zenmux->note, "plan expires "));
    g_assert_nonnull(strstr(zenmux->note, "2026"));
    g_assert_cmpstr(zenmux->primary.label, ==, "5-hour");
    g_assert_cmpfloat_with_epsilon(zenmux->primary.used_percent, 7.15, 0.0001);
    g_assert_cmpstr(zenmux->primary.reset_description, ==, "57.20 / 800 flows");
    g_assert_nonnull(strstr(zenmux->primary.resets_at, "Mar"));
    g_assert_cmpstr(zenmux->secondary.label, ==, "weekly");
    g_assert_true(codexbar_zenmux_apply_payg(
        zenmux, "{\"success\":true,\"data\":{\"currency\":\"usd\",\"total_credits\":482.74}}", &error));
    g_assert_no_error(error);
    g_assert_cmpstr(zenmux->credits_label, ==, "pay as you go");
    g_assert_cmpfloat_with_epsilon(zenmux->credits_remaining, 482.74, 0.0001);
    g_assert_true(codexbar_zenmux_apply_payg(
        zenmux, "{\"success\":true,\"data\":{\"currency\":\"usd\",\"total_credits\":10}}", &error));
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(zenmux->credits_remaining, 10.0, 0.0001);
    g_assert_false(codexbar_zenmux_apply_payg(
        zenmux, "{\"success\":true,\"data\":{\"currency\":\"eur\",\"total_credits\":10}}", &error));
    g_assert_error(error, g_quark_from_static_string("codexbar-simple-provider-error"), 10);
    g_clear_error(&error);
    codexbar_provider_free(zenmux);

    zenmux = codexbar_zenmux_parse_subscription(
        "{\"success\":true,\"data\":{\"plan\":{},\"quota_5_hour\":{},\"quota_7_day\":{}}}", &error);
    g_assert_null(zenmux);
    g_assert_error(error, g_quark_from_static_string("codexbar-simple-provider-error"), 9);
    g_clear_error(&error);
}

static void test_codex_rate_limits(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_codex_parse_rate_limits(
        "{\"id\":2,\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":28,\"windowDurationMins\":300,\"resetsAt\":1776216359},\"secondary\":{\"usedPercent\":71,\"windowDurationMins\":10080,\"resetsAt\":1776395384},\"credits\":{\"hasCredits\":true,\"balance\":\"12.5\"}}}}",
        &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(provider->primary.used_percent, 28.0, 0.0001);
    g_assert_cmpfloat_with_epsilon(provider->secondary.used_percent, 71.0, 0.0001);
    g_assert_cmpfloat_with_epsilon(provider->credits_remaining, 12.5, 0.0001);
    g_assert_nonnull(strstr(provider->primary.reset_description, "2026"));

    g_assert_true(codexbar_codex_apply_account(
        provider,
        "{\"id\":3,\"result\":{\"account\":{\"type\":\"chatgpt\",\"email\":\"dev@example.com\",\"planType\":\"pro\"}}}",
        &error));
    g_assert_no_error(error);
    g_assert_cmpstr(provider->account, ==, "dev@example.com");
    g_assert_cmpstr(provider->plan, ==, "Pro");
    codexbar_provider_free(provider);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/model/parse-snapshot", test_parse_snapshot);
    g_test_add_func("/model/reject-non-array", test_rejects_non_array);
    g_test_add_func("/http/endpoint-policy", test_endpoint_policy);
    g_test_add_func("/http/post-and-response-headers", test_http_post_and_response_headers);
    g_test_add_func("/http/rejects-malformed-requests", test_http_rejects_malformed_requests);
    g_test_add_func("/http/redirect-policy", test_http_redirect_policy);
    g_test_add_func("/config/extended-provider-fields", test_extended_provider_config);
    g_test_add_func("/render/waybar", test_waybar_rendering);
    g_test_add_func("/provider/openrouter-credits", test_openrouter_credits);
    g_test_add_func("/provider/simple-parsers", test_simple_provider_parsers);
    g_test_add_func("/provider/codex-rate-limits", test_codex_rate_limits);
    return g_test_run();
}
