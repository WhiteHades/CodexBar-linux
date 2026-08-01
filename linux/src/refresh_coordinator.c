#include "refresh_coordinator.h"

typedef struct {
    guint64 generation;
    gboolean active;
} RefreshState;

struct CodexBarRefreshCoordinator {
    GHashTable *states;
    GMutex mutex;
};

static RefreshState *state_for_key(CodexBarRefreshCoordinator *coordinator, const char *key) {
    RefreshState *state = g_hash_table_lookup(coordinator->states, key);
    if (state) return state;
    state = g_new0(RefreshState, 1);
    g_hash_table_insert(coordinator->states, g_strdup(key), state);
    return state;
}

CodexBarRefreshCoordinator *codexbar_refresh_coordinator_new(void) {
    CodexBarRefreshCoordinator *coordinator = g_new0(CodexBarRefreshCoordinator, 1);
    coordinator->states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    g_mutex_init(&coordinator->mutex);
    return coordinator;
}

void codexbar_refresh_coordinator_free(CodexBarRefreshCoordinator *coordinator) {
    if (!coordinator) return;
    g_hash_table_unref(coordinator->states);
    g_mutex_clear(&coordinator->mutex);
    g_free(coordinator);
}

CodexBarRefreshRequest codexbar_refresh_coordinator_request(CodexBarRefreshCoordinator *coordinator,
                                                            const char *key,
                                                            gboolean replace) {
    g_return_val_if_fail(coordinator != NULL && key != NULL,
                         ((CodexBarRefreshRequest){.generation = 0, .should_start = FALSE}));
    g_mutex_lock(&coordinator->mutex);
    RefreshState *state = state_for_key(coordinator, key);
    gboolean was_active = state->active;
    if (was_active && !replace) {
        CodexBarRefreshRequest request = {.generation = state->generation, .should_start = FALSE};
        g_mutex_unlock(&coordinator->mutex);
        return request;
    }
    state->generation++;
    state->active = TRUE;
    CodexBarRefreshRequest request = {
        .generation = state->generation,
        .should_start = TRUE,
        .replaced_active = was_active,
    };
    g_mutex_unlock(&coordinator->mutex);
    return request;
}

gboolean codexbar_refresh_coordinator_is_current(CodexBarRefreshCoordinator *coordinator,
                                                 const char *key,
                                                 guint64 generation) {
    g_return_val_if_fail(coordinator != NULL && key != NULL, FALSE);
    g_mutex_lock(&coordinator->mutex);
    RefreshState *state = g_hash_table_lookup(coordinator->states, key);
    gboolean current = state && state->active && state->generation == generation;
    g_mutex_unlock(&coordinator->mutex);
    return current;
}

gboolean codexbar_refresh_coordinator_complete(CodexBarRefreshCoordinator *coordinator,
                                               const char *key,
                                               guint64 generation) {
    g_return_val_if_fail(coordinator != NULL && key != NULL, FALSE);
    g_mutex_lock(&coordinator->mutex);
    RefreshState *state = g_hash_table_lookup(coordinator->states, key);
    gboolean current = state && state->active && state->generation == generation;
    if (current) state->active = FALSE;
    g_mutex_unlock(&coordinator->mutex);
    return current;
}

void codexbar_refresh_coordinator_invalidate(CodexBarRefreshCoordinator *coordinator, const char *key) {
    g_return_if_fail(coordinator != NULL && key != NULL);
    g_mutex_lock(&coordinator->mutex);
    RefreshState *state = state_for_key(coordinator, key);
    state->generation++;
    state->active = FALSE;
    g_mutex_unlock(&coordinator->mutex);
}
