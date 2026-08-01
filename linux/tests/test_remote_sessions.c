#include "sessions.h"

#include <glib.h>
#include <string.h>

static CodexBarProcessResult *process_result(const char *output, int status) {
    CodexBarProcessResult *result = g_new0(CodexBarProcessResult, 1);
    result->standard_output = g_strdup(output);
    result->standard_output_length = strlen(output);
    result->standard_error = g_strdup("");
    result->exit_status = status;
    return result;
}

static void test_tailscale_dictionary_and_array_shapes(void) {
    const char *dictionary =
        "{\"BackendState\":\"Running\",\"Self\":{\"DNSName\":\"local.tail.ts.net.\"},\"Peer\":{"
        "\"one\":{\"DNSName\":\"LinuxBox.tail.ts.net.\",\"OS\":\"linux\",\"Online\":true},"
        "\"two\":{\"DNSName\":\"clawmac.tail.ts.net.\",\"OS\":\"macOS\",\"Online\":true},"
        "\"dup\":{\"DNSName\":\"linuxbox.other.ts.net.\",\"OS\":\"linux\",\"Online\":true},"
        "\"self\":{\"DNSName\":\"local.other.ts.net.\",\"OS\":\"linux\",\"Online\":true},"
        "\"phone\":{\"DNSName\":\"phone.tail.ts.net.\",\"OS\":\"iOS\",\"Online\":true},"
        "\"off\":{\"DNSName\":\"offline.tail.ts.net.\",\"OS\":\"linux\",\"Online\":false}}}";
    GPtrArray *hosts = codexbar_tailscale_status_parse_hosts(dictionary, strlen(dictionary), "LOCAL");
    g_assert_nonnull(hosts);
    g_assert_cmpuint(hosts->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(hosts, 0), ==, "LinuxBox");
    g_assert_cmpstr(g_ptr_array_index(hosts, 1), ==, "clawmac");
    g_ptr_array_unref(hosts);

    const char *array =
        "{\"Self\":{},\"Peer\":["
        "{\"DNSName\":\"beta.tail.ts.net\",\"OS\":\"linux\",\"Online\":true},"
        "{\"DNSName\":\"alpha.tail.ts.net\",\"OS\":\"macOS\",\"Online\":true}]}";
    hosts = codexbar_tailscale_status_parse_hosts(array, strlen(array), NULL);
    g_assert_nonnull(hosts);
    g_assert_cmpstr(g_ptr_array_index(hosts, 0), ==, "alpha");
    g_assert_cmpstr(g_ptr_array_index(hosts, 1), ==, "beta");
    g_ptr_array_unref(hosts);

    g_assert_null(codexbar_tailscale_status_parse_hosts("{\"Version\":\"1\"}", 15, NULL));
    const char *inactive = "{\"BackendState\":\"NeedsLogin\",\"Peer\":null}";
    g_assert_null(codexbar_tailscale_status_parse_hosts(inactive, strlen(inactive), NULL));
    const char *empty = "{\"BackendState\":\"Running\",\"Peer\":null}";
    hosts = codexbar_tailscale_status_parse_hosts(empty, strlen(empty), NULL);
    g_assert_nonnull(hosts);
    g_assert_cmpuint(hosts->len, ==, 0);
    g_ptr_array_unref(hosts);
}

static void test_host_sanitation_and_csv(void) {
    const char *raw[] = {
        " user@clawmac ",
        "USER@CLAWMAC",
        "-oProxyCommand=bad",
        "host with-space",
        "host\nother",
        "linuxbox",
        "\xC2\xA0unicode\xC2\xA0"};
    GPtrArray *hosts = codexbar_session_hosts_sanitize(raw, G_N_ELEMENTS(raw));
    g_assert_cmpuint(hosts->len, ==, 3);
    g_assert_cmpstr(g_ptr_array_index(hosts, 0), ==, "user@clawmac");
    g_assert_cmpstr(g_ptr_array_index(hosts, 1), ==, "linuxbox");
    g_assert_cmpstr(g_ptr_array_index(hosts, 2), ==, "unicode");
    g_ptr_array_unref(hosts);

    hosts = codexbar_session_hosts_from_csv(" linuxbox, user@clawmac, LINUXBOX, , -bad ");
    g_assert_cmpuint(hosts->len, ==, 2);
    g_assert_cmpstr(g_ptr_array_index(hosts, 0), ==, "linuxbox");
    g_assert_cmpstr(g_ptr_array_index(hosts, 1), ==, "user@clawmac");
    g_ptr_array_unref(hosts);
}

static gint active_runners;
static gint maximum_active_runners;

static void update_maximum(gint value) {
    gint previous = g_atomic_int_get(&maximum_active_runners);
    while (value > previous && !g_atomic_int_compare_and_exchange(&maximum_active_runners, previous, value)) {
        previous = g_atomic_int_get(&maximum_active_runners);
    }
}

