#include "codex.h"
#include "codebuff.h"
#include "config.h"
#include "http.h"
#include "kimi.h"
#include "model.h"
#include "openrouter.h"
#include "provider_registry.h"
#include "proxy_providers.h"
#include "render.h"
#include "simple_providers.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <sys/stat.h>

typedef struct {
    GSocketListener *listener;
    char *request;
} HttpTestServer;

typedef struct {
    GSocketListener *listener;
    guint16 port;
    gboolean invalid_target;
} RedirectTestServer;

typedef struct {
    GSocketListener *listener;
    GMutex mutex;
    GCond condition;
    gboolean request_received;
} CancellationTestServer;

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

static gpointer serve_cancellation_test(gpointer data) {
    CancellationTestServer *server = data;
    GSocketConnection *connection = accept_http_connection(server->listener);
    g_mutex_lock(&server->mutex);
    server->request_received = TRUE;
    g_cond_signal(&server->condition);
    g_mutex_unlock(&server->mutex);

    char buffer[64];
    GInputStream *input = g_io_stream_get_input_stream(G_IO_STREAM(connection));
    while (g_input_stream_read(input, buffer, sizeof(buffer), NULL, NULL) > 0) {}
    g_object_unref(connection);
    return NULL;
}

static gpointer cancel_received_request(gpointer data) {
    gpointer *values = data;
    CancellationTestServer *server = values[0];
    GCancellable *cancellable = values[1];
    g_mutex_lock(&server->mutex);
    while (!server->request_received) g_cond_wait(&server->condition, &server->mutex);
    g_mutex_unlock(&server->mutex);
    g_cancellable_cancel(cancellable);
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
    "\"primary\":{\"label\":\"session\",\"usedPercent\":28,\"windowDurationMins\":300,"
    "\"resetDescription\":\"28 / 100 requests\","
    "\"resetsAt\":\"resets Thu, Jul 23 at 10:16\"},"
    "\"secondary\":{\"usedPercent\":71.4,\"resetDescription\":\"Resets Friday\","
    "\"resetsAt\":1776216359},"
    "\"tertiary\":null,"
    "\"extraRateWindows\":[{\"id\":\"sonnet\",\"title\":\"Sonnet\","
    "\"window\":{\"usedPercent\":0},\"usageKnown\":true}]},"
    "\"credits\":{\"label\":\"balance\",\"remaining\":12.5}"
    "},{"
    "\"provider\":\"claude\","
    "\"source\":\"cli\","
    "\"usage\":{\"primary\":{\"usedPercent\":91}},"
    "\"error\":null"
    "}]";

static CodexBarQuotaWindow *window_at(const CodexBarProvider *provider, guint index) {
    CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, index);
    g_assert_nonnull(window);
    return window;
}

static CodexBarBalance *balance_at(const CodexBarProvider *provider, guint index) {
    CodexBarBalance *balance = codexbar_provider_balance(provider, index);
    g_assert_nonnull(balance);
    return balance;
}

static void test_usage_percent_display_normalization(void) {
    const double boundaries[] = {0.0, 0.25, 100.0};
    for (guint index = 0; index < G_N_ELEMENTS(boundaries); index++) {
        CodexBarUsagePercent percent = codexbar_usage_percent_from_raw(boundaries[index]);
        g_assert_cmpfloat(percent.raw, ==, boundaries[index]);
        g_assert_cmpfloat(codexbar_usage_percent_display(percent), ==, boundaries[index]);
    }

    CodexBarUsagePercent overage = codexbar_usage_percent_from_ratio(150.0, 100.0);
    g_assert_cmpfloat(overage.raw, ==, 150.0);
    g_assert_cmpfloat(codexbar_usage_percent_display(overage), ==, 100.0);

    CodexBarUsagePercent negative = codexbar_usage_percent_from_ratio(-1.0, 100.0);
    g_assert_cmpfloat(negative.raw, ==, -1.0);
    g_assert_cmpfloat(codexbar_usage_percent_display(negative), ==, 0.0);

    g_assert_cmpfloat(codexbar_usage_percent_display(codexbar_usage_percent_from_raw(NAN)), ==, 0.0);
    g_assert_cmpfloat(codexbar_usage_percent_display(codexbar_usage_percent_from_raw(INFINITY)), ==, 100.0);
    g_assert_cmpfloat(codexbar_usage_percent_display(codexbar_usage_percent_from_raw(-INFINITY)), ==, 0.0);
}

static void test_raw_usage_overage_has_clamped_waybar_projection(void) {
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(
        "[{\"provider\":\"copilot\",\"usage\":{\"primary\":{\"usedPercent\":115}}}]", &error);
    g_assert_no_error(error);
    CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, 0);
    g_assert_cmpfloat(window_at(provider, 0)->used_percent, ==, 115.0);

    char *rendered = codexbar_render_waybar(snapshot);
    json_object *object = json_tokener_parse(rendered);
    json_object *percentage = NULL;
    json_object *tooltip = NULL;
    g_assert_true(json_object_object_get_ex(object, "percentage", &percentage));
    g_assert_cmpint(json_object_get_int(percentage), ==, 100);
    g_assert_true(json_object_object_get_ex(object, "tooltip", &tooltip));
    g_assert_nonnull(strstr(json_object_get_string(tooltip), "100% used · 0% left"));
    json_object_put(object);
    g_free(rendered);

    char *usage_text = codexbar_render_usage_text(snapshot);
    g_assert_nonnull(strstr(usage_text, "100% used"));
    g_assert_null(strstr(usage_text, "115% used"));
    g_free(usage_text);
    codexbar_snapshot_free(snapshot);
}

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
    g_assert_cmpuint(codex->quota_windows->len, ==, 3);
    CodexBarQuotaWindow *window = window_at(codex, 0);
    g_assert_cmpstr(window->id, ==, "primary");
    g_assert_cmpstr(window->title, ==, "session");
    g_assert_true(window->usage_known);
    g_assert_cmpfloat(window->used_percent, ==, 28.0);
    g_assert_true(window->has_window_minutes);
    g_assert_cmpint(window->window_minutes, ==, 300);
    g_assert_cmpstr(window->detail, ==, "28 / 100 requests");
    g_assert_cmpstr(window->reset_description, ==, "resets Thu, Jul 23 at 10:16");
    g_assert_true(window_at(codex, 1)->has_resets_at);
    g_assert_cmpint(window_at(codex, 1)->resets_at_ms, ==, G_GINT64_CONSTANT(1776216359000));
    g_assert_cmpstr(window_at(codex, 2)->id, ==, "sonnet");
    g_assert_cmpstr(window_at(codex, 2)->title, ==, "Sonnet");
    g_assert_cmpuint(codex->balances->len, ==, 1);
    CodexBarBalance *balance = balance_at(codex, 0);
    g_assert_cmpstr(balance->id, ==, "credits");
    g_assert_cmpstr(balance->title, ==, "balance");
    g_assert_cmpfloat(balance->remaining, ==, 12.5);
    g_assert_cmpfloat(codexbar_snapshot_highest_used(snapshot), ==, 91.0);
    codexbar_snapshot_free(snapshot);
}

static void test_data_confidence_values(void) {
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(
        "[{\"provider\":\"test\",\"usage\":{\"dataConfidence\":\"percentOnly\"}}]", &error);
    g_assert_no_error(error);
    CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, 0);
    json_object *value = NULL;
    g_assert_true(json_object_object_get_ex(provider->usage_extensions, "dataConfidence", &value));
    g_assert_cmpstr(json_object_get_string(value), ==, "percentOnly");
    codexbar_snapshot_free(snapshot);

    snapshot = codexbar_snapshot_parse(
        "[{\"provider\":\"test\",\"usage\":{\"dataConfidence\":\"exact\\u0000ignored\"}}]", &error);
    g_assert_no_error(error);
    provider = g_ptr_array_index(snapshot->providers, 0);
    g_assert_null(provider->usage_extensions);
    codexbar_snapshot_free(snapshot);
}

