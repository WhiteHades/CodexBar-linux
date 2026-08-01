#include "provider_registry.h"
#include "status.h"

#include <glib.h>
#include <string.h>

static CodexBarHttpResponse *response(long status, const char *body, const char *url) {
    CodexBarHttpResponse *value = g_new0(CodexBarHttpResponse, 1);
    value->status = status;
    value->body = g_strdup(body);
    value->body_length = strlen(body);
    value->headers = g_ptr_array_new();
    value->effective_url = g_strdup(url);
    return value;
}

static gint64 timestamp_ms(const char *text) {
    GDateTime *date = g_date_time_new_from_iso8601(text, NULL);
    g_assert_nonnull(date);
    gint64 result = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return result;
}

static CodexBarStatusComponent *component_at(const GPtrArray *components, guint index) {
    g_assert_nonnull(components);
    g_assert_cmpuint(index, <, components->len);
    return g_ptr_array_index((GPtrArray *)components, index);
}

static void test_statuspage_status(void) {
    const char *json =
        "{\"page\":{\"updated_at\":\"2026-06-18T19:41:22.125Z\"},"
        "\"status\":{\"indicator\":\"minor\",\"description\":\"Partial System Degradation\"}}";
    GError *error = NULL;
    CodexBarServiceStatus *status = codexbar_statuspage_parse_status(json, strlen(json), &error);
    g_assert_no_error(error);
    g_assert_nonnull(status);
    g_assert_cmpint(status->indicator, ==, CODEXBAR_STATUS_MINOR);
    g_assert_cmpstr(status->description, ==, "Partial System Degradation");
    g_assert_true(status->has_updated_at);
    g_assert_cmpint(status->updated_at_ms, ==, timestamp_ms("2026-06-18T19:41:22.125Z"));
    codexbar_service_status_free(status);

    json = "{\"status\":{\"indicator\":\"new_indicator\"}}";
    status = codexbar_statuspage_parse_status(json, strlen(json), &error);
    g_assert_no_error(error);
    g_assert_cmpint(status->indicator, ==, CODEXBAR_STATUS_UNKNOWN);
    g_assert_null(status->description);
    g_assert_false(status->has_updated_at);
    codexbar_service_status_free(status);
}

