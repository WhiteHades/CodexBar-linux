#include "tui_actions.h"

#include "provider_registry.h"

#include <gio/gio.h>

static GQuark action_error_quark(void) {
    return g_quark_from_static_string("codexbar-tui-action-error");
}

static void add_action(GPtrArray *actions, CodexBarTuiActionKind kind, const char *label, const char *target) {
    CodexBarTuiAction *action = g_new0(CodexBarTuiAction, 1);
    action->kind = kind;
    action->label = g_strdup(label);
    action->target = g_strdup(target);
    g_ptr_array_add(actions, action);
}

GPtrArray *codexbar_tui_actions_new(const char *provider_id,
                                    const char *config_path,
                                    const char *dashboard_override) {
    GPtrArray *actions = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_tui_action_free);
    const CodexBarProviderDescriptor *provider = codexbar_provider_registry_find(provider_id);
    if (provider && (dashboard_override || provider->dashboard_url)) {
        add_action(actions,
                   CODEXBAR_TUI_ACTION_DASHBOARD,
                   "Usage Dashboard",
                   dashboard_override ? dashboard_override : provider->dashboard_url);
    }
    if (provider && provider->status_url) {
        add_action(actions, CODEXBAR_TUI_ACTION_STATUS, "Status Page", provider->status_url);
    }
    char *config_uri = config_path ? g_filename_to_uri(config_path, NULL, NULL) : NULL;
    add_action(actions, CODEXBAR_TUI_ACTION_SETTINGS, "Open Config", config_uri);
    g_free(config_uri);
    add_action(actions, CODEXBAR_TUI_ACTION_ABOUT, "About CodexBar", NULL);
    add_action(actions, CODEXBAR_TUI_ACTION_QUIT, "Quit", NULL);
    return actions;
}

void codexbar_tui_action_free(CodexBarTuiAction *action) {
    if (!action) return;
    g_free(action->label);
    g_free(action->target);
    g_free(action);
}

static gboolean exact_loopback(const char *host) {
    if (g_ascii_strcasecmp(host, "localhost") == 0) return TRUE;
    GInetAddress *address = g_inet_address_new_from_string(host);
    gboolean loopback = address && g_inet_address_get_is_loopback(address);
    g_clear_object(&address);
    return loopback;
}

gboolean codexbar_tui_uri_is_allowed(const char *uri, gboolean allow_file) {
    if (!uri) return FALSE;
    GError *error = NULL;
    GUri *parsed = g_uri_parse(uri, G_URI_FLAGS_NONE, &error);
    g_clear_error(&error);
    if (!parsed) return FALSE;
    const char *scheme = g_uri_get_scheme(parsed);
    const char *host = g_uri_get_host(parsed);
    gboolean allowed = FALSE;
    if (allow_file && g_strcmp0(scheme, "file") == 0) {
        const char *path = g_uri_get_path(parsed);
        allowed = g_uri_get_userinfo(parsed) == NULL && (!host || host[0] == '\0' || g_str_equal(host, "localhost")) &&
                  path && g_path_is_absolute(path) && g_uri_get_query(parsed) == NULL &&
                  g_uri_get_fragment(parsed) == NULL;
    } else if (g_strcmp0(scheme, "https") == 0 && host && host[0] != '\0') {
        allowed = g_uri_get_userinfo(parsed) == NULL;
    } else if (g_strcmp0(scheme, "http") == 0 && host && exact_loopback(host)) {
        allowed = g_uri_get_userinfo(parsed) == NULL;
    }
    g_uri_unref(parsed);
    return allowed;
}

static gboolean default_launcher(const char *uri, GError **error, gpointer user_data) {
    (void)user_data;
    return g_app_info_launch_default_for_uri(uri, NULL, error);
}

gboolean codexbar_tui_action_execute(const CodexBarTuiAction *action,
                                     CodexBarTuiUriLauncher launcher,
                                     gpointer user_data,
                                     CodexBarTuiEffect *effect,
                                     GError **error) {
    g_return_val_if_fail(action != NULL, FALSE);
    g_return_val_if_fail(effect != NULL, FALSE);
    switch (action->kind) {
    case CODEXBAR_TUI_ACTION_ABOUT:
        *effect = CODEXBAR_TUI_EFFECT_SHOW_ABOUT;
        return TRUE;
    case CODEXBAR_TUI_ACTION_QUIT:
        *effect = CODEXBAR_TUI_EFFECT_QUIT;
        return TRUE;
    case CODEXBAR_TUI_ACTION_DASHBOARD:
    case CODEXBAR_TUI_ACTION_STATUS:
    case CODEXBAR_TUI_ACTION_SETTINGS:
        if (!codexbar_tui_uri_is_allowed(action->target, action->kind == CODEXBAR_TUI_ACTION_SETTINGS)) {
            g_set_error_literal(error, action_error_quark(), 1, "Action target is not a trusted URI");
            return FALSE;
        }
        if (!launcher) {
            g_set_error_literal(error, action_error_quark(), 3, "URI launcher is required");
            return FALSE;
        }
        if (!launcher(action->target, error, user_data)) return FALSE;
        *effect = CODEXBAR_TUI_EFFECT_OPENED;
        return TRUE;
    }
    g_set_error_literal(error, action_error_quark(), 2, "Unknown TUI action");
    return FALSE;
}

gboolean codexbar_tui_action_execute_default(const CodexBarTuiAction *action,
                                             CodexBarTuiEffect *effect,
                                             GError **error) {
    return codexbar_tui_action_execute(action, default_launcher, NULL, effect, error);
}