static void test_parse_canonical_collections(void) {
    const char *json =
        "[{\"provider\":\"dynamic\",\"quotaWindows\":["
        "{\"id\":\"hourly\",\"title\":\"hourly\",\"usageKnown\":true,\"usedPercent\":12.5,"
        "\"windowMinutes\":60,\"resetsAt\":1776216359000,\"detail\":\"25 / 200\"},"
        "{\"id\":\"daily\",\"title\":\"daily\",\"usageKnown\":true,\"usedPercent\":42,"
        "\"resetsAt\":\"2026-04-15T05:25:59.123Z\"},"
        "{\"id\":\"monthly\",\"title\":\"monthly\",\"usageKnown\":false},"
        "{\"id\":\"extra\",\"title\":\"extra\",\"usageKnown\":true,\"usedPercent\":88},"
        "{\"id\":\"hourly\",\"title\":\"hourly updated\",\"usageKnown\":true,\"usedPercent\":50}],"
        "\"usage\":{\"primary\":{\"usedPercent\":99}},"
        "\"balances\":[{\"id\":\"payg\",\"title\":\"pay as you go\",\"remaining\":7.5,"
        "\"unit\":\"USD\",\"used\":2.5,\"limit\":10,\"expiry\":1776216359}],"
        "\"credits\":{\"remaining\":99}}]";
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(json, &error);
    g_assert_no_error(error);
    CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, 0);
    g_assert_cmpuint(provider->quota_windows->len, ==, 4);
    g_assert_cmpuint(provider->balances->len, ==, 1);
    CodexBarQuotaWindow *hourly = window_at(provider, 0);
    g_assert_cmpstr(hourly->title, ==, "hourly updated");
    g_assert_cmpfloat(hourly->used_percent, ==, 50.0);
    CodexBarQuotaWindow *daily = window_at(provider, 1);
    g_assert_cmpint(daily->resets_at_ms, ==, G_GINT64_CONSTANT(1776230759123));
    g_assert_false(window_at(provider, 2)->usage_known);
    CodexBarBalance *balance = balance_at(provider, 0);
    g_assert_cmpstr(balance->unit, ==, "USD");
    g_assert_true(balance->has_used);
    g_assert_cmpfloat(balance->used, ==, 2.5);
    g_assert_true(balance->has_limit);
    g_assert_cmpfloat(balance->limit, ==, 10.0);
    g_assert_true(balance->has_expiry);
    g_assert_cmpint(balance->expiry_ms, ==, G_GINT64_CONSTANT(1776216359000));
    g_assert_cmpfloat(codexbar_snapshot_highest_used(snapshot), ==, 88.0);
    char *rendered = codexbar_render_waybar(snapshot);
    json_object *rendered_object = json_tokener_parse(rendered);
    json_object *tooltip = NULL;
    g_assert_true(json_object_object_get_ex(rendered_object, "tooltip", &tooltip));
    const char *tooltip_text = json_object_get_string(tooltip);
    g_assert_nonnull(strstr(tooltip_text, "hourly"));
    g_assert_nonnull(strstr(tooltip_text, "daily"));
    g_assert_nonnull(strstr(tooltip_text, "monthly"));
    g_assert_nonnull(strstr(tooltip_text, "extra"));
    g_assert_nonnull(strstr(tooltip_text, "2026 at"));
    g_assert_nonnull(strstr(tooltip_text, "pay as y 7.50 USD left"));
    json_object_put(rendered_object);
    g_free(rendered);
    codexbar_snapshot_free(snapshot);

}

static void test_empty_canonical_collections_fall_back_to_legacy(void) {
    const char *json =
        "[{\"provider\":\"mixed\",\"quotaWindows\":[],"
        "\"usage\":{\"primary\":{\"usedPercent\":42}},\"balances\":[],"
        "\"credits\":{\"remaining\":9}}]";
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(json, &error);
    g_assert_no_error(error);
    const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, 0);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    g_assert_cmpfloat(window_at(provider, 0)->used_percent, ==, 42.0);
    g_assert_cmpuint(provider->balances->len, ==, 1);
    g_assert_cmpfloat(balance_at(provider, 0)->remaining, ==, 9.0);
    codexbar_snapshot_free(snapshot);
}

static void test_codex_credit_limit_legacy_mapping(void) {
    const char *json =
        "[{\"provider\":\"codex\",\"credits\":{\"remaining\":99,\"codexCreditLimit\":{"
        "\"title\":\"Monthly credit limit\",\"used\":40,\"limit\":100,\"remaining\":60,"
        "\"remainingPercent\":60,\"resetsAt\":1776216359}}}]";
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(json, &error);
    g_assert_no_error(error);
    const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, 0);
    g_assert_cmpuint(provider->balances->len, ==, 2);
    g_assert_cmpstr(balance_at(provider, 0)->id, ==, "credits");
    g_assert_cmpfloat(balance_at(provider, 0)->remaining, ==, 99.0);
    const CodexBarBalance *balance = balance_at(provider, 1);
    g_assert_cmpstr(balance->id, ==, "codex-credit-limit");
    g_assert_cmpstr(balance->title, ==, "Monthly credit limit");
    g_assert_cmpfloat(balance->remaining, ==, 60.0);
    g_assert_true(balance->has_used);
    g_assert_cmpfloat(balance->used, ==, 40.0);
    g_assert_true(balance->has_limit);
    g_assert_cmpfloat(balance->limit, ==, 100.0);
    g_assert_true(balance->has_resets_at);
    g_assert_true(balance->has_remaining_percent);
    g_assert_cmpfloat(balance->remaining_percent, ==, 60.0);
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