static void test_statuspage_components(void) {
    const char *json =
        "{\"components\":["
        "{\"id\":\"c-resp\",\"name\":\"Responses\",\"status\":\"operational\","
        "\"group_id\":\"g1\",\"position\":2},"
        "{\"id\":\"blank\",\"name\":\"   \",\"status\":\"operational\",\"position\":0},"
        "{\"id\":\"g1\",\"name\":\" API \",\"status\":\"degraded_performance\","
        "\"group\":true,\"position\":0},"
        "{\"id\":\"c-chat\",\"name\":\"Chat Completions\",\"status\":\"major_outage\","
        "\"group_id\":\"g1\",\"position\":1},"
        "{\"id\":\"c-cli\",\"name\":\"CLI\",\"status\":\"under_maintenance\",\"position\":3}"
        "]}";
    GError *error = NULL;
    GPtrArray *components = codexbar_statuspage_parse_components(json, strlen(json), &error);
    g_assert_no_error(error);
    g_assert_cmpuint(components->len, ==, 2);

    CodexBarStatusComponent *group = component_at(components, 0);
    g_assert_cmpstr(group->name, ==, "API");
    g_assert_cmpint(group->indicator, ==, CODEXBAR_STATUS_MINOR);
    g_assert_cmpuint(group->children->len, ==, 2);
    g_assert_cmpstr(component_at(group->children, 0)->name, ==, "Chat Completions");
    g_assert_cmpint(component_at(group->children, 0)->indicator, ==, CODEXBAR_STATUS_CRITICAL);
    g_assert_cmpstr(component_at(group->children, 1)->name, ==, "Responses");
    g_assert_cmpstr(component_at(components, 1)->name, ==, "CLI");
    g_assert_cmpint(component_at(components, 1)->indicator, ==, CODEXBAR_STATUS_MAINTENANCE);
    g_ptr_array_unref(components);

    components = codexbar_statuspage_parse_components("{}", 2, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(components->len, ==, 0);
    g_ptr_array_unref(components);
}

static void test_incident_io_summary(void) {
    const char *json =
        "{\"summary\":{"
        "\"affected_components\":["
        "{\"component_id\":\"c-fed\",\"status\":\"degraded_performance\"},"
        "{\"component_id\":\"c-top\",\"status\":\"full_outage\"}],"
        "\"structure\":{\"items\":["
        "{\"group\":{\"id\":\"g-codex\",\"name\":\"Codex\",\"hidden\":false,\"components\":["
        "{\"component_id\":\"c-cli\",\"name\":\"CLI\",\"hidden\":false},"
        "{\"component_id\":\"c-secret\",\"name\":\"Hidden\",\"hidden\":true}]}},"
        "{\"group\":{\"id\":\"g-fed\",\"name\":\"FedRAMP\",\"components\":["
        "{\"component_id\":\"c-fed\",\"name\":\"FedRAMP\"}]}},"
        "{\"component\":{\"component_id\":\"c-top\",\"name\":\"Standalone\"}}"
        "]}}}";
    GError *error = NULL;
    CodexBarStatusSummary *summary = codexbar_incident_io_parse_summary(json, strlen(json), &error);
    g_assert_no_error(error);
    g_assert_nonnull(summary);
    g_assert_cmpint(summary->status->indicator, ==, CODEXBAR_STATUS_CRITICAL);
    g_assert_cmpuint(summary->components->len, ==, 3);
    g_assert_cmpuint(component_at(summary->components, 0)->children->len, ==, 1);
    g_assert_cmpint(component_at(summary->components, 0)->indicator, ==, CODEXBAR_STATUS_NONE);
    g_assert_cmpint(component_at(summary->components, 1)->indicator, ==, CODEXBAR_STATUS_MINOR);
    g_assert_cmpstr(component_at(summary->components, 1)->status, ==, "degraded_performance");
    g_assert_cmpint(component_at(summary->components, 2)->indicator, ==, CODEXBAR_STATUS_CRITICAL);
    codexbar_status_summary_free(summary);
}

static void test_workspace_status(void) {
    const char *product_id = "npdyhgECDJ6tB66MxXyo";
    const char *json =
        "["
        "{\"begin\":\"2025-12-02T08:00:00+00:00\",\"end\":\"2025-12-02T09:00:00+00:00\","
        "\"affected_products\":[{\"id\":\"npdyhgECDJ6tB66MxXyo\"}],"
        "\"most_recent_update\":{\"status\":\"SERVICE_OUTAGE\"}},"
        "{\"begin\":\"2025-12-02T09:00:00+00:00\",\"end\":null,"
        "\"currently_affected_products\":[{\"id\":\"different\"}],"
        "\"affected_products\":[{\"id\":\"npdyhgECDJ6tB66MxXyo\"}],"
        "\"most_recent_update\":{\"status\":\"SERVICE_OUTAGE\"}},"
        "{\"begin\":\"2025-12-02T10:00:00+00:00\",\"end\":null,"
        "\"affected_products\":[{\"id\":\"npdyhgECDJ6tB66MxXyo\"}],"
        "\"most_recent_update\":{\"when\":\"2025-12-02T10:15:00.250+00:00\","
        "\"status\":\"SERVICE_INFORMATION\",\"text\":\"**Summary**\\r\\nMinor issue.\"}},"
        "{\"begin\":\"2025-12-02T11:00:00+00:00\",\"end\":null,"
        "\"affected_products\":[{\"id\":\"npdyhgECDJ6tB66MxXyo\"}],"
        "\"updates\":[{\"status\":\"SERVICE_INFORMATION\"},{"
        "\"when\":\"2025-12-02T12:00:00+00:00\",\"status\":\"SERVICE_OUTAGE\","
        "\"text\":\"**Description**\\n- [Gemini API](https://example.test) error.\\n\"}]}"
        "]";
    GError *error = NULL;
    CodexBarServiceStatus *status =
        codexbar_workspace_parse_status(json, strlen(json), product_id, &error);
    g_assert_no_error(error);
    g_assert_nonnull(status);
    g_assert_cmpint(status->indicator, ==, CODEXBAR_STATUS_CRITICAL);
    g_assert_cmpstr(status->description, ==, "Gemini API error.");
    g_assert_true(status->has_updated_at);
    g_assert_cmpint(status->updated_at_ms, ==, timestamp_ms("2025-12-02T12:00:00+00:00"));
    codexbar_service_status_free(status);

    status = codexbar_workspace_parse_status("[]", 2, product_id, &error);
    g_assert_no_error(error);
    g_assert_cmpint(status->indicator, ==, CODEXBAR_STATUS_NONE);
    g_assert_null(status->description);
    codexbar_service_status_free(status);
}

typedef enum {
    FETCH_INCIDENT,
    FETCH_FALLBACK,
    FETCH_WORKSPACE,
} FetchFlow;

static FetchFlow fetch_flow;
static guint request_count;
static GCancellable *expected_cancellable;

static CodexBarHttpResponse *transport(const CodexBarHttpRequest *request, GError **error) {
    request_count++;
    g_assert_cmpint(request->timeout_seconds, ==, 10);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
    g_assert_true(request->cancellable == expected_cancellable);
    if (fetch_flow == FETCH_INCIDENT) {
        if (request_count == 1) {
            g_assert_cmpstr(request->url, ==, "https://status.example.test/proxy/status.example.test");
            return response(200,
                            "{\"summary\":{\"affected_components\":[{\"component_id\":\"api\","
                            "\"status\":\"major_outage\"}],\"structure\":{\"items\":[{\"component\":{"
                            "\"component_id\":\"api\",\"name\":\"API\"}}]}}}",
                            request->url);
        }
        g_assert_cmpstr(request->url, ==, "https://status.example.test/api/v2/status.json");
        return response(200,
                        "{\"page\":{\"updated_at\":\"2026-06-20T10:00:00Z\"},"
                        "\"status\":{\"indicator\":\"minor\",\"description\":\"Elevated errors\"}}",
                        request->url);
    }
    if (fetch_flow == FETCH_FALLBACK) {
        if (request_count == 1) return response(200, "{}", request->url);
        if (request_count == 2) {
            g_assert_cmpstr(request->url, ==, "https://status.example.test/api/v2/summary.json");
            return response(200,
                            "{\"status\":{\"indicator\":\"minor\",\"description\":\"Partial Outage\"}}",
                            request->url);
        }
        g_assert_cmpstr(request->url, ==, "https://status.example.test/api/v2/components.json");
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE, "offline");
        return NULL;
    }
    g_assert_cmpstr(request->url, ==, "https://www.google.com/appsstatus/dashboard/incidents.json");
    return response(200,
                    "[{\"end\":null,\"affected_products\":[{\"id\":\"npdyhgECDJ6tB66MxXyo\"}],"
                    "\"most_recent_update\":{\"status\":\"SERVICE_OUTAGE\"}}]",
                    request->url);
}

