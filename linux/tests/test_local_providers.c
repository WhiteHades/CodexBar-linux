#include "local_providers.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

static void test_kiro_parsing(void) {
    const char *usage =
        "\033[1mEstimated Usage\033[0m | resets on 2026-06-01 | KIRO FREE\n"
        "Bonus credits: 45.53/2000 credits used, expires in 19 days\n"
        "Credits (0.17 of 50 covered in plan)\n"
        "████████ 0%\n"
        "Overages: Disabled\n"
        "https://app.kiro.dev/account/usage\n";
    const char *account = "Logged in with Google\nEmail: person@example.com\n";
    const char *context = "Context window: 1.3% used\nContext files 0.5%\nTools 0.8%\n"
                          "Kiro responses 0.0%\nYour prompts 0.0%\n";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_kiro_parse_usage(usage, account, context, 1700000000000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->provider, ==, "kiro");
    g_assert_cmpstr(provider->source, ==, "cli");
    g_assert_cmpstr(provider->plan, ==, "KIRO FREE");
    g_assert_cmpstr(provider->account, ==, "person@example.com");
    g_assert_cmpstr(provider->identity->login_method, ==, "Google");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    CodexBarQuotaWindow *primary = codexbar_provider_quota_window(provider, 0);
    CodexBarQuotaWindow *bonus = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpfloat(primary->used_percent, ==, 0.0);
    g_assert_true(primary->has_resets_at);
    g_assert_cmpfloat_with_epsilon(bonus->used_percent, 2.2765, 0.0001);
    g_assert_cmpstr(bonus->reset_description, ==, "expires in 19d");
    json_object *details = NULL;
    g_assert_true(json_object_object_get_ex(provider->usage_extensions, "kiroUsage", &details));
    g_assert_cmpstr(json_object_get_string(json_object_object_get(details, "displayPlanName")), ==, "Kiro Free");
    g_assert_cmpfloat(json_object_get_double(json_object_object_get(details, "creditsUsed")), ==, 0.17);
    g_assert_cmpstr(json_object_get_string(json_object_object_get(details, "overagesStatus")), ==, "Disabled");
    json_object *context_usage = json_object_object_get(details, "contextUsage");
    g_assert_cmpfloat(json_object_get_double(json_object_object_get(context_usage, "toolsPercent")), ==, 0.8);
    codexbar_provider_free(provider);
}