static void test_http_cancellation(void) {
    GError *error = NULL;
    CancellationTestServer server = {.listener = g_socket_listener_new()};
    g_mutex_init(&server.mutex);
    g_cond_init(&server.condition);
    guint16 port = g_socket_listener_add_any_inet_port(server.listener, NULL, &error);
    g_assert_no_error(error);
    GThread *server_thread = g_thread_new("http-cancellation-server", serve_cancellation_test, &server);
    GCancellable *cancellable = g_cancellable_new();
    gpointer cancel_data[] = {&server, cancellable};
    GThread *cancel_thread = g_thread_new("http-cancellation-trigger", cancel_received_request, cancel_data);

    char *url = g_strdup_printf("http://127.0.0.1:%u/wait", port);
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .timeout_seconds = 10,
        .protocol_policy = CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    gint64 started = g_get_monotonic_time();
    CodexBarHttpResponse *response = codexbar_http_send(&request, &error);
    gint64 elapsed = g_get_monotonic_time() - started;
    g_assert_null(response);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpint(elapsed, <, 3 * G_USEC_PER_SEC);
    g_clear_error(&error);

    g_thread_join(cancel_thread);
    g_thread_join(server_thread);
    g_free(url);
    g_object_unref(cancellable);
    g_object_unref(server.listener);
    g_cond_clear(&server.condition);
    g_mutex_clear(&server.mutex);
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
        "\"awsAuthMode\":\"profile\"},{\"id\":\"poe\",\"enabled\":true},"
        "{\"id\":\"deepinfra\",\"enabled\":true,\"apiKey\":\"valid\\u0000bad\"}]}";
    g_assert_true(g_file_set_contents(path, json, -1, &error));
    g_assert_no_error(error);
    const char *previous = g_getenv("CODEXBAR_CONFIG");
    char *saved = previous ? g_strdup(previous) : NULL;
    g_setenv("CODEXBAR_CONFIG", path, TRUE);
    CodexBarConfig *config = codexbar_config_load(&error);
    g_assert_no_error(error);
    g_assert_nonnull(config);
    g_assert_cmpuint(config->providers->len, ==, codexbar_provider_registry_count());
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
    provider = codexbar_config_provider(config, "deepinfra");
    g_assert_null(provider->api_key);
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

static void test_config_normalization_and_secure_persistence(void) {
    GError *error = NULL;
    char *cwd = g_get_current_dir();
    char *directory = g_build_filename(cwd, "codexbar-config-store-test", NULL);
    char *path = g_build_filename(directory, "config.json", NULL);
    g_free(cwd);
    g_assert_cmpint(g_mkdir(directory, 0755), ==, 0);
    const char json[] =
        "{\"version\":99,\"custom\":{\"keep\":true},\"providers\":["
        "{\"id\":\"openrouter\",\"enabled\":false,\"tokenAccounts\":{\"keep\":true}},"
        "{\"id\":\"openrouter\",\"enabled\":true}]}";
    g_assert_true(g_file_set_contents(path, json, -1, &error));
    g_assert_no_error(error);
    const char *previous = g_getenv("CODEXBAR_CONFIG");
    char *saved = previous ? g_strdup(previous) : NULL;
    g_setenv("CODEXBAR_CONFIG", path, TRUE);

    CodexBarConfig *config = codexbar_config_load_for_update(&error);
    g_assert_no_error(error);
    g_assert_nonnull(config);
    g_assert_cmpint(config->version, ==, 99);
    g_assert_cmpuint(config->providers->len, ==, codexbar_provider_registry_count());
    CodexBarProviderConfig *openrouter = codexbar_config_provider(config, "or");
    g_assert_nonnull(openrouter);
    g_assert_false(openrouter->enabled);
    g_assert_true(codexbar_config_set_api_key(
        config, "or", "  test-secret  ", strlen("  test-secret  "), TRUE, &error));
    g_assert_no_error(error);
    g_assert_cmpstr(openrouter->api_key, ==, "test-secret");
    g_assert_true(openrouter->enabled);
    g_assert_true(codexbar_config_save(config, &error));
    g_assert_no_error(error);
    codexbar_config_free(config);

    struct stat file_stat = {0};
    struct stat directory_stat = {0};
    g_assert_cmpint(g_stat(path, &file_stat), ==, 0);
    g_assert_cmpint(g_stat(directory, &directory_stat), ==, 0);
    g_assert_cmpint(file_stat.st_mode & 0777, ==, 0600);
    g_assert_cmpint(directory_stat.st_mode & 0777, ==, 0755);

    char *contents = NULL;
    g_assert_true(g_file_get_contents(path, &contents, NULL, &error));
    g_assert_no_error(error);
    json_object *root = json_tokener_parse(contents);
    g_free(contents);
    json_object *custom = NULL;
    json_object *providers = NULL;
    g_assert_true(json_object_object_get_ex(root, "custom", &custom));
    g_assert_true(json_object_object_get_ex(root, "providers", &providers));
    g_assert_cmpuint(json_object_array_length(providers), ==, codexbar_provider_registry_count());
    json_object *first = json_object_array_get_idx(providers, 0);
    json_object *token_accounts = NULL;
    json_object *api_key = NULL;
    g_assert_true(json_object_object_get_ex(first, "tokenAccounts", &token_accounts));
    g_assert_true(json_object_object_get_ex(first, "apiKey", &api_key));
    g_assert_cmpstr(json_object_get_string(api_key), ==, "test-secret");
    json_object_put(root);

    config = codexbar_config_load_for_update(&error);
    g_assert_no_error(error);
    g_assert_cmpint(config->version, ==, 1);
    g_assert_false(codexbar_config_set_api_key(
        config, "bedrock", "not-valid-for-bedrock", strlen("not-valid-for-bedrock"), TRUE, &error));
    g_assert_error(error, g_quark_from_static_string("codexbar-config-error"), 4);
    g_clear_error(&error);
    g_assert_true(codexbar_config_set_enabled(config, "deepseek", TRUE, &error));
    g_assert_no_error(error);
    g_assert_true(g_file_set_contents(path, "{\"version\":1,\"providers\":[]}", -1, &error));
    g_assert_no_error(error);
    g_assert_false(codexbar_config_save(config, &error));
    g_assert_error(error, g_quark_from_static_string("codexbar-config-error"), 6);
    g_clear_error(&error);
    char *nested_directory = g_build_filename(directory, "owned", NULL);
    char *nested_path = g_build_filename(nested_directory, "config.json", NULL);
    g_free(config->path);
    config->path = g_strdup(nested_path);
    config->loaded_from_disk = FALSE;
    g_clear_pointer(&config->loaded_digest, g_free);
    g_assert_true(codexbar_config_save(config, &error));
    g_assert_no_error(error);
    g_assert_cmpint(g_stat(nested_path, &file_stat), ==, 0);
    g_assert_cmpint(g_stat(nested_directory, &directory_stat), ==, 0);
    g_assert_cmpint(file_stat.st_mode & 0777, ==, 0600);
    g_assert_cmpint(directory_stat.st_mode & 0777, ==, 0700);
    codexbar_config_free(config);

    if (saved) {
        g_setenv("CODEXBAR_CONFIG", saved, TRUE);
    } else {
        g_unsetenv("CODEXBAR_CONFIG");
    }
    g_free(saved);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_assert_cmpint(g_remove(nested_path), ==, 0);
    g_assert_cmpint(g_rmdir(nested_directory), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_free(nested_path);
    g_free(nested_directory);
    g_free(path);
    g_free(directory);
}

static void test_config_skips_removed_and_unknown_providers(void) {
    GError *error = NULL;
    char *cwd = g_get_current_dir();
    char *path = g_build_filename(cwd, "codexbar-config-removed-providers-test.json", NULL);
    g_free(cwd);
    const char json[] =
        "{\"version\":1,\"providers\":["
        "{\"id\":\"kimik2\",\"enabled\":true},"
        "{\"id\":\"crossmodel\",\"enabled\":true},"
        "{\"id\":\"or\",\"enabled\":true},"
        "{\"id\":\"future-provider\",\"enabled\":true},"
        "{\"id\":\"codex\",\"enabled\":false,\"source\":\"oauth\"}]}";
    g_assert_true(g_file_set_contents(path, json, -1, &error));
    g_assert_no_error(error);
    const char *previous = g_getenv("CODEXBAR_CONFIG");
    char *saved = previous ? g_strdup(previous) : NULL;
    g_setenv("CODEXBAR_CONFIG", path, TRUE);

    CodexBarConfig *config = codexbar_config_load(&error);
    g_assert_no_error(error);
    g_assert_nonnull(config);
    g_assert_cmpuint(config->providers->len, ==, codexbar_provider_registry_count());
    CodexBarProviderConfig *codex = codexbar_config_provider(config, "codex");
    g_assert_nonnull(codex);
    g_assert_false(codex->enabled);
    g_assert_cmpstr(codex->source, ==, "oauth");
    g_assert_null(codexbar_config_provider(config, "kimik2"));
    g_assert_null(codexbar_config_provider(config, "crossmodel"));
    char *rendered = codexbar_config_render_json(config, FALSE);
    g_assert_null(strstr(rendered, "kimik2"));
    g_assert_null(strstr(rendered, "crossmodel"));
    g_assert_null(strstr(rendered, "future-provider"));
    g_free(rendered);
    codexbar_config_free(config);

    g_assert_true(g_file_set_contents(path, "{\"providers\":[{\"enabled\":true}]}", -1, &error));
    g_assert_no_error(error);
    config = codexbar_config_load(&error);
    g_assert_null(config);
    g_assert_error(error, g_quark_from_static_string("codexbar-config-error"), 2);
    g_clear_error(&error);
    config = codexbar_config_load(NULL);
    g_assert_null(config);

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

static void test_claude_presentation_metadata(void) {
    const char *json =
        "[{\"provider\":\"claude\",\"account\":\"owner@example.com\",\"plan\":\"Max\","
        "\"source\":\"oauth\",\"status\":{\"indicator\":\"minor\","
        "\"description\":\"Elevated API errors\",\"url\":\"https://status.anthropic.com\","
        "\"updatedAt\":\"2020-07-17T10:00:00Z\"},\"usage\":{"
        "\"primary\":{\"label\":\"Session\",\"usedPercent\":42,\"windowMinutes\":300,"
        "\"resetsAt\":\"2026-07-17T15:00:00Z\"},"
        "\"secondary\":{\"label\":\"Weekly\",\"usedPercent\":74,\"windowMinutes\":10080,"
        "\"resetsAt\":\"2026-07-21T10:00:00Z\"},"
        "\"extraRateWindows\":[{\"id\":\"sonnet\",\"title\":\"Sonnet\","
        "\"window\":{\"usedPercent\":18},\"usageKnown\":true}],"
        "\"updatedAt\":\"2020-07-17T09:55:00Z\","
        "\"identity\":{\"providerID\":\"claude\",\"accountEmail\":\"fallback@example.com\","
        "\"accountOrganization\":\"Example Labs\",\"loginMethod\":\"OAuth\","
        "\"accountID\":\"acct_claude_123\"},"
        "\"subscriptionExpiresAt\":\"2026-08-01T00:00:00Z\","
        "\"subscriptionRenewsAt\":\"2026-07-24T00:00:00Z\","
        "\"providerCost\":{\"used\":12.5,\"limit\":50,\"currencyCode\":\"USD\","
        "\"period\":\"Monthly\",\"resetsAt\":\"2026-08-01T00:00:00Z\","
        "\"nextRegenAmount\":5,\"personalUsed\":7.25,"
        "\"updatedAt\":\"2020-07-17T09:54:00Z\"}},"
        "\"pace\":{\"primary\":{\"stage\":\"slightlyBehind\",\"deltaPercent\":-4,"
        "\"expectedUsedPercent\":46,\"willLastToReset\":true,\"etaSeconds\":null,"
        "\"runOutProbability\":0.12,"
        "\"summary\":\"4% in reserve | Expected 46% used | Lasts until reset\"},"
        "\"secondary\":{\"stage\":\"ahead\",\"deltaPercent\":9,"
        "\"expectedUsedPercent\":65,\"willLastToReset\":false,\"etaSeconds\":172800,"
        "\"runOutProbability\":null,"
        "\"summary\":\"9% in deficit | Expected 65% used | Runs out Friday\"}},"
        "\"tokenCost\":{\"sessionTokens\":123456,\"sessionCostUSD\":1.25,"
        "\"sessionRequests\":14,\"last30DaysTokens\":9876543,\"last30DaysCostUSD\":98.75,"
        "\"last30DaysRequests\":321,\"currencyCode\":\"USD\",\"historyDays\":30,"
        "\"historyLabel\":\"Last 30 days\",\"updatedAt\":\"2020-07-17T09:53:00Z\"}}]";
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(json, &error);
    g_assert_no_error(error);
    g_assert_nonnull(snapshot);
    g_assert_cmpuint(snapshot->providers->len, ==, 1);

    const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, 0);
    g_assert_cmpstr(provider->account, ==, "owner@example.com");
    g_assert_cmpstr(provider->plan, ==, "Max");
    g_assert_cmpstr(provider->identity->organization, ==, "Example Labs");
    g_assert_cmpstr(provider->identity->account_id, ==, "acct_claude_123");
    g_assert_cmpstr(provider->identity->login_method, ==, "OAuth");
    g_assert_true(provider->has_updated_at);
    g_assert_true(provider->has_subscription_expires_at);
    g_assert_true(provider->has_subscription_renews_at);
    g_assert_cmpint(provider->status->indicator, ==, CODEXBAR_STATUS_MINOR);
    g_assert_cmpstr(provider->status->description, ==, "Elevated API errors");
    g_assert_true(provider->status->has_updated_at);

    g_assert_cmpuint(provider->quota_windows->len, ==, 3);
    const CodexBarQuotaWindow *session = window_at(provider, 0);
    const CodexBarQuotaWindow *weekly = window_at(provider, 1);
    g_assert_cmpstr(session->title, ==, "Session");
    g_assert_cmpstr(weekly->title, ==, "Weekly");
    g_assert_cmpstr(window_at(provider, 2)->title, ==, "Sonnet");
    g_assert_nonnull(session->pace);
    g_assert_cmpint(session->pace->stage, ==, CODEXBAR_PACE_SLIGHTLY_BEHIND);
    g_assert_true(session->pace->will_last);
    g_assert_true(session->pace->has_runout_probability);
    g_assert_cmpfloat(session->pace->runout_probability, ==, 0.12);
    g_assert_nonnull(weekly->pace);
    g_assert_cmpint(weekly->pace->stage, ==, CODEXBAR_PACE_AHEAD);
    g_assert_true(weekly->pace->has_eta);
    g_assert_cmpfloat(weekly->pace->eta_seconds, ==, 172800.0);

    g_assert_nonnull(provider->provider_cost);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 12.5);
    g_assert_cmpfloat(provider->provider_cost->limit, ==, 50.0);
    g_assert_cmpstr(provider->provider_cost->period, ==, "Monthly");
    g_assert_true(provider->provider_cost->has_resets_at);
    g_assert_true(provider->provider_cost->has_next_regen);
    g_assert_true(provider->provider_cost->has_personal_used);
    g_assert_true(provider->provider_cost->has_updated_at);

    g_assert_nonnull(provider->token_cost);
    g_assert_true(provider->token_cost->has_today_tokens);
    g_assert_cmpint(provider->token_cost->today_tokens, ==, 123456);
    g_assert_true(provider->token_cost->has_last_days_cost);
    g_assert_cmpfloat(provider->token_cost->last_days_cost, ==, 98.75);
    g_assert_cmpstr(provider->token_cost->history_label, ==, "Last 30 days");
    g_assert_true(provider->token_cost->has_updated_at);
    g_assert_cmpfloat(codexbar_provider_highest_used(provider), ==, 74.0);

    char *rendered = codexbar_render_waybar(snapshot);
    json_object *object = json_tokener_parse(rendered);
    json_object *tooltip = NULL;
    g_assert_true(json_object_object_get_ex(object, "tooltip", &tooltip));
    const char *text = json_object_get_string(tooltip);
    g_assert_nonnull(strstr(text, "claude · owner@example.com"));
    g_assert_nonnull(strstr(text, "Max · oauth"));
    g_assert_nonnull(strstr(text, "Example Labs"));
    g_assert_nonnull(strstr(text, "acct_claude_123"));
    g_assert_nonnull(strstr(text, "updated Fri, Jul 17 2020"));
    g_assert_nonnull(strstr(text, "subscription expires"));
    g_assert_nonnull(strstr(text, "Pace: 4% in reserve"));
    g_assert_nonnull(strstr(text, "Pace: 9% in deficit"));
    g_assert_nonnull(strstr(text, "Extra usage  $12.50 / $50.00 · Monthly"));
    g_assert_nonnull(strstr(text, "Cost\n    Today"));
    g_assert_nonnull(strstr(text, "$1.25 · 123K tokens · 14 requests"));
    g_assert_nonnull(strstr(text, "Last 30 days"));
    g_assert_nonnull(strstr(text, "$98.75 · 9.88M tokens · 321 requests"));
    g_assert_nonnull(strstr(text, "status    Partial outage · Elevated API errors"));
    g_assert_nonnull(strstr(text, "https://status.anthropic.com"));

    json_object_put(object);
    g_free(rendered);

    char *usage_json = codexbar_render_usage_json(snapshot, FALSE);
    CodexBarSnapshot *round_trip = codexbar_snapshot_parse(usage_json, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(round_trip->providers->len, ==, 1);
    const CodexBarProvider *round_trip_provider = g_ptr_array_index(round_trip->providers, 0);
    g_assert_cmpstr(round_trip_provider->account, ==, "owner@example.com");
    g_assert_cmpuint(round_trip_provider->quota_windows->len, ==, 3);
    g_assert_cmpfloat(codexbar_provider_highest_used(round_trip_provider), ==, 74.0);
    g_assert_nonnull(round_trip_provider->provider_cost);
    g_assert_nonnull(window_at(round_trip_provider, 0)->pace);
    codexbar_snapshot_free(round_trip);
    g_free(usage_json);
    codexbar_snapshot_free(snapshot);
}

static void test_freshness_and_login_method_fallback(void) {
    gint64 now_ms = g_get_real_time() / 1000;
    char *json = g_strdup_printf(
        "[{\"provider\":\"claude\",\"usage\":{\"updatedAt\":%" G_GINT64_FORMAT ","
        "\"identity\":{\"providerID\":\"claude\",\"loginMethod\":\"OAuth\"}}}]",
        now_ms);
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(json, &error);
    g_assert_no_error(error);
    const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, 0);
    g_assert_cmpstr(provider->plan, ==, "OAuth");

    char *rendered = codexbar_render_waybar(snapshot);
    json_object *object = json_tokener_parse(rendered);
    json_object *tooltip = NULL;
    g_assert_true(json_object_object_get_ex(object, "tooltip", &tooltip));
    const char *text = json_object_get_string(tooltip);
    g_assert_nonnull(strstr(text, "\n  OAuth"));
    g_assert_null(strstr(strstr(text, "\n  OAuth") + 1, "\n  OAuth"));
    g_assert_nonnull(strstr(text, "updated just now"));
    json_object_put(object);
    g_free(rendered);
    codexbar_snapshot_free(snapshot);
    g_free(json);

    json = g_strdup_printf(
        "[{\"provider\":\"claude\",\"updatedAt\":%" G_GINT64_FORMAT "}]", now_ms + 120000);
    snapshot = codexbar_snapshot_parse(json, &error);
    g_assert_no_error(error);
    rendered = codexbar_render_waybar(snapshot);
    object = json_tokener_parse(rendered);
    g_assert_true(json_object_object_get_ex(object, "tooltip", &tooltip));
    g_assert_null(strstr(json_object_get_string(tooltip), "updated just now"));
    json_object_put(object);
    g_free(rendered);
    codexbar_snapshot_free(snapshot);
    g_free(json);
}

static void test_identity_provider_siloing_and_operational_status(void) {
    const char *json =
        "[{\"provider\":\"claude\",\"status\":{\"indicator\":\"none\","
        "\"url\":\"https://status.anthropic.com\"},\"usage\":{\"updatedAt\":0,"
        "\"identity\":{\"providerID\":\"codex\",\"accountEmail\":\"wrong@example.com\","
        "\"accountOrganization\":\"Wrong Org\",\"accountID\":\"wrong-id\"}}}]";
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(json, &error);
    g_assert_no_error(error);
    const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, 0);
    g_assert_null(provider->account);
    g_assert_null(provider->identity);
    g_assert_true(provider->has_updated_at);

    char *rendered = codexbar_render_waybar(snapshot);
    json_object *object = json_tokener_parse(rendered);
    json_object *tooltip = NULL;
    g_assert_true(json_object_object_get_ex(object, "tooltip", &tooltip));
    const char *text = json_object_get_string(tooltip);
    g_assert_null(strstr(text, "wrong@example.com"));
    g_assert_null(strstr(text, "Wrong Org"));
    g_assert_null(strstr(text, "Operational"));
    g_assert_null(strstr(text, "status.anthropic.com"));
    json_object_put(object);
    g_free(rendered);
    codexbar_snapshot_free(snapshot);

    snapshot = codexbar_snapshot_parse(
        "[{\"provider\":\"claude\",\"usage\":{\"identity\":{"
        "\"accountEmail\":\"unknown@example.com\",\"loginMethod\":\"OAuth\"}}}]",
        &error);
    g_assert_no_error(error);
    provider = g_ptr_array_index(snapshot->providers, 0);
    g_assert_null(provider->account);
    g_assert_null(provider->identity);
    codexbar_snapshot_free(snapshot);
}

