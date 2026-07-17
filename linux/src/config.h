#pragma once

#include <glib.h>

typedef struct {
    char *id;
    gboolean enabled;
    char *source;
    char *api_key;
    char *region;
} CodexBarProviderConfig;

typedef struct {
    char *path;
    GPtrArray *providers;
} CodexBarConfig;

CodexBarConfig *codexbar_config_load(GError **error);
void codexbar_config_free(CodexBarConfig *config);