static void test_kiro_variants_and_errors(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_kiro_parse_usage(
        "| KIRO PRO |\n(40.00 of 50 covered in plan), resets on 02/01\n", NULL, NULL, 1700000000000, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 80.0);
    codexbar_provider_free(provider);

    provider = codexbar_kiro_parse_usage(
        "Plan: Q Developer Pro\nYour plan is managed by admin\n", NULL, NULL, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->plan, ==, "Q Developer Pro");
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 0.0);
    codexbar_provider_free(provider);

    provider = codexbar_kiro_parse_usage("Plan: loading\n", NULL, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    provider = codexbar_kiro_parse_usage("Failed to initialize auth portal. Run kiro-cli login.\n",
                                         NULL, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
}

static void test_augment_parsing(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_augment_parse_cli_usage(
        "319,054 credits remaining                     Max Plan\n"
        "450,000 credits / month\n"
        "9 days remaining in this billing cycle (ends 6/9/2026)\n",
        1700000000000,
        &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->plan, ==, "450000 credits/month");
    CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat_with_epsilon(window->used_percent, 29.099111, 0.0001);
    g_assert_true(window->has_resets_at);
    codexbar_provider_free(provider);

    provider = codexbar_augment_parse_cli_usage(
        "Max Plan 450,000 credits / month\n"
        "11,657 remaining · 953,170 / 964,827 credits used\n"
        "2 days remaining in this billing cycle (ends 1/8/2026)\n",
        1700000000000,
        &error);
    g_assert_no_error(error);
    window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat_with_epsilon(window->used_percent, 98.791804, 0.0001);
    codexbar_provider_free(provider);

    provider = codexbar_augment_parse_cli_usage("Authentication failed. Run auggie login.\n", 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
}

static const char *quota_summary(void) {
    return "{\"response\":{\"groups\":["
           "{\"displayName\":\"Claude and GPT models\",\"buckets\":["
           "{\"bucketId\":\"3p-weekly\",\"displayName\":\"Weekly Limit\","
           "\"remaining\":{\"remainingFraction\":0.64},\"resetTime\":\"2026-06-20T00:39:54Z\"},"
           "{\"bucketId\":\"3p-5h\",\"displayName\":\"Five Hour Limit\","
           "\"remaining\":{\"remainingFraction\":0.73},\"resetTime\":\"2026-06-15T12:52:10Z\"}]},"
           "{\"displayName\":\"Gemini Models\",\"buckets\":["
           "{\"bucketId\":\"gemini-weekly\",\"displayName\":\"Weekly Limit\","
           "\"remaining\":{\"remainingFraction\":0.82},\"resetTime\":\"2026-06-19T08:45:39Z\"},"
           "{\"bucketId\":\"gemini-5h\",\"displayName\":\"Five Hour Limit\","
           "\"remaining\":{\"remainingFraction\":0.91},\"resetTime\":\"2026-06-15T11:39:34Z\"}]}]}}";
}

static const char *user_status(void) {
    return "{\"code\":0,\"userStatus\":{\"email\":\"test@example.com\","
           "\"userTier\":{\"name\":\"Ultra\"},\"planStatus\":{\"planInfo\":{\"planName\":\"Pro\"}},"
           "\"cascadeModelConfigData\":{\"clientModelConfigs\":[{"
           "\"label\":\"Gemini 3 Pro Low\",\"modelOrAlias\":{\"model\":\"gemini-3-pro-low\"},"
           "\"quotaInfo\":{\"remainingFraction\":0.9,\"resetTime\":\"2025-12-24T10:00:00Z\"}}]}}}";
}

static void test_antigravity_summary(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_antigravity_parse_usage(quota_summary(), user_status(), NULL, 42, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->provider, ==, "antigravity");
    g_assert_cmpstr(provider->account, ==, "test@example.com");
    g_assert_cmpstr(provider->plan, ==, "Ultra");
    g_assert_cmpuint(provider->quota_windows->len, ==, 6);
    CodexBarQuotaWindow *primary = codexbar_provider_quota_window(provider, 0);
    CodexBarQuotaWindow *secondary = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpstr(primary->title, ==, "Gemini weekly");
    g_assert_cmpfloat_with_epsilon(primary->used_percent, 18.0, 0.0001);
    g_assert_cmpstr(secondary->title, ==, "Claude/GPT weekly");
    g_assert_cmpfloat_with_epsilon(secondary->used_percent, 36.0, 0.0001);
    CodexBarQuotaWindow *extra = codexbar_provider_quota_window(provider, 2);
    g_assert_cmpstr(extra->output_id, ==, "antigravity-quota-summary-gemini-5h");
    g_assert_cmpstr(extra->title, ==, "Gemini 5-hour");
    g_assert_cmpint(extra->window_minutes, ==, 300);
    codexbar_provider_free(provider);
}

static void test_antigravity_oneof_and_fallback(void) {
    const char *oneof = "{\"groups\":[{\"displayName\":\"Gemini Models\",\"buckets\":[{"
                         "\"bucketId\":\"gemini_session\",\"displayName\":\"Gemini\","
                         "\"remaining\":{\"case\":\"remainingFraction\",\"value\":0.5}}]}]}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_antigravity_parse_usage(oneof, NULL, NULL, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 50.0);
    g_assert_cmpint(codexbar_provider_quota_window(provider, 1)->window_minutes, ==, 300);
    codexbar_provider_free(provider);

    provider = codexbar_antigravity_parse_usage("{\"code\":16}", user_status(), NULL, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    g_assert_cmpfloat_with_epsilon(codexbar_provider_quota_window(provider, 0)->used_percent, 10.0, 0.0001);
    codexbar_provider_free(provider);

    provider = codexbar_antigravity_parse_usage("{}", "{}", "{} trailing", 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);

    const char *compact = "{\"clientModelConfigs\":[{\"label\":\"Custom Model\","
                          "\"modelOrAlias\":{\"model\":\"custom-model\"},"
                          "\"quotaInfo\":{\"remainingFraction\":0.25}}]}";
    provider = codexbar_antigravity_parse_usage(NULL, NULL, compact, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    g_assert_cmpstr(codexbar_provider_quota_window(provider, 0)->output_id,
                    ==,
                    "antigravity-compact-fallback-custom-model");
    codexbar_provider_free(provider);
}

static void test_antigravity_proc_ports(void) {
    const char *table =
        "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt uid timeout inode\n"
        "   0: 0100007F:1F90 00000000:0000 0A 0:0 00:0 0 1000 0 12345\n"
        "   1: 00000000:FFFF 00000000:0000 01 0:0 00:0 0 1000 0 12345\n"
        "   2: 0100007F:20FB 00000000:0000 0A 0:0 00:0 0 1000 0 99999\n";
    const char *inodes[] = {"12345"};
    GArray *ports = codexbar_antigravity_parse_listening_ports(table, inodes, 1);
    g_assert_cmpuint(ports->len, ==, 1);
    g_assert_cmpuint(g_array_index(ports, guint16, 0), ==, 8080);
    g_array_unref(ports);
}

typedef struct {
    guint count;
    GCancellable *cancellable;
    gboolean cancel;
} LocalTransportFixture;

static LocalTransportFixture transport_fixture;

static const char *request_header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static CodexBarHttpResponse *local_transport(const CodexBarHttpRequest *request, GError **error) {
    transport_fixture.count++;
    g_assert_true(g_str_has_prefix(request->url, "https://127.0.0.1:64440/"));
    g_assert_cmpstr(request->method, ==, "POST");
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_DENY);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_true(request->cancellable == transport_fixture.cancellable);
    g_assert_cmpstr(request_header(request, "X-Codeium-Csrf-Token"), ==, "csrf-token");
    if (transport_fixture.cancel) {
        g_cancellable_cancel(transport_fixture.cancellable);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_CANCELLED, "cancelled");
        return NULL;
    }
    const char *body = strstr(request->url, "RetrieveUserQuotaSummary") ? quota_summary() : user_status();
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = 200;
    response->body = g_strdup(body);
    response->body_length = strlen(body);
    return response;
}

static void test_antigravity_transport_and_cancellation(void) {
    memset(&transport_fixture, 0, sizeof(transport_fixture));
    guint16 port = 64440;
    GError *error = NULL;
    GCancellable *cancellable = g_cancellable_new();
    transport_fixture.cancellable = cancellable;
    CodexBarProvider *provider = codexbar_antigravity_fetch_ports_with_transport_and_cancellable(
        &port, 1, "csrf-token", local_transport, cancellable, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(transport_fixture.count, ==, 2);
    codexbar_provider_free(provider);
    g_object_unref(cancellable);

    memset(&transport_fixture, 0, sizeof(transport_fixture));
    cancellable = g_cancellable_new();
    transport_fixture.cancellable = cancellable;
    transport_fixture.cancel = TRUE;
    provider = codexbar_antigravity_fetch_ports_with_transport_and_cancellable(
        &port, 1, "csrf-token", local_transport, cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpuint(transport_fixture.count, ==, 1);
    g_clear_error(&error);
    g_object_unref(cancellable);

    provider = codexbar_antigravity_fetch_ports_with_transport_and_cancellable(
        &port, 1, "bad\nvalue", local_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
}

static void test_pre_cancelled_cli_fetches(void) {
    GCancellable *cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_kiro_fetch_with_binary_and_cancellable(
        "/does/not/run", cancellable, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    provider = codexbar_augment_fetch_with_binary_and_cancellable("/does/not/run", cancellable, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(cancellable);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/local-providers/kiro/parsing", test_kiro_parsing);
    g_test_add_func("/local-providers/kiro/variants-errors", test_kiro_variants_and_errors);
    g_test_add_func("/local-providers/augment/parsing", test_augment_parsing);
    g_test_add_func("/local-providers/antigravity/summary", test_antigravity_summary);
    g_test_add_func("/local-providers/antigravity/oneof-fallback", test_antigravity_oneof_and_fallback);
    g_test_add_func("/local-providers/antigravity/proc-ports", test_antigravity_proc_ports);
    g_test_add_func("/local-providers/antigravity/transport", test_antigravity_transport_and_cancellation);
    g_test_add_func("/local-providers/cli/pre-cancelled", test_pre_cancelled_cli_fetches);
    return g_test_run();
}
