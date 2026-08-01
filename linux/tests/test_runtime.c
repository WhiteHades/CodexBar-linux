#include "runtime.h"

#include <glib/gstdio.h>
#include <string.h>

static guint status_calls;
static gboolean status_fails;
static const char *status_indicator = "minor";
static guint hook_calls;
static char *last_hook_event;
static char *last_hook_account;
static char *last_hook_window;
static char *last_hook_status;
static double last_hook_usage;
static guint notification_calls;
static char *last_notification_event;
static int last_notification_threshold;
static gboolean usage_has_window;
static double usage_percent;
static gint64 usage_updated_at;
static const char *usage_account;
static const char *usage_error;
static const char *usage_error_kind;

static CodexBarSnapshot *usage_fetcher(GCancellable *cancellable, gpointer user_data, GError **error) {
    (void)cancellable;
    (void)user_data;
    (void)error;
    CodexBarSnapshot *snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("codex");
    provider->source = g_strdup("cli");
    provider->account = g_strdup(usage_account);
    provider->error = g_strdup(usage_error);
    provider->error_kind = g_strdup(usage_error_kind);
    if (usage_updated_at > 0) {
        provider->has_updated_at = TRUE;
        provider->updated_at_ms = usage_updated_at;
    }
    if (usage_has_window) {
        CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "5-hour");
        window->usage_known = TRUE;
        window->used_percent = usage_percent;
        window->has_window_minutes = TRUE;
        window->window_minutes = 300;
        codexbar_provider_add_quota_window(provider, window);
    }
    g_ptr_array_add(snapshot->providers, provider);
    return snapshot;
}

static CodexBarHttpResponse *status_transport(const CodexBarHttpRequest *request, GError **error) {
    status_calls++;
    if (status_fails) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE, "status offline");
        return NULL;
    }
    const char *body = "{}";
    char *status_body = NULL;
    if (!strstr(request->url, "/proxy/") && !strstr(request->url, "/components.json")) {
        status_body = g_strdup_printf(
            "{\"status\":{\"indicator\":\"%s\",\"description\":\"degraded\"}}", status_indicator);
        body = status_body;
    }
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = 200;
    response->body = g_strdup(body);
    response->body_length = strlen(body);
    response->headers = g_ptr_array_new();
    response->effective_url = g_strdup(request->url);
    g_free(status_body);
    return response;
}

static void capture_hook(json_object *hooks, const CodexBarHookEvent *event, gpointer user_data) {
    (void)hooks;
    (void)user_data;
    hook_calls++;
    g_free(last_hook_event);
    g_free(last_hook_account);
    g_free(last_hook_window);
    g_free(last_hook_status);
    last_hook_event = g_strdup(event->event);
    last_hook_account = g_strdup(event->account);
    last_hook_window = g_strdup(event->window);
    last_hook_status = g_strdup(event->status);
    last_hook_usage = event->usage_percent;
}

static void capture_notification(const CodexBarHookEvent *event, gpointer user_data) {
    (void)user_data;
    notification_calls++;
    g_free(last_notification_event);
    last_notification_event = g_strdup(event->event);
    last_notification_threshold = event->has_warning_threshold ? event->warning_threshold : 0;
}

static void reset_usage(void) {
    usage_has_window = FALSE;
    usage_percent = 0;
    usage_updated_at = 0;
    usage_account = NULL;
    usage_error = NULL;
    usage_error_kind = NULL;
}

static void reset_hooks(void) {
    hook_calls = 0;
    g_clear_pointer(&last_hook_event, g_free);
    g_clear_pointer(&last_hook_account, g_free);
    g_clear_pointer(&last_hook_window, g_free);
    g_clear_pointer(&last_hook_status, g_free);
    last_hook_usage = 0;
}

static void reset_notifications(void) {
    notification_calls = 0;
    g_clear_pointer(&last_notification_event, g_free);
    last_notification_threshold = 0;
}

static char *write_config(void) {
    g_assert_cmpint(g_mkdir_with_parents(".tmp", 0700), ==, 0);
    char *directory = g_strdup(".tmp/codexbar-runtime-XXXXXX");
    g_assert_nonnull(g_mkdtemp(directory));
    char *path = g_build_filename(directory, "config.json", NULL);
    g_assert_true(g_file_set_contents(path,
                                      "{\"version\":1,\"providers\":[{\"id\":\"codex\",\"enabled\":true}]}",
                                      -1,
                                      NULL));
    g_free(directory);
    return path;
}

static void replace_config(const char *path, const char *hooks) {
    char *contents = g_strdup_printf(
        "{\"version\":1,\"providers\":[{\"id\":\"codex\",\"enabled\":true}],\"hooks\":%s}", hooks);
    g_assert_true(g_file_set_contents(path, contents, -1, NULL));
    g_free(contents);
}

