#pragma once

#include <glib.h>

typedef enum {
    CODEXBAR_TUI_ACTION_DASHBOARD,
    CODEXBAR_TUI_ACTION_STATUS,
    CODEXBAR_TUI_ACTION_SETTINGS,
    CODEXBAR_TUI_ACTION_ABOUT,
    CODEXBAR_TUI_ACTION_QUIT,
} CodexBarTuiActionKind;

typedef enum {
    CODEXBAR_TUI_EFFECT_OPENED,
    CODEXBAR_TUI_EFFECT_SHOW_ABOUT,
    CODEXBAR_TUI_EFFECT_QUIT,
} CodexBarTuiEffect;

typedef struct {
    CodexBarTuiActionKind kind;
    char *label;
    char *target;
} CodexBarTuiAction;

typedef gboolean (*CodexBarTuiUriLauncher)(const char *uri, GError **error, gpointer user_data);

GPtrArray *codexbar_tui_actions_new(const char *provider_id,
                                    const char *config_path,
                                    const char *dashboard_override);
void codexbar_tui_action_free(CodexBarTuiAction *action);
gboolean codexbar_tui_uri_is_allowed(const char *uri, gboolean allow_file);
gboolean codexbar_tui_action_execute(const CodexBarTuiAction *action,
                                     CodexBarTuiUriLauncher launcher,
                                     gpointer user_data,
                                     CodexBarTuiEffect *effect,
                                     GError **error);
gboolean codexbar_tui_action_execute_default(const CodexBarTuiAction *action,
                                             CodexBarTuiEffect *effect,
                                             GError **error);
