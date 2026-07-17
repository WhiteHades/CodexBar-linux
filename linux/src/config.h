#pragma once

#include <glib.h>

typedef struct {
    char *id;
    gboolean enabled;
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
} CodexBarProviderConfig;

typedef struct {
    char *path;
    GPtrArray *providers;
} CodexBarConfig;

CodexBarConfig *codexbar_config_load(GError **error);
void codexbar_config_free(CodexBarConfig *config);