static void remove_config(char *path) {
    char *directory = g_path_get_dirname(path);
    g_assert_cmpint(g_remove(path), ==, 0);
    g_assert_cmpint(g_rmdir(directory), ==, 0);
    g_free(directory);
    g_free(path);
}

static void test_status_publication_and_last_good(void) {
    reset_usage();
    char *path = write_config();
    g_setenv("CODEXBAR_CONFIG", path, TRUE);
    status_calls = 0;
    status_fails = FALSE;
    status_indicator = "minor";
    CodexBarRuntime *runtime = codexbar_runtime_new_with_transports(usage_fetcher, NULL, status_transport);
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_runtime_fetch(runtime, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(snapshot);
    CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, 0);
    g_assert_nonnull(provider->status);
    g_assert_cmpint(provider->status->indicator, ==, CODEXBAR_STATUS_MINOR);
    g_assert_cmpstr(provider->status->description, ==, "degraded");
    g_assert_cmpstr(provider->status->url, ==, "https://status.openai.com/");
    g_assert_cmpuint(status_calls, ==, 3);
    codexbar_snapshot_free(snapshot);

    status_fails = TRUE;
    snapshot = codexbar_runtime_fetch(runtime, NULL, &error);
    g_assert_no_error(error);
    provider = g_ptr_array_index(snapshot->providers, 0);
    g_assert_cmpint(provider->status->indicator, ==, CODEXBAR_STATUS_MINOR);
    g_assert_cmpstr(provider->status->description, ==, "degraded");
    codexbar_snapshot_free(snapshot);

    g_assert_true(g_file_set_contents(
        path,
        "{\"version\":1,\"refreshFrequency\":\"5m\",\"providers\":[{\"id\":\"codex\",\"enabled\":true}]}",
        -1,
        NULL));
    snapshot = codexbar_runtime_fetch(runtime, NULL, &error);
    g_assert_no_error(error);
    provider = g_ptr_array_index(snapshot->providers, 0);
    g_assert_cmpint(provider->status->indicator, ==, CODEXBAR_STATUS_UNKNOWN);
    g_assert_cmpstr(provider->status->description, ==, "status offline");
    codexbar_snapshot_free(snapshot);
    codexbar_runtime_free(runtime);
    g_unsetenv("CODEXBAR_CONFIG");
    remove_config(path);
}

static void test_first_status_failure_is_unknown(void) {
    reset_usage();
    char *path = write_config();
    g_setenv("CODEXBAR_CONFIG", path, TRUE);
    status_fails = TRUE;
    CodexBarRuntime *runtime = codexbar_runtime_new_with_transports(usage_fetcher, NULL, status_transport);
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_runtime_fetch(runtime, NULL, &error);
    g_assert_no_error(error);
    CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, 0);
    g_assert_cmpint(provider->status->indicator, ==, CODEXBAR_STATUS_UNKNOWN);
    g_assert_cmpstr(provider->status->description, ==, "status offline");
    codexbar_snapshot_free(snapshot);
    codexbar_runtime_free(runtime);
    g_unsetenv("CODEXBAR_CONFIG");
    remove_config(path);
}

static void test_status_hooks_follow_outage_transitions(void) {
    reset_usage();
    char *path = write_config();
    g_assert_true(g_file_set_contents(
        path,
        "{\"version\":1,\"providers\":[{\"id\":\"codex\",\"enabled\":true}],"
        "\"hooks\":{\"enabled\":true,\"events\":["
        "{\"event\":\"provider_unavailable\",\"executable\":\"/bin/true\"},"
        "{\"event\":\"provider_recovered\",\"executable\":\"/bin/true\"}]}}",
        -1,
        NULL));
    g_setenv("CODEXBAR_CONFIG", path, TRUE);
    status_fails = FALSE;
    status_indicator = "minor";
    reset_hooks();
    CodexBarRuntime *runtime = codexbar_runtime_new_with_transports(usage_fetcher, NULL, status_transport);
    codexbar_runtime_set_hook_dispatcher(runtime, capture_hook, NULL);

    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_runtime_fetch(runtime, NULL, &error);
    g_assert_no_error(error);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(hook_calls, ==, 1);
    g_assert_cmpstr(last_hook_event, ==, "provider_unavailable");

    snapshot = codexbar_runtime_fetch(runtime, NULL, &error);
    g_assert_no_error(error);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(hook_calls, ==, 1);

    status_indicator = "none";
    snapshot = codexbar_runtime_fetch(runtime, NULL, &error);
    g_assert_no_error(error);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(hook_calls, ==, 2);
    g_assert_cmpstr(last_hook_event, ==, "provider_recovered");

    codexbar_runtime_free(runtime);
    reset_hooks();
    g_unsetenv("CODEXBAR_CONFIG");
    remove_config(path);
}

