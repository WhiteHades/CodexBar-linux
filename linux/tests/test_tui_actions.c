#include "tui_actions.h"

#include <gio/gio.h>
#include <glib.h>

typedef struct {
    guint calls;
    char *uri;
    gboolean succeeds;
} RecordingLauncher;

static gboolean record_launch(const char *uri, GError **error, gpointer user_data) {
    RecordingLauncher *launcher = user_data;
    launcher->calls++;
    g_free(launcher->uri);
    launcher->uri = g_strdup(uri);
    if (!launcher->succeeds) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "recorded launch failure");
        return FALSE;
    }
    return TRUE;
}

static CodexBarTuiAction *action_of_kind(GPtrArray *actions, CodexBarTuiActionKind kind) {
    for (guint index = 0; index < actions->len; index++) {
        CodexBarTuiAction *action = g_ptr_array_index(actions, index);
        if (action->kind == kind) return action;
    }
    return NULL;
}

static void test_action_availability(void) {
    GPtrArray *actions = codexbar_tui_actions_new("codex", "/home/test/config.json");
    g_assert_cmpuint(actions->len, ==, 5);
    g_assert_cmpstr(action_of_kind(actions, CODEXBAR_TUI_ACTION_DASHBOARD)->target,
                    ==,
                    "https://chatgpt.com/codex/settings/usage");
    g_assert_cmpstr(action_of_kind(actions, CODEXBAR_TUI_ACTION_STATUS)->target,
                    ==,
                    "https://status.openai.com/");
    g_assert_true(g_str_has_prefix(action_of_kind(actions, CODEXBAR_TUI_ACTION_SETTINGS)->target, "file:///"));
    g_ptr_array_unref(actions);

    actions = codexbar_tui_actions_new("moonshot", "/home/test/config.json");
    g_assert_cmpuint(actions->len, ==, 4);
    g_assert_nonnull(action_of_kind(actions, CODEXBAR_TUI_ACTION_DASHBOARD));
    g_assert_null(action_of_kind(actions, CODEXBAR_TUI_ACTION_STATUS));
    g_ptr_array_unref(actions);

    actions = codexbar_tui_actions_new("unknown", "/home/test/config.json");
    g_assert_cmpuint(actions->len, ==, 3);
    g_assert_null(action_of_kind(actions, CODEXBAR_TUI_ACTION_DASHBOARD));
    g_assert_null(action_of_kind(actions, CODEXBAR_TUI_ACTION_STATUS));
    g_ptr_array_unref(actions);
}

static void test_uri_policy(void) {
    g_assert_true(codexbar_tui_uri_is_allowed("https://status.openai.com/", FALSE));
    g_assert_true(codexbar_tui_uri_is_allowed("http://127.0.0.1:8088/router", FALSE));
    g_assert_true(codexbar_tui_uri_is_allowed("file:///home/test/config.json", TRUE));
    g_assert_false(codexbar_tui_uri_is_allowed("file:///home/test/config.json", FALSE));
    g_assert_false(codexbar_tui_uri_is_allowed("file://remote-host/home/test/config.json", TRUE));
    g_assert_false(codexbar_tui_uri_is_allowed("file:///home/test/config.json?remote=true", TRUE));
    g_assert_false(codexbar_tui_uri_is_allowed("http://example.com", FALSE));
    g_assert_false(codexbar_tui_uri_is_allowed("javascript:alert(1)", FALSE));
    g_assert_false(codexbar_tui_uri_is_allowed("https://user:secret@example.com/", FALSE));
    g_assert_false(codexbar_tui_uri_is_allowed("https:///missing-host", FALSE));
}

static void test_action_execution(void) {
    GPtrArray *actions = codexbar_tui_actions_new("openrouter", "/home/test/config.json");
    RecordingLauncher launcher = {.succeeds = TRUE};
    CodexBarTuiEffect effect = CODEXBAR_TUI_EFFECT_QUIT;
    GError *error = NULL;
    g_assert_true(codexbar_tui_action_execute(action_of_kind(actions, CODEXBAR_TUI_ACTION_DASHBOARD),
                                              record_launch,
                                              &launcher,
                                              &effect,
                                              &error));
    g_assert_no_error(error);
    g_assert_cmpint(effect, ==, CODEXBAR_TUI_EFFECT_OPENED);
    g_assert_cmpuint(launcher.calls, ==, 1);
    g_assert_cmpstr(launcher.uri, ==, "https://openrouter.ai/settings/credits");

    g_assert_true(codexbar_tui_action_execute(action_of_kind(actions, CODEXBAR_TUI_ACTION_ABOUT),
                                              record_launch,
                                              &launcher,
                                              &effect,
                                              &error));
    g_assert_cmpint(effect, ==, CODEXBAR_TUI_EFFECT_SHOW_ABOUT);
    g_assert_cmpuint(launcher.calls, ==, 1);
    g_assert_true(codexbar_tui_action_execute(action_of_kind(actions, CODEXBAR_TUI_ACTION_QUIT),
                                              record_launch,
                                              &launcher,
                                              &effect,
                                              &error));
    g_assert_cmpint(effect, ==, CODEXBAR_TUI_EFFECT_QUIT);
    g_assert_cmpuint(launcher.calls, ==, 1);

    launcher.succeeds = FALSE;
    g_assert_false(codexbar_tui_action_execute(action_of_kind(actions, CODEXBAR_TUI_ACTION_STATUS),
                                               record_launch,
                                               &launcher,
                                               &effect,
                                               &error));
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&error);
    g_free(launcher.uri);
    g_ptr_array_unref(actions);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/tui-actions/availability", test_action_availability);
    g_test_add_func("/tui-actions/uri-policy", test_uri_policy);
    g_test_add_func("/tui-actions/execution", test_action_execution);
    return g_test_run();
}
