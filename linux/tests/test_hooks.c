#include "hooks.h"

#include <glib.h>
#include <json-c/json.h>
#include <string.h>

typedef struct {
    guint calls;
    char **arguments;
    char **environment;
    char *standard_input;
    size_t standard_input_length;
    guint timeout_milliseconds;
    guint termination_grace_milliseconds;
    size_t maximum_output_bytes;
    gboolean new_session;
} HookProcessCapture;

static CodexBarProcessResult *capture_process(const CodexBarProcessRequest *request,
                                              gpointer user_data,
                                              GError **error) {
    (void)error;
    HookProcessCapture *capture = user_data;
    capture->calls++;
    capture->arguments = g_strdupv((char **)request->arguments);
    capture->environment = g_strdupv((char **)request->environment);
    capture->standard_input = g_strndup(request->standard_input, request->standard_input_length);
    capture->standard_input_length = request->standard_input_length;
    capture->timeout_milliseconds = request->timeout_milliseconds;
    capture->termination_grace_milliseconds = request->termination_grace_milliseconds;
    capture->maximum_output_bytes = request->maximum_output_bytes;
    capture->new_session = request->new_session;
    return g_new0(CodexBarProcessResult, 1);
}

static CodexBarProcessResult *capture_successful_process(const CodexBarProcessRequest *request,
                                                         gpointer user_data,
                                                         GError **error) {
    CodexBarProcessResult *result = capture_process(request, user_data, error);
    result->standard_output = g_strdup("  captured output \n");
    result->standard_output_length = strlen(result->standard_output);
    return result;
}

static void clear_capture(HookProcessCapture *capture) {
    g_strfreev(capture->arguments);
    g_strfreev(capture->environment);
    g_free(capture->standard_input);
}

static json_object *test_hooks(gboolean enabled, const char *provider) {
    json_object *hooks = json_object_new_object();
    json_object_object_add(hooks, "enabled", json_object_new_boolean(enabled));
    json_object *events = json_object_new_array();
    json_object *rule = json_object_new_object();
    json_object_object_add(rule, "id", json_object_new_string("runtime-rule"));
    json_object_object_add(rule, "event", json_object_new_string("quota_low"));
    if (provider) json_object_object_add(rule, "provider", json_object_new_string(provider));
    json_object_object_add(rule, "threshold", json_object_new_double(0.5));
    json_object_object_add(rule, "executable", json_object_new_string("/bin/example"));
    json_object *arguments = json_object_new_array();
    json_object_array_add(arguments, json_object_new_string("--flag"));
    json_object_object_add(rule, "arguments", arguments);
    json_object_object_add(rule, "timeoutSeconds", json_object_new_double(1.25));
    json_object_array_add(events, rule);
    json_object_object_add(hooks, "events", events);
    return hooks;
}

static CodexBarHookEvent quota_event(void) {
    return (CodexBarHookEvent){
        .event = "quota_low",
        .provider = "codex",
        .account = "user@example.com",
        .window = "session",
        .timestamp = "2026-08-01T00:00:00Z",
        .has_usage_percent = TRUE,
        .usage_percent = 0.75,
        .has_used = TRUE,
        .used = 75,
        .has_limit = TRUE,
        .limit = 100,
        .reset_at = "2026-08-02T00:00:00Z",
    };
}