static void test_quota_low_is_crossing_and_lane_scoped(void) {
    char *path = write_config();
    replace_config(path,
                   "{\"enabled\":true,\"events\":[{\"event\":\"quota_low\","
                   "\"threshold\":0.8,\"executable\":\"/bin/true\"}]}");
    g_setenv("CODEXBAR_CONFIG", path, TRUE);
    g_setenv("CODEXBAR_DISABLE_STATUS", "1", TRUE);
    reset_usage();
    reset_hooks();
    usage_has_window = TRUE;
    usage_account = "one";
    usage_percent = 75;
    usage_updated_at = 1000;
    status_fails = FALSE;
    status_indicator = "none";
    status_calls = 0;
    CodexBarRuntime *runtime = codexbar_runtime_new_with_transports(usage_fetcher, NULL, status_transport);
    codexbar_runtime_set_hook_dispatcher(runtime, capture_hook, NULL);

    CodexBarSnapshot *snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    g_assert_nonnull(snapshot);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(hook_calls, ==, 0);

    usage_percent = 85;
    usage_updated_at = 2000;
    snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    g_assert_nonnull(snapshot);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(hook_calls, ==, 1);
    g_assert_cmpstr(last_hook_event, ==, "quota_low");
    g_assert_cmpstr(last_hook_account, ==, "one");
    g_assert_cmpstr(last_hook_window, ==, "5-hour");
    g_assert_cmpfloat(last_hook_usage, ==, 0.85);
    g_assert_cmpuint(status_calls, ==, 0);

    usage_account = "two";
    usage_percent = 90;
    usage_updated_at = 3000;
    snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    g_assert_nonnull(snapshot);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(hook_calls, ==, 1);

    codexbar_runtime_free(runtime);
    reset_hooks();
    g_unsetenv("CODEXBAR_CONFIG");
    g_unsetenv("CODEXBAR_DISABLE_STATUS");
    remove_config(path);
}

static void test_session_hooks_confirm_codex_restore(void) {
    char *path = write_config();
    replace_config(path,
                   "{\"enabled\":true,\"events\":["
                   "{\"event\":\"quota_reached\",\"executable\":\"/bin/true\"},"
                   "{\"event\":\"quota_reset\",\"executable\":\"/bin/true\"}]}");
    g_setenv("CODEXBAR_CONFIG", path, TRUE);
    reset_usage();
    reset_hooks();
    usage_has_window = TRUE;
    usage_account = "one";
    usage_percent = 100;
    usage_updated_at = 1000;
    status_fails = FALSE;
    status_indicator = "none";
    CodexBarRuntime *runtime = codexbar_runtime_new_with_transports(usage_fetcher, NULL, status_transport);
    codexbar_runtime_set_hook_dispatcher(runtime, capture_hook, NULL);

    CodexBarSnapshot *snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    g_assert_nonnull(snapshot);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(hook_calls, ==, 1);
    g_assert_cmpstr(last_hook_event, ==, "quota_reached");

    usage_percent = 50;
    usage_updated_at = 2000;
    snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    g_assert_nonnull(snapshot);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(hook_calls, ==, 1);

    usage_updated_at = 3000;
    snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    g_assert_nonnull(snapshot);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(hook_calls, ==, 2);
    g_assert_cmpstr(last_hook_event, ==, "quota_reset");

    codexbar_runtime_free(runtime);
    reset_hooks();
    g_unsetenv("CODEXBAR_CONFIG");
    remove_config(path);
}

static void test_refresh_failure_suppression_and_rate_limit(void) {
    char *path = write_config();
    replace_config(path,
                   "{\"enabled\":true,\"events\":[{\"event\":\"refresh_failed\","
                   "\"executable\":\"/bin/true\"}]}");
    g_setenv("CODEXBAR_CONFIG", path, TRUE);
    reset_usage();
    reset_hooks();
    usage_account = "one";
    status_fails = FALSE;
    status_indicator = "none";
    CodexBarRuntime *runtime = codexbar_runtime_new_with_transports(usage_fetcher, NULL, status_transport);
    codexbar_runtime_set_hook_dispatcher(runtime, capture_hook, NULL);

    CodexBarSnapshot *snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    g_assert_nonnull(snapshot);
    codexbar_snapshot_free(snapshot);
    usage_error = "request timed out";
    usage_error_kind = "timeout";
    snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    g_assert_nonnull(snapshot);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(hook_calls, ==, 0);

    snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    g_assert_nonnull(snapshot);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(hook_calls, ==, 1);
    g_assert_cmpstr(last_hook_event, ==, "refresh_failed");
    g_assert_cmpstr(last_hook_status, ==, "timeout");

    snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    g_assert_nonnull(snapshot);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(hook_calls, ==, 1);
    codexbar_runtime_free(runtime);

    reset_hooks();
    runtime = codexbar_runtime_new_with_transports(usage_fetcher, NULL, status_transport);
    codexbar_runtime_set_hook_dispatcher(runtime, capture_hook, NULL);
    snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    g_assert_nonnull(snapshot);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(hook_calls, ==, 1);
    g_assert_cmpstr(last_hook_event, ==, "refresh_failed");
    codexbar_runtime_free(runtime);

    reset_usage();
    reset_hooks();
    g_unsetenv("CODEXBAR_CONFIG");
    remove_config(path);
}

