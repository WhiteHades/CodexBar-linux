#include "notifications.h"

static char *captured_title;
static char *captured_body;

static void capture(const char *title, const char *body, gpointer user_data) {
    (void)user_data;
    g_free(captured_title);
    g_free(captured_body);
    captured_title = g_strdup(title);
    captured_body = g_strdup(body);
}

static void test_quota_warning_text(void) {
    CodexBarHookEvent event = {
        .event = "quota_low",
        .provider = "codex",
        .account = "Work",
        .window = "5-hour",
        .has_usage_percent = TRUE,
        .usage_percent = 0.8,
        .has_warning_threshold = TRUE,
        .warning_threshold = 20,
    };
    codexbar_notification_send_with_sender(&event, capture, NULL);
    g_assert_cmpstr(captured_title, ==, "codex 5-hour quota low");
    g_assert_cmpstr(captured_body, ==, "Account Work. 20% left. Reached your 20% 5-hour warning threshold.");
}

static void test_reset_text(void) {
    CodexBarHookEvent event = {
        .event = "quota_reset",
        .provider = "claude",
        .window = "session",
    };
    codexbar_notification_send_with_sender(&event, capture, NULL);
    g_assert_cmpstr(captured_title, ==, "claude quota reset");
    g_assert_cmpstr(captured_body, ==, "current account can use session again.");
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/notifications/quota", test_quota_warning_text);
    g_test_add_func("/notifications/reset", test_reset_text);
    int result = g_test_run();
    g_free(captured_title);
    g_free(captured_body);
    return result;
}