static void test_dispatch_payload_environment_and_limits(void) {
    g_setenv("OPENAI_API_KEY", "private-marker", TRUE);
    json_object *hooks = test_hooks(TRUE, "codex");
    CodexBarHookEvent event = quota_event();
    HookProcessCapture capture = {0};
    json_object *results = codexbar_hooks_dispatch(hooks, &event, capture_successful_process, &capture);

    g_assert_cmpuint(capture.calls, ==, 1);
    g_assert_cmpuint(json_object_array_length(results), ==, 1);
    json_object *result = json_object_array_get_idx(results, 0);
    g_assert_cmpstr(json_object_get_string(json_object_object_get(result, "ruleID")), ==, "runtime-rule");
    g_assert_cmpstr(json_object_get_string(json_object_object_get(result, "executable")), ==, "/bin/example");
    g_assert_true(json_object_get_boolean(json_object_object_get(result, "success")));
    g_assert_cmpstr(json_object_get_string(json_object_object_get(result, "stdout")), ==, "captured output");

    json_object *payload = json_tokener_parse(capture.standard_input);
    g_assert_nonnull(payload);
    g_assert_cmpstr(json_object_get_string(json_object_object_get(payload, "event")), ==, "quota_low");
    g_assert_cmpstr(json_object_get_string(json_object_object_get(payload, "provider")), ==, "codex");
    g_assert_cmpstr(json_object_get_string(json_object_object_get(payload, "account")), ==, "user@example.com");
    g_assert_cmpfloat(json_object_get_double(json_object_object_get(payload, "usagePercent")), ==, 0.75);
    g_assert_cmpstr(json_object_get_string(json_object_object_get(payload, "window")), ==, "session");
    g_assert_cmpfloat(json_object_get_double(json_object_object_get(payload, "used")), ==, 75);
    g_assert_cmpfloat(json_object_get_double(json_object_object_get(payload, "limit")), ==, 100);
    g_assert_cmpstr(json_object_get_string(json_object_object_get(payload, "resetAt")),
                    ==,
                    "2026-08-02T00:00:00Z");
    g_assert_cmpstr(json_object_get_string(json_object_object_get(payload, "timestamp")),
                    ==,
                    "2026-08-01T00:00:00Z");

    g_assert_cmpstr(capture.arguments[0], ==, "/bin/example");
    g_assert_cmpstr(capture.arguments[1], ==, "--flag");
    g_assert_null(capture.arguments[2]);
    g_assert_cmpstr(g_environ_getenv(capture.environment, "CODEXBAR_EVENT"), ==, "quota_low");
    g_assert_cmpstr(g_environ_getenv(capture.environment, "CODEXBAR_PROVIDER"), ==, "codex");
    g_assert_cmpstr(g_environ_getenv(capture.environment, "CODEXBAR_ACCOUNT"), ==, "user@example.com");
    g_assert_cmpstr(g_environ_getenv(capture.environment, "CODEXBAR_WINDOW"), ==, "session");
    g_assert_cmpstr(g_environ_getenv(capture.environment, "CODEXBAR_USAGE_PERCENT"), ==, "0.75");
    g_assert_cmpstr(g_environ_getenv(capture.environment, "CODEXBAR_USED"), ==, "75");
    g_assert_cmpstr(g_environ_getenv(capture.environment, "CODEXBAR_LIMIT"), ==, "100");
    g_assert_cmpstr(g_environ_getenv(capture.environment, "CODEXBAR_RESET_AT"),
                    ==,
                    "2026-08-02T00:00:00Z");
    g_assert_cmpstr(g_environ_getenv(capture.environment, "CODEXBAR_TIMESTAMP"),
                    ==,
                    "2026-08-01T00:00:00Z");
    g_assert_null(g_environ_getenv(capture.environment, "OPENAI_API_KEY"));
    g_assert_cmpuint(capture.standard_input_length, ==, strlen(capture.standard_input));
    g_assert_cmpuint(capture.timeout_milliseconds, ==, 1250);
    g_assert_cmpuint(capture.termination_grace_milliseconds, ==, 400);
    g_assert_cmpuint(capture.maximum_output_bytes, ==, CODEXBAR_MAX_HOOK_OUTPUT_BYTES);
    g_assert_true(capture.new_session);

    json_object_put(payload);
    json_object_put(results);
    json_object_put(hooks);
    clear_capture(&capture);
    g_unsetenv("OPENAI_API_KEY");
}

static void test_dispatch_skips_disabled_and_nonmatching_rules(void) {
    CodexBarHookEvent event = quota_event();
    HookProcessCapture capture = {0};
    json_object *disabled = test_hooks(FALSE, "codex");
    json_object *results = codexbar_hooks_dispatch(disabled, &event, capture_process, &capture);
    g_assert_cmpuint(json_object_array_length(results), ==, 0);
    g_assert_cmpuint(capture.calls, ==, 0);
    json_object_put(results);
    json_object_put(disabled);

    json_object *nonmatching = test_hooks(TRUE, "claude");
    results = codexbar_hooks_dispatch(nonmatching, &event, capture_process, &capture);
    g_assert_cmpuint(json_object_array_length(results), ==, 0);
    g_assert_cmpuint(capture.calls, ==, 0);
    json_object_put(results);
    json_object_put(nonmatching);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/hooks/dispatch-payload-environment-limits", test_dispatch_payload_environment_and_limits);
    g_test_add_func("/hooks/dispatch-skips-disabled-nonmatching", test_dispatch_skips_disabled_and_nonmatching_rules);
    return g_test_run();
}