static void test_quota_warning_notifications_follow_provider_config(void) {
    char *path = write_config();
    g_assert_true(g_file_set_contents(
        path,
        "{\"version\":1,\"quotaWarningNotificationsEnabled\":true,\"providers\":[{\"id\":\"codex\",\"enabled\":true,"
        "\"quotaWarnings\":{\"session\":{\"enabled\":true,\"thresholds\":[50,20]}}}]}",
        -1,
        NULL));
    g_setenv("CODEXBAR_CONFIG", path, TRUE);
    reset_usage();
    reset_hooks();
    reset_notifications();
    usage_has_window = TRUE;
    usage_account = "one";
    usage_percent = 40;
    usage_updated_at = 1000;
    status_fails = FALSE;
    status_indicator = "none";
    CodexBarRuntime *runtime = codexbar_runtime_new_with_transports(usage_fetcher, NULL, status_transport);
    codexbar_runtime_set_hook_dispatcher(runtime, capture_hook, NULL);
    codexbar_runtime_set_notification_dispatcher(runtime, capture_notification, NULL);

    CodexBarSnapshot *snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(notification_calls, ==, 0);
    usage_percent = 55;
    usage_updated_at = 2000;
    snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(notification_calls, ==, 1);
    g_assert_cmpstr(last_notification_event, ==, "quota_low");
    g_assert_cmpint(last_notification_threshold, ==, 50);

    usage_percent = 100;
    usage_updated_at = 3000;
    snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(notification_calls, ==, 3);
    g_assert_cmpstr(last_notification_event, ==, "quota_reached");

    usage_percent = 50;
    usage_updated_at = 4000;
    snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(notification_calls, ==, 3);
    usage_updated_at = 5000;
    snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(notification_calls, ==, 4);
    g_assert_cmpstr(last_notification_event, ==, "quota_reset");
    g_assert_cmpuint(hook_calls, ==, 0);

    codexbar_runtime_free(runtime);
    reset_notifications();
    usage_percent = 90;
    usage_updated_at = 6000;
    runtime = codexbar_runtime_new_with_transports(usage_fetcher, NULL, status_transport);
    codexbar_runtime_set_notification_dispatcher(runtime, capture_notification, NULL);
    snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(notification_calls, ==, 1);
    g_assert_cmpstr(last_notification_event, ==, "quota_low");
    g_assert_cmpint(last_notification_threshold, ==, 20);
    usage_percent = 30;
    usage_updated_at = 7000;
    snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    codexbar_snapshot_free(snapshot);
    usage_percent = 60;
    usage_updated_at = 8000;
    snapshot = codexbar_runtime_fetch(runtime, NULL, NULL);
    codexbar_snapshot_free(snapshot);
    g_assert_cmpuint(notification_calls, ==, 2);
    g_assert_cmpint(last_notification_threshold, ==, 50);
    codexbar_runtime_free(runtime);
    reset_notifications();
    reset_hooks();
    reset_usage();
    g_unsetenv("CODEXBAR_CONFIG");
    remove_config(path);
}

int main(int argc, char **argv) {
    g_setenv("CODEXBAR_DISABLE_HISTORY", "1", TRUE);
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/runtime/status/last-good", test_status_publication_and_last_good);
    g_test_add_func("/runtime/status/first-failure", test_first_status_failure_is_unknown);
    g_test_add_func("/runtime/status/hooks", test_status_hooks_follow_outage_transitions);
    g_test_add_func("/runtime/hooks/quota-low", test_quota_low_is_crossing_and_lane_scoped);
    g_test_add_func("/runtime/hooks/session", test_session_hooks_confirm_codex_restore);
    g_test_add_func("/runtime/hooks/refresh-failed", test_refresh_failure_suppression_and_rate_limit);
    g_test_add_func("/runtime/notifications/quota", test_quota_warning_notifications_follow_provider_config);
    return g_test_run();
}