static CodexBarProcessResult *fetch_runner(const CodexBarProcessRequest *request,
                                           GCancellable *cancellable,
                                           GError **error) {
    (void)error;
    g_assert_null(cancellable);
    g_assert_cmpstr(request->arguments[1], ==, "-o");
    g_assert_cmpstr(request->arguments[2], ==, "BatchMode=yes");
    g_assert_cmpstr(request->arguments[3], ==, "-o");
    g_assert_cmpstr(request->arguments[4], ==, "ConnectTimeout=3");
    g_assert_cmpstr(request->arguments[6], ==, "sh");
    g_assert_cmpstr(request->arguments[7], ==, "-lc");
    g_assert_nonnull(strstr(request->arguments[8], "CODEXBAR_REMOTE_SESSIONS_LOCAL_ONLY=1"));
    g_assert_cmpuint(request->timeout_milliseconds, ==, 5000);
    g_assert_cmpuint(request->termination_grace_milliseconds, ==, 400);
    g_assert_cmpuint(request->maximum_output_bytes, ==, 1024U * 1024U);
    g_assert_true(request->new_session);
    gint active = g_atomic_int_add(&active_runners, 1) + 1;
    update_maximum(active);
    g_usleep(30000);
    g_atomic_int_add(&active_runners, -1);
    return process_result(
        "[{\"id\":\"remote-id\",\"provider\":\"codex\",\"source\":\"ide\","
        "\"state\":\"active\",\"pid\":42,\"cwd\":\"/work/project\","
        "\"projectName\":\"project\",\"sessionName\":\"Fix remote sessions\","
        "\"startedAt\":\"2026-07-01T10:00:00Z\",\"lastActivityAt\":null,"
        "\"transcriptPath\":\"/work/rollout.jsonl\",\"host\":\"ignored\"}]",
        0);
}

static void test_concurrent_remote_fetch_and_schema(void) {
    const char *hosts[] = {"zeta", "Alpha", "alpha", "-bad"};
    active_runners = 0;
    maximum_active_runners = 0;
    GPtrArray *results = codexbar_remote_sessions_fetch(hosts, G_N_ELEMENTS(hosts), fetch_runner, NULL);
    g_assert_cmpuint(results->len, ==, 2);
    g_assert_cmpint(g_atomic_int_get(&maximum_active_runners), >=, 2);
    CodexBarRemoteSessionHostResult *alpha = g_ptr_array_index(results, 0);
    CodexBarRemoteSessionHostResult *zeta = g_ptr_array_index(results, 1);
    g_assert_cmpstr(alpha->host, ==, "Alpha");
    g_assert_cmpstr(zeta->host, ==, "zeta");
    g_assert_null(alpha->error);
    g_assert_cmpuint(alpha->sessions->len, ==, 1);
    CodexBarAgentSession *session = g_ptr_array_index(alpha->sessions, 0);
    g_assert_cmpstr(session->host, ==, "Alpha");
    g_assert_cmpstr(session->session_name, ==, "Fix remote sessions");
    g_assert_true(session->has_started_at);
    g_ptr_array_unref(results);
}

static char *focus_command;

static char *decode_shell_quote(const char *quoted) {
    size_t length = strlen(quoted);
    g_assert_cmpuint(length, >=, 2);
    g_assert_cmpint(quoted[0], ==, '\'');
    g_assert_cmpint(quoted[length - 1], ==, '\'');
    GString *decoded = g_string_new(NULL);
    for (size_t index = 1; index < length - 1;) {
        if (quoted[index] != '\'') {
            g_string_append_c(decoded, quoted[index++]);
            continue;
        }
        g_assert_cmpuint(index + 3, <, length);
        g_assert_cmpmem(quoted + index, 4, "'\\''", 4);
        g_string_append_c(decoded, '\'');
        index += 4;
    }
    return g_string_free(decoded, FALSE);
}

static CodexBarProcessResult *focus_runner(const CodexBarProcessRequest *request,
                                           GCancellable *cancellable,
                                           GError **error) {
    (void)cancellable;
    (void)error;
    g_assert_cmpstr(request->arguments[5], ==, "user@host");
    focus_command = g_strdup(request->arguments[8]);
    return process_result("", 2);
}

static void test_remote_focus_is_quoted_and_accepts_remote_failure(void) {
    GError *error = NULL;
    g_assert_true(codexbar_remote_session_focus(
        "id'; touch unsafe; '", " user@host ", focus_runner, NULL, &error));
    g_assert_no_error(error);
    char *remote_command = decode_shell_quote(focus_command);
    g_assert_nonnull(strstr(remote_command, "'id'\\''; touch unsafe; '\\'''"));
    g_free(remote_command);
    g_free(focus_command);
    focus_command = NULL;

    g_assert_false(codexbar_remote_session_focus("id", "-oProxyCommand=bad", focus_runner, NULL, &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/remote-sessions/tailscale/shapes", test_tailscale_dictionary_and_array_shapes);
    g_test_add_func("/remote-sessions/hosts/sanitize", test_host_sanitation_and_csv);
    g_test_add_func("/remote-sessions/ssh/fetch", test_concurrent_remote_fetch_and_schema);
    g_test_add_func("/remote-sessions/ssh/focus", test_remote_focus_is_quoted_and_accepts_remote_failure);
    return g_test_run();
}