static void test_openrouter_credits(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_openrouter_parse_credits(
        "{\"data\":{\"total_credits\":100,\"total_usage\":27.5}}", &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpfloat_with_epsilon(balance_at(provider, 0)->remaining, 72.5, 0.0001);
    g_assert_cmpfloat_with_epsilon(window_at(provider, 0)->used_percent, 27.5, 0.0001);
    codexbar_provider_free(provider);
}

static void test_kimi_usage(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_kimi_parse_usage(
        "{\"usage\":{\"limit\":\"2048\",\"remaining\":\"1834\","
        "\"resetTime\":\"2026-01-09T15:23:13.716839300Z\"},\"limits\":[{\"detail\":{"
        "\"limit\":200,\"used\":139,\"remaining\":61,\"reset_at\":\"2026-01-06T13:33:02Z\"}}]}",
        G_GINT64_CONSTANT(1800000000000),
        &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->provider, ==, "kimi");
    g_assert_cmpstr(provider->source, ==, "api");
    g_assert_cmpint(provider->updated_at_ms, ==, G_GINT64_CONSTANT(1800000000000));
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    g_assert_cmpfloat_with_epsilon(window_at(provider, 0)->used_percent, 10.44921875, 0.0001);
    g_assert_cmpstr(window_at(provider, 0)->detail, ==, "214/2048 requests");
    g_assert_true(window_at(provider, 0)->has_resets_at);
    g_assert_cmpfloat_with_epsilon(window_at(provider, 1)->used_percent, 69.5, 0.0001);
    g_assert_cmpint(window_at(provider, 1)->window_minutes, ==, 300);
    g_assert_cmpstr(window_at(provider, 1)->detail, ==, "Rate: 139/200 per 5 hours");
    codexbar_provider_free(provider);

    provider = codexbar_kimi_parse_usage(
        "{\"usage\":{\"limit\":0,\"used\":100},\"limits\":[{\"detail\":{\"limit\":0}}]}",
        1,
        &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    g_assert_cmpfloat(window_at(provider, 0)->used_percent, ==, 0.0);
    codexbar_provider_free(provider);

    provider = codexbar_kimi_parse_usage("{\"usage\":{\"limit\":\"1.5\"}}", 1, &error);
    g_assert_null(provider);
    g_assert_error(error, g_quark_from_static_string("codexbar-kimi-error"), 1);
    g_clear_error(&error);

    char *url = codexbar_kimi_usage_url("https://proxy.example.com/kimi/coding/v1/", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "https://proxy.example.com/kimi/coding/v1/usages");
    g_free(url);
    url = codexbar_kimi_usage_url("http://api.kimi.com", &error);
    g_assert_null(url);
    g_assert_nonnull(error);
    g_clear_error(&error);
    url = codexbar_kimi_usage_url("https://api.kimi.com?tenant=one", &error);
    g_assert_null(url);
    g_assert_error(error, g_quark_from_static_string("codexbar-kimi-error"), 2);
    g_clear_error(&error);

    provider = codexbar_kimi_parse_usage(
        "{\"usage\":{\"limit\":9223372036854775807,\"remaining\":-9223372036854775808}}",
        1,
        &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(window_at(provider, 0)->used_percent, ==, 100.0);
    codexbar_provider_free(provider);
}

static void test_clawrouter_usage(void) {
    GError *error = NULL;
    const char *json =
        "{\"budget\":{\"configured\":true,\"ledger\":\"durable_object\","
        "\"windowKey\":\"openclaw/policy/2026-07\",\"limitMicros\":25000000,"
        "\"spentMicros\":6000,\"remainingMicros\":24994000},\"usage\":{\"summary\":{"
        "\"requestCount\":6,\"successCount\":5,\"errorCount\":1,\"inputTokens\":50000,"
        "\"outputTokens\":4191,\"totalTokens\":54191,\"actualCostMicros\":6000},\"providers\":["
        "{\"provider\":\"openai\",\"requestCount\":4,\"successCount\":3,\"errorCount\":1,"
        "\"totalTokens\":42000,\"actualCostMicros\":4000},"
        "{\"provider\":\"anthropic\",\"requestCount\":2,\"successCount\":2,\"errorCount\":0,"
        "\"totalTokens\":12191,\"actualCostMicros\":2000}]}}";
    CodexBarProvider *provider = codexbar_clawrouter_parse(json, 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->identity->organization, ==, "2 routed providers");
    g_assert_cmpstr(provider->identity->login_method, ==, "Managed monthly budget");
    g_assert_cmpfloat_with_epsilon(window_at(provider, 0)->used_percent, 0.024, 0.0001);
    g_assert_cmpint(window_at(provider, 0)->resets_at_ms, ==, G_GINT64_CONSTANT(1785542400000));
    g_assert_cmpfloat_with_epsilon(provider->provider_cost->used, 0.006, 0.0001);
    g_assert_cmpfloat_with_epsilon(provider->provider_cost->limit, 25.0, 0.0001);
    CodexBarSnapshot *snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    g_ptr_array_add(snapshot->providers, provider);
    char *rendered = codexbar_render_usage_json(snapshot, FALSE);
    g_assert_nonnull(strstr(rendered, "\"updatedAt\":\"1970-01-01T00:00:01"));
    g_assert_nonnull(strstr(rendered, "\"clawRouterUsage\":{\"budgetConfigured\":true"));
    g_assert_nonnull(strstr(rendered, "\"dataConfidence\":\"exact\""));
    g_assert_nonnull(strstr(rendered, "\"providers\":[{\"provider\":\"openai\""));
    g_free(rendered);
    codexbar_snapshot_free(snapshot);

    char *url = codexbar_proxy_provider_url("https://router.example.com/v1", "usage", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(url, ==, "https://router.example.com/v1/usage");
    g_free(url);
}

static void test_llmproxy_usage(void) {
    GError *error = NULL;
    const char *json =
        "{\"providers\":{\"openai\":{\"credential_count\":3,\"active_count\":2,"
        "\"exhausted_count\":1,\"total_requests\":120,\"tokens\":{\"input_cached\":1000,"
        "\"input_uncached\":2000,\"output\":3000},\"approx_cost\":12.5,\"quota_groups\":{"
        "\"default\":{\"remaining_percent\":42,\"reset_time\":\"2026-05-18T12:00:00.123Z\"}}},"
        "\"anthropic\":{\"credential_count\":1,\"active_count\":1,\"exhausted_count\":0,"
        "\"total_requests\":40,\"tokens\":{\"input_cached\":0,\"input_uncached\":500,"
        "\"output\":500},\"approx_cost\":3,\"quota_groups\":[{\"remaining_percent\":80}]}},"
        "\"summary\":{\"total_requests\":160,\"total_tokens\":7000,\"approx_cost\":15.5}}";
    CodexBarProvider *provider = codexbar_llmproxy_parse(json, 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->identity->organization, ==, "3/4 active keys");
    g_assert_cmpfloat_with_epsilon(window_at(provider, 0)->used_percent, 58.0, 0.0001);
    g_assert_cmpstr(window_at(provider, 1)->reset_description, ==, "160 requests");
    g_assert_cmpstr(window_at(provider, 2)->reset_description, ==, "7,000 tokens");
    g_assert_cmpstr(window_at(provider, 3)->title, ==, "openai");
    g_assert_cmpstr(window_at(provider, 3)->reset_description, ==, "120 req · 6,000 tok · $12.50");
    g_assert_cmpfloat_with_epsilon(provider->provider_cost->used, 15.5, 0.0001);
    CodexBarSnapshot *snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    g_ptr_array_add(snapshot->providers, provider);
    char *rendered = codexbar_render_usage_json(snapshot, FALSE);
    g_assert_nonnull(strstr(rendered, "\"resetDescription\":\"160 requests\""));
    g_assert_nonnull(strstr(rendered, "\"id\":\"openai\""));
    g_assert_null(strstr(rendered, "llmproxy-openai"));
    g_free(rendered);
    codexbar_snapshot_free(snapshot);

    provider = codexbar_llmproxy_parse("{\"providers\":{}}", 1000, &error);
    g_assert_no_error(error);
    snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    g_ptr_array_add(snapshot->providers, provider);
    rendered = codexbar_render_usage_json(snapshot, FALSE);
    g_assert_nonnull(strstr(rendered, "\"primary\":null"));
    g_assert_nonnull(strstr(rendered, "\"secondary\":{\"usedPercent\":0.0,\"resetDescription\":\"0 requests\""));
    g_assert_nonnull(strstr(rendered, "\"tertiary\":{\"usedPercent\":0.0,\"resetDescription\":\"0 tokens\""));
    g_free(rendered);
    codexbar_snapshot_free(snapshot);
}

static void test_codebuff_usage(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_codebuff_parse_usage(
        "{\"usage\":\"1250\",\"quota\":5000,\"remainingBalance\":3750,"
        "\"autoTopupEnabled\":true,\"next_quota_reset\":\"2026-05-01T00:00:00Z\"}",
        1000,
        &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->provider, ==, "codebuff");
    g_assert_cmpfloat_with_epsilon(window_at(provider, 0)->used_percent, 25.0, 0.0001);
    g_assert_true(window_at(provider, 0)->has_resets_at);
    g_assert_cmpstr(provider->identity->login_method, ==, "3,750 remaining · auto top-up");
    codexbar_provider_free(provider);

    provider = codexbar_codebuff_parse_usage(
        "{\"used\":40,\"remaining\":60,\"autoTopupEnabled\":\"invalid\","
        "\"auto_topup_enabled\":true}",
        1000,
        &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(window_at(provider, 0)->used_percent, 40.0, 0.0001);
    g_assert_cmpstr(provider->identity->login_method, ==, "60 remaining · auto top-up");
    codexbar_provider_free(provider);

    provider = codexbar_codebuff_parse_usage("{\"usage\":42}", 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(window_at(provider, 0)->used_percent, ==, 100.0);
    codexbar_provider_free(provider);

    provider = codexbar_codebuff_parse_usage("{}", 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 0);
    g_assert_nonnull(provider->identity);
    codexbar_provider_free(provider);

    provider = codexbar_codebuff_parse_usage(
        "{\"remainingBalance\":1e100,\"next_quota_reset\":9223372036854775808}", 1000, &error);
    g_assert_no_error(error);
    g_assert_false(window_at(provider, 0)->has_resets_at);
    g_assert_cmpuint(strlen(provider->identity->login_method), >, 100);
    codexbar_provider_free(provider);

    provider = codexbar_codebuff_parse_usage("[]", 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, g_quark_from_static_string("codexbar-codebuff-error"), 1);
    g_clear_error(&error);

    provider = codexbar_codebuff_parse_usage("{\"usage\":25} trailing", 1000, &error);
    g_assert_null(provider);
    g_assert_error(error, g_quark_from_static_string("codexbar-codebuff-error"), 1);
    g_clear_error(&error);
}

static void test_simple_provider_parsers(void) {
    GError *error = NULL;
    CodexBarProvider *deepseek = codexbar_deepseek_parse(
        "{\"is_available\":true,\"balance_infos\":[{\"total_balance\":\"8.25\"}]}", &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(balance_at(deepseek, 0)->remaining, 8.25, 0.0001);
    codexbar_provider_free(deepseek);

    CodexBarProvider *moonshot = codexbar_moonshot_parse(
        "{\"code\":0,\"status\":true,\"data\":{\"available_balance\":12.5}}", &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(balance_at(moonshot, 0)->remaining, 12.5, 0.0001);
    codexbar_provider_free(moonshot);

    CodexBarProvider *elevenlabs = codexbar_elevenlabs_parse(
        "{\"character_count\":250,\"character_limit\":1000}", &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(window_at(elevenlabs, 0)->used_percent, 25.0, 0.0001);
    codexbar_provider_free(elevenlabs);

    CodexBarProvider *crof = codexbar_crof_parse(
        "{\"credits\":9.9999,\"requests_plan\":1000,\"usable_requests\":998}", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(window_at(crof, 0)->title, ==, "requests");
    g_assert_cmpfloat_with_epsilon(window_at(crof, 0)->used_percent, 1.0, 0.0001);
    g_assert_cmpstr(window_at(crof, 0)->detail, ==, "998 requests left");
    g_assert_true(window_at(crof, 0)->has_resets_at);
    g_assert_cmpstr(window_at(crof, 1)->title, ==, "balance");
    g_assert_cmpstr(window_at(crof, 1)->detail, ==, "$9.99");
    codexbar_provider_free(crof);

    crof = codexbar_crof_parse("{\"credits\":0,\"requests_plan\":0,\"usable_requests\":0}", &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(window_at(crof, 0)->used_percent, 100.0, 0.0001);
    codexbar_provider_free(crof);

    crof = codexbar_crof_parse("{\"credits\":0,\"requests_plan\":1000,\"usable_requests\":1200}", &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(window_at(crof, 0)->used_percent, 0.0, 0.0001);
    g_assert_cmpstr(window_at(crof, 0)->detail, ==, "1200 requests left");
    codexbar_provider_free(crof);

    CodexBarProvider *venice = codexbar_venice_parse(
        "{\"canConsume\":true,\"consumptionCurrency\":\"BUNDLED_CREDITS\","
        "\"balances\":{\"diem\":\"50.0\",\"usd\":10.0},\"diemEpochAllocation\":\"100.0\"}",
        &error);
    g_assert_no_error(error);
    g_assert_cmpstr(window_at(venice, 0)->title, ==, "balance");
    g_assert_cmpfloat_with_epsilon(window_at(venice, 0)->used_percent, 50.0, 0.0001);
    g_assert_cmpstr(window_at(venice, 0)->detail, ==, "DIEM 50.00 / 100.00 epoch allocation");
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
    g_assert_cmpfloat_with_epsilon(window_at(venice, 0)->used_percent, 100.0, 0.0001);
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
    g_assert_cmpstr(window_at(zenmux, 0)->title, ==, "5-hour");
    g_assert_cmpfloat_with_epsilon(window_at(zenmux, 0)->used_percent, 7.15, 0.0001);
    g_assert_cmpstr(window_at(zenmux, 0)->detail, ==, "57.20 / 800 flows");
    g_assert_true(window_at(zenmux, 0)->has_resets_at);
    g_assert_cmpstr(window_at(zenmux, 1)->title, ==, "weekly");
    g_assert_true(codexbar_zenmux_apply_payg(
        zenmux, "{\"success\":true,\"data\":{\"currency\":\"usd\",\"total_credits\":482.74}}", &error));
    g_assert_no_error(error);
    g_assert_cmpstr(balance_at(zenmux, 0)->title, ==, "pay as you go");
    g_assert_cmpfloat_with_epsilon(balance_at(zenmux, 0)->remaining, 482.74, 0.0001);
    g_assert_true(codexbar_zenmux_apply_payg(
        zenmux, "{\"success\":true,\"data\":{\"currency\":\"usd\",\"total_credits\":10}}", &error));
    g_assert_no_error(error);
    g_assert_cmpuint(zenmux->balances->len, ==, 1);
    g_assert_cmpfloat_with_epsilon(balance_at(zenmux, 0)->remaining, 10.0, 0.0001);
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
    g_assert_cmpfloat_with_epsilon(window_at(provider, 0)->used_percent, 28.0, 0.0001);
    g_assert_cmpfloat_with_epsilon(window_at(provider, 1)->used_percent, 71.0, 0.0001);
    g_assert_cmpint(window_at(provider, 0)->window_minutes, ==, 300);
    g_assert_cmpint(window_at(provider, 0)->resets_at_ms, ==, G_GINT64_CONSTANT(1776216359000));
    g_assert_cmpfloat_with_epsilon(balance_at(provider, 0)->remaining, 12.5, 0.0001);

    g_assert_true(codexbar_codex_apply_account(
        provider,
        "{\"id\":3,\"result\":{\"account\":{\"type\":\"chatgpt\",\"email\":\"dev@example.com\",\"planType\":\"pro\"}}}",
        &error));
    g_assert_no_error(error);
    g_assert_cmpstr(provider->account, ==, "dev@example.com");
    g_assert_cmpstr(provider->plan, ==, "Pro");
    CodexBarSnapshot *snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    g_ptr_array_add(snapshot->providers, provider);
    char *rendered = codexbar_render_usage_json(snapshot, FALSE);
    g_assert_nonnull(strstr(rendered, "\"loginMethod\":\"Pro\""));
    g_free(rendered);
    codexbar_snapshot_free(snapshot);
}

static void test_provider_registry(void) {
    const char *expected_ids[] = {
        "codex", "openai", "azureopenai", "claude", "clinepass", "cursor", "opencode", "opencodego",
        "alibaba", "alibabatokenplan", "factory", "gemini", "antigravity", "copilot", "devin", "zai",
        "minimax", "manus", "kimi", "kilo", "kiro", "vertexai", "augment", "jetbrains", "moonshot",
        "amp", "t3chat", "ollama", "synthetic", "warp", "openrouter", "elevenlabs", "windsurf", "zed",
        "perplexity", "mimo", "doubao", "sakana", "abacus", "mistral", "deepseek", "deepinfra",
        "codebuff", "crof", "venice", "commandcode", "qoder", "stepfun", "bedrock", "grok", "groq",
        "llmproxy", "litellm", "deepgram", "poe", "chutes", "neuralwatt", "clawrouter", "longcat",
        "sub2api", "wayfinder", "zenmux", "aiand",
    };
    g_assert_cmpuint(codexbar_provider_registry_count(), ==, G_N_ELEMENTS(expected_ids));
    for (guint index = 0; index < G_N_ELEMENTS(expected_ids); index++) {
        const CodexBarProviderDescriptor *provider = codexbar_provider_registry_at(index);
        g_assert_nonnull(provider);
        g_assert_cmpstr(provider->id, ==, expected_ids[index]);
        g_assert_true(codexbar_provider_registry_find(provider->id) == provider);
        g_assert_true(codexbar_provider_registry_find(provider->cli_name) == provider);
        if (provider->aliases) {
            char **aliases = g_strsplit(provider->aliases, ",", -1);
            for (guint alias_index = 0; aliases[alias_index]; alias_index++) {
                g_assert_true(codexbar_provider_registry_find(aliases[alias_index]) == provider);
            }
            g_strfreev(aliases);
        }
    }
    g_assert_null(codexbar_provider_registry_at(G_N_ELEMENTS(expected_ids)));
    g_assert_null(codexbar_provider_registry_find("unknown"));
    g_assert_cmpstr(codexbar_provider_registry_find("aoai")->id, ==, "azureopenai");
    g_assert_cmpstr(codexbar_provider_registry_find("or")->id, ==, "openrouter");
    g_assert_cmpstr(codexbar_provider_registry_find("groq")->id, ==, "groq");
    g_assert_null(codexbar_provider_registry_find("kimiK2"));
    g_assert_null(codexbar_provider_registry_find("crossmodel"));
    g_assert_cmpint(codexbar_provider_registry_find("kimi")->native_provider, ==, CODEXBAR_NATIVE_KIMI);
    g_assert_cmpint(codexbar_provider_registry_find("clawrouter")->native_provider, ==, CODEXBAR_NATIVE_PROXY);
    g_assert_cmpint(codexbar_provider_registry_find("llmproxy")->native_provider, ==, CODEXBAR_NATIVE_PROXY);
    g_assert_cmpint(codexbar_provider_registry_find("codebuff")->native_provider, ==, CODEXBAR_NATIVE_CODEBUFF);
    const CodexBarProviderDescriptor *jetbrains = codexbar_provider_registry_find("jetbrains");
    g_assert_cmpint(jetbrains->native_provider, ==, CODEXBAR_NATIVE_JETBRAINS);
    g_assert_true(codexbar_provider_supports_source(jetbrains, "auto"));
    g_assert_true(codexbar_provider_supports_source(jetbrains, "cli"));
    g_assert_false(codexbar_provider_supports_source(jetbrains, "api"));
    g_assert_false(codexbar_provider_supports_source(jetbrains, "web"));
    g_assert_false(codexbar_provider_supports_source(jetbrains, "oauth"));
    const CodexBarProviderDescriptor *codex = codexbar_provider_registry_find("codex");
    g_assert_true(codex->default_enabled);
    g_assert_true(codexbar_provider_supports_source(codex, "oauth"));
    g_assert_false(codexbar_provider_supports_source(codex, "api"));
    g_assert_true(codexbar_provider_supports_source(codexbar_provider_registry_find("deepseek"), "api"));
    g_assert_false(codexbar_provider_supports_source(codexbar_provider_registry_find("deepseek"), "web"));
    const CodexBarProviderDescriptor *clinepass = codexbar_provider_registry_find("clinepass");
    g_assert_cmpstr(clinepass->dashboard_url, ==, "https://app.cline.bot/dashboard/subscription?personal=true");
    g_assert_cmpint(clinepass->native_provider, ==, CODEXBAR_NATIVE_CLINEPASS);
    g_assert_true(codexbar_provider_supports_source(clinepass, "api"));
    g_assert_true(codexbar_provider_supports_config_api_key(clinepass));
    const CodexBarProviderDescriptor *deepinfra = codexbar_provider_registry_find("di");
    g_assert_cmpstr(deepinfra->id, ==, "deepinfra");
    g_assert_cmpint(deepinfra->native_provider, ==, CODEXBAR_NATIVE_DEEPINFRA);
    g_assert_cmpstr(deepinfra->status_url, ==, "https://status.deepinfra.com");
    g_assert_false(codexbar_provider_status_is_pollable(deepinfra));
    g_assert_true(codexbar_provider_supports_config_api_key(deepinfra));
    const CodexBarProviderDescriptor *neuralwatt = codexbar_provider_registry_find("neural");
    g_assert_cmpstr(neuralwatt->id, ==, "neuralwatt");
    g_assert_true(codexbar_provider_registry_find("nw") == neuralwatt);
    g_assert_true(codexbar_provider_supports_config_api_key(neuralwatt));
    g_assert_true(codexbar_provider_supports_source(neuralwatt, "api"));
    g_assert_cmpint(neuralwatt->native_provider, ==, CODEXBAR_NATIVE_NEURALWATT);
    g_assert_true(codexbar_provider_supports_source(codexbar_provider_registry_find("long-cat"), "web"));
    g_assert_false(codexbar_provider_supports_config_api_key(codexbar_provider_registry_find("longcat")));
    g_assert_true(codexbar_provider_supports_config_api_key(codexbar_provider_registry_find("ai&")));
    g_assert_cmpint(codexbar_provider_registry_find("ai&")->native_provider, ==, CODEXBAR_NATIVE_AIAND);
    g_assert_true(codexbar_provider_status_is_pollable(codex));
    g_assert_false(codexbar_provider_status_is_pollable(codexbar_provider_registry_find("deepseek")));
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/model/usage-percent-display-normalization", test_usage_percent_display_normalization);
    g_test_add_func("/model/raw-overage-waybar-projection", test_raw_usage_overage_has_clamped_waybar_projection);
    g_test_add_func("/model/parse-snapshot", test_parse_snapshot);
    g_test_add_func("/model/data-confidence-values", test_data_confidence_values);
    g_test_add_func("/model/parse-canonical-collections", test_parse_canonical_collections);
    g_test_add_func(
        "/model/empty-canonical-falls-back", test_empty_canonical_collections_fall_back_to_legacy);
    g_test_add_func("/model/codex-credit-limit-legacy", test_codex_credit_limit_legacy_mapping);
    g_test_add_func("/model/reject-non-array", test_rejects_non_array);
    g_test_add_func("/http/endpoint-policy", test_endpoint_policy);
    g_test_add_func("/http/post-and-response-headers", test_http_post_and_response_headers);
    g_test_add_func("/http/rejects-malformed-requests", test_http_rejects_malformed_requests);
    g_test_add_func("/http/redirect-policy", test_http_redirect_policy);
    g_test_add_func("/http/cancellation", test_http_cancellation);
    g_test_add_func("/config/extended-provider-fields", test_extended_provider_config);
    g_test_add_func("/config/normalization-secure-persistence", test_config_normalization_and_secure_persistence);
    g_test_add_func("/config/skips-removed-unknown-providers", test_config_skips_removed_and_unknown_providers);
    g_test_add_func("/render/waybar", test_waybar_rendering);
    g_test_add_func("/render/claude-presentation-metadata", test_claude_presentation_metadata);
    g_test_add_func("/render/freshness-login-fallback", test_freshness_and_login_method_fallback);
    g_test_add_func("/render/provider-siloing-operational-status", test_identity_provider_siloing_and_operational_status);
    g_test_add_func("/provider/openrouter-credits", test_openrouter_credits);
    g_test_add_func("/provider/kimi-usage", test_kimi_usage);
    g_test_add_func("/provider/clawrouter-usage", test_clawrouter_usage);
    g_test_add_func("/provider/llmproxy-usage", test_llmproxy_usage);
    g_test_add_func("/provider/codebuff-usage", test_codebuff_usage);
    g_test_add_func("/provider/simple-parsers", test_simple_provider_parsers);
    g_test_add_func("/provider/codex-rate-limits", test_codex_rate_limits);
    g_test_add_func("/provider/registry", test_provider_registry);
    return g_test_run();
}
