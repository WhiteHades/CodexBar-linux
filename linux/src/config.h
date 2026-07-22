#pragma once

#include <glib.h>

typedef struct json_object json_object;

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
    json_object *raw;
} CodexBarProviderConfig;

typedef struct {
    int version;
    char *path;
    GPtrArray *providers;
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
void codexbar_config_free(CodexBarConfig *config);