static void test_status_fetch_seams(void) {
    expected_cancellable = g_cancellable_new();
    GError *error = NULL;

    fetch_flow = FETCH_INCIDENT;
    request_count = 0;
    CodexBarStatusSummary *summary = codexbar_statuspage_fetch_summary(
        "https://status.example.test/", transport, expected_cancellable, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(request_count, ==, 2);
    g_assert_cmpint(summary->status->indicator, ==, CODEXBAR_STATUS_CRITICAL);
    g_assert_cmpstr(summary->status->description, ==, "Elevated errors");
    g_assert_true(summary->status->has_updated_at);
    codexbar_status_summary_free(summary);

    fetch_flow = FETCH_FALLBACK;
    request_count = 0;
    summary = codexbar_statuspage_fetch_summary(
        "https://status.example.test", transport, expected_cancellable, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(request_count, ==, 3);
    g_assert_cmpint(summary->status->indicator, ==, CODEXBAR_STATUS_MINOR);
    g_assert_cmpstr(summary->status->description, ==, "Partial Outage");
    g_assert_null(summary->components);
    codexbar_status_summary_free(summary);

    fetch_flow = FETCH_WORKSPACE;
    request_count = 0;
    CodexBarServiceStatus *status = codexbar_workspace_fetch_status(
        "npdyhgECDJ6tB66MxXyo", transport, expected_cancellable, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(request_count, ==, 1);
    g_assert_cmpint(status->indicator, ==, CODEXBAR_STATUS_CRITICAL);
    codexbar_service_status_free(status);
    g_object_unref(expected_cancellable);
    expected_cancellable = NULL;
}

static void test_registry_status_sources(void) {
    const char *statuspage[] = {"codex", "openai", "claude", "cursor", "factory", "copilot", "augment", "zoommate"};
    const char *workspace[] = {"gemini", "antigravity"};
    guint pollable_count = 0;
    for (guint index = 0; index < codexbar_provider_registry_count(); index++) {
        const CodexBarProviderDescriptor *provider = codexbar_provider_registry_at(index);
        if (codexbar_provider_status_is_pollable(provider)) pollable_count++;
    }
    g_assert_cmpuint(pollable_count, ==, 10);
    for (guint index = 0; index < G_N_ELEMENTS(statuspage); index++) {
        const CodexBarProviderDescriptor *provider = codexbar_provider_registry_find(statuspage[index]);
        g_assert_cmpint(codexbar_provider_status_source(provider), ==, CODEXBAR_PROVIDER_STATUS_STATUSPAGE);
        g_assert_cmpstr(codexbar_provider_status_source_value(provider), ==, provider->status_url);
    }
    for (guint index = 0; index < G_N_ELEMENTS(workspace); index++) {
        const CodexBarProviderDescriptor *provider = codexbar_provider_registry_find(workspace[index]);
        g_assert_cmpint(codexbar_provider_status_source(provider), ==, CODEXBAR_PROVIDER_STATUS_GOOGLE_WORKSPACE);
        g_assert_cmpstr(codexbar_provider_status_source_value(provider), ==, "npdyhgECDJ6tB66MxXyo");
    }
    g_assert_cmpint(codexbar_provider_status_source(codexbar_provider_registry_find("deepinfra")),
                    ==,
                    CODEXBAR_PROVIDER_STATUS_NONE);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/status/statuspage/status", test_statuspage_status);
    g_test_add_func("/status/statuspage/components", test_statuspage_components);
    g_test_add_func("/status/incident-io/summary", test_incident_io_summary);
    g_test_add_func("/status/workspace/status", test_workspace_status);
    g_test_add_func("/status/fetch/seams", test_status_fetch_seams);
    g_test_add_func("/status/registry/sources", test_registry_status_sources);
    return g_test_run();
}
