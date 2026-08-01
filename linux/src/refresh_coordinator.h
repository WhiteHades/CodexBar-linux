#pragma once

#include <glib.h>

typedef struct CodexBarRefreshCoordinator CodexBarRefreshCoordinator;

typedef struct {
    guint64 generation;
    gboolean should_start;
    gboolean replaced_active;
} CodexBarRefreshRequest;

CodexBarRefreshCoordinator *codexbar_refresh_coordinator_new(void);
void codexbar_refresh_coordinator_free(CodexBarRefreshCoordinator *coordinator);
CodexBarRefreshRequest codexbar_refresh_coordinator_request(CodexBarRefreshCoordinator *coordinator,
                                                            const char *key,
                                                            gboolean replace);
gboolean codexbar_refresh_coordinator_is_current(CodexBarRefreshCoordinator *coordinator,
                                                 const char *key,
                                                 guint64 generation);
gboolean codexbar_refresh_coordinator_complete(CodexBarRefreshCoordinator *coordinator,
                                               const char *key,
                                               guint64 generation);
void codexbar_refresh_coordinator_invalidate(CodexBarRefreshCoordinator *coordinator, const char *key);
