#pragma once

#include <glib.h>

#include "refresh_policy.h"

typedef struct json_object json_object;

typedef enum {
    CODEXBAR_CODEX_SOURCE_LIVE_SYSTEM,
    CODEXBAR_CODEX_SOURCE_MANAGED_ACCOUNT,
    CODEXBAR_CODEX_SOURCE_PROFILE_HOME,
} CodexBarCodexActiveSourceKind;

typedef struct {
    char *id;
    gboolean enabled;
    gboolean has_enabled;
    char *source;
    gboolean extras_enabled;
    gboolean has_extras_enabled;
    char *api_key;
    char *secret_key;
    char *region;
    char *workspace_id;
    char *enterprise_host;
    char *aws_profile;
    char *aws_auth_mode;
    gboolean has_codex_active_source;
    CodexBarCodexActiveSourceKind codex_active_source;
    char *codex_active_account_id;
    char *codex_active_home_path;
    gboolean has_codex_profile_home_paths;
    GPtrArray *codex_profile_home_paths;
    json_object *raw;
} CodexBarProviderConfig;

typedef struct {
    int version;
    char *path;
    GPtrArray *providers;
    CodexBarRefreshFrequency refresh_frequency;
    gboolean agent_sessions_enabled;
    char *agent_sessions_manual_hosts;
    gboolean quota_warning_notifications_enabled;
    gboolean quota_warning_session_enabled;
    gboolean quota_warning_weekly_enabled;
    guint quota_warning_session_threshold_count;
    guint quota_warning_weekly_threshold_count;
    int quota_warning_session_thresholds[100];
    int quota_warning_weekly_thresholds[100];
    gboolean historical_tracking_enabled;
    json_object *raw;
    gboolean loaded_from_disk;
    char *loaded_digest;
    int lock_fd;
} CodexBarConfig;

typedef struct {
    gboolean error;
    char *provider;
    char *field;
    char *code;
    char *message;
} CodexBarConfigIssue;

char *codexbar_config_resolve_path(void);
CodexBarConfig *codexbar_config_load(GError **error);
CodexBarConfig *codexbar_config_load_for_update(GError **error);
gboolean codexbar_config_save(CodexBarConfig *config, GError **error);
char *codexbar_config_render_json(const CodexBarConfig *config, gboolean pretty);
GPtrArray *codexbar_config_validate(const CodexBarConfig *config);
void codexbar_config_issue_free(CodexBarConfigIssue *issue);
CodexBarProviderConfig *codexbar_config_provider(CodexBarConfig *config, const char *id);
gboolean codexbar_config_set_enabled(CodexBarConfig *config, const char *id, gboolean enabled, GError **error);
gboolean codexbar_config_set_api_key(CodexBarConfig *config,
                                      const char *id,
                                      const char *api_key,
                                      size_t api_key_length,
                                      gboolean enable,
                                      GError **error);
gboolean codexbar_config_set_codex_active_source(CodexBarConfig *config,
                                                  CodexBarCodexActiveSourceKind source,
                                                  const char *value,
                                                  GError **error);
void codexbar_config_free(CodexBarConfig *config);
