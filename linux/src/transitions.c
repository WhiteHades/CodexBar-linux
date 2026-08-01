#include "transitions.h"

#include <math.h>

#define RATE_LIMIT_MILLISECONDS (10 * 60 * 1000)
#define RESET_EQUIVALENCE_MILLISECONDS (2 * 60 * 1000)
#define SESSION_DEPLETED_THRESHOLD 0.0001

struct CodexBarTransitionState {
    GHashTable *quota_usage;
    GHashTable *provider_outages;
    GHashTable *last_fired;
    char *config_revision;
};

static char *transition_key(const char *first,
                            const char *second,
                            const char *third,
                            const char *fourth) {
    return g_strjoin("\x1f", first ? first : "", second ? second : "", third ? third : "", fourth ? fourth : "", NULL);
}

CodexBarTransitionState *codexbar_transition_state_new(void) {
    CodexBarTransitionState *state = g_new0(CodexBarTransitionState, 1);
    state->quota_usage = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    state->provider_outages = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    state->last_fired = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    return state;
}

void codexbar_transition_state_free(CodexBarTransitionState *state) {
    if (!state) return;
    g_hash_table_unref(state->quota_usage);
    g_hash_table_unref(state->provider_outages);
    g_hash_table_unref(state->last_fired);
    g_free(state->config_revision);
    g_free(state);
}

void codexbar_transition_state_set_config_revision(CodexBarTransitionState *state, const char *revision) {
    g_return_if_fail(state != NULL);
    if (g_strcmp0(state->config_revision, revision) == 0) return;
    g_hash_table_remove_all(state->quota_usage);
    g_free(state->config_revision);
    state->config_revision = g_strdup(revision);
}

void codexbar_transition_forget_quota(CodexBarTransitionState *state,
                                      const char *provider,
                                      const char *account,
                                      const char *window,
                                      const char *window_id) {
    g_return_if_fail(state != NULL);
    char *key = transition_key(provider, account, window, window_id);
    g_hash_table_remove(state->quota_usage, key);
    g_free(key);
}

gboolean codexbar_transition_quota_observe(CodexBarTransitionState *state,
                                           const char *provider,
                                           const char *account,
                                           const char *window,
                                           const char *window_id,
                                           double current_usage,
                                           double *previous_usage) {
    g_return_val_if_fail(state != NULL, FALSE);
    g_return_val_if_fail(provider != NULL, FALSE);
    g_return_val_if_fail(isfinite(current_usage), FALSE);

    char *key = transition_key(provider, account, window, window_id);
    double *stored = g_hash_table_lookup(state->quota_usage, key);
    if (previous_usage) *previous_usage = stored ? *stored : NAN;
    gboolean had_previous = stored != NULL;
    double *next = g_new(double, 1);
    *next = current_usage;
    g_hash_table_replace(state->quota_usage, key, next);
    return had_previous;
}

CodexBarProviderTransition codexbar_transition_provider_status(CodexBarTransitionState *state,
                                                               const char *provider,
                                                               CodexBarServiceStatusIndicator indicator) {
    g_return_val_if_fail(state != NULL, CODEXBAR_PROVIDER_TRANSITION_NONE);
    g_return_val_if_fail(provider != NULL, CODEXBAR_PROVIDER_TRANSITION_NONE);

    if (indicator == CODEXBAR_STATUS_MAINTENANCE || indicator == CODEXBAR_STATUS_UNKNOWN) {
        return CODEXBAR_PROVIDER_TRANSITION_NONE;
    }
    gboolean outage = indicator == CODEXBAR_STATUS_MINOR || indicator == CODEXBAR_STATUS_MAJOR ||
                      indicator == CODEXBAR_STATUS_CRITICAL;
    gboolean previous = g_hash_table_contains(state->provider_outages, provider);
    if (outage == previous) return CODEXBAR_PROVIDER_TRANSITION_NONE;
    if (outage) {
        g_hash_table_add(state->provider_outages, g_strdup(provider));
        return CODEXBAR_PROVIDER_TRANSITION_UNAVAILABLE;
    }
    g_hash_table_remove(state->provider_outages, provider);
    return CODEXBAR_PROVIDER_TRANSITION_RECOVERED;
}

gboolean codexbar_transition_rate_limit_allow(CodexBarTransitionState *state,
                                              const char *event,
                                              const char *provider,
                                              const char *account,
                                              const char *window,
                                              gint64 now_milliseconds) {
    g_return_val_if_fail(state != NULL, FALSE);
    g_return_val_if_fail(event != NULL, FALSE);
    g_return_val_if_fail(provider != NULL, FALSE);

    char *key = transition_key(event, provider, account, window);
    gint64 *previous = g_hash_table_lookup(state->last_fired, key);
    if (previous && now_milliseconds - *previous < RATE_LIMIT_MILLISECONDS) {
        g_free(key);
        return FALSE;
    }
    gint64 *next = g_new(gint64, 1);
    *next = now_milliseconds;
    g_hash_table_replace(state->last_fired, key, next);
    return TRUE;
}

const char *codexbar_transition_refresh_failure_status(const GError *error) {
    if (!error) return "error";
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) return "cancelled";
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT)) return "timeout";
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
        return "auth_required";
    }
    if (g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_NETWORK_UNREACHABLE) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_HOST_UNREACHABLE) ||
        g_error_matches(error, G_IO_ERROR, G_IO_ERROR_CONNECTION_CLOSED)) {
        return "offline";
    }
    if (error->domain == G_IO_ERROR) return "network_error";
    return "error";
}

void codexbar_session_quota_state_clear(CodexBarSessionQuotaState *state) {
    if (!state) return;
    g_free(state->source);
    g_free(state->codex_owner_key);
    *state = (CodexBarSessionQuotaState){0};
}

static gboolean is_codex_observation(const CodexBarSessionQuotaObservation *observation) {
    return g_strcmp0(observation->provider, "codex") == 0;
}

static gboolean is_depleted(double remaining) {
    return remaining <= SESSION_DEPLETED_THRESHOLD;
}

static gboolean valid_reset_boundary(const CodexBarSessionQuotaObservation *observation) {
    return observation->has_reset_boundary &&
           observation->reset_boundary_milliseconds > observation->observed_at_milliseconds &&
           observation->reset_boundary_milliseconds > observation->evaluation_time_milliseconds;
}

static gboolean equivalent_reset_boundaries(gint64 first, gint64 second) {
    return llabs(first - second) < RESET_EQUIVALENCE_MILLISECONDS;
}

static void baseline_state(const CodexBarSessionQuotaObservation *observation, CodexBarSessionQuotaState *next) {
    *next = (CodexBarSessionQuotaState){
        .remaining = observation->remaining,
        .source = g_strdup(observation->source),
        .observed_at_milliseconds = observation->observed_at_milliseconds,
        .codex_owner_key = is_codex_observation(observation) ? g_strdup(observation->codex_owner_key) : NULL,
        .has_trusted_reset_boundary = is_codex_observation(observation) && valid_reset_boundary(observation),
        .trusted_reset_boundary_milliseconds = observation->reset_boundary_milliseconds,
    };
}

static void updated_state(const CodexBarSessionQuotaState *previous,
                          const CodexBarSessionQuotaObservation *observation,
                          gboolean preserve_reset_boundary,
                          CodexBarSessionQuotaState *next) {
    baseline_state(observation, next);
    if (!is_codex_observation(observation)) return;
    if (preserve_reset_boundary) {
        next->has_trusted_reset_boundary = previous->has_trusted_reset_boundary;
        next->trusted_reset_boundary_milliseconds = previous->trusted_reset_boundary_milliseconds;
        return;
    }
    if (previous->has_trusted_reset_boundary &&
        (!next->has_trusted_reset_boundary ||
         next->trusted_reset_boundary_milliseconds <= previous->trusted_reset_boundary_milliseconds ||
         equivalent_reset_boundaries(next->trusted_reset_boundary_milliseconds,
                                     previous->trusted_reset_boundary_milliseconds))) {
        next->has_trusted_reset_boundary = TRUE;
        next->trusted_reset_boundary_milliseconds = previous->trusted_reset_boundary_milliseconds;
    }
}

static void preserved_depleted_state(const CodexBarSessionQuotaState *previous,
                                     const CodexBarSessionQuotaObservation *observation,
                                     gboolean pending,
                                     CodexBarSessionQuotaState *next) {
    *next = (CodexBarSessionQuotaState){
        .remaining = previous->remaining,
        .source = g_strdup(observation->source),
        .observed_at_milliseconds = observation->observed_at_milliseconds,
        .codex_owner_key = g_strdup(observation->codex_owner_key),
        .has_trusted_reset_boundary = previous->has_trusted_reset_boundary,
        .trusted_reset_boundary_milliseconds = previous->trusted_reset_boundary_milliseconds,
        .has_pending_codex_restore_observation = pending,
        .pending_codex_restore_observation_milliseconds = observation->observed_at_milliseconds,
    };
}

CodexBarSessionQuotaOutcome codexbar_session_quota_evaluate(const CodexBarSessionQuotaState *previous,
                                                            const CodexBarSessionQuotaObservation *observation,
                                                            gboolean notifications_enabled,
                                                            gboolean force_baseline,
                                                            CodexBarSessionQuotaState *next) {
    g_return_val_if_fail(observation != NULL, CODEXBAR_SESSION_QUOTA_NONE);
    g_return_val_if_fail(observation->provider != NULL, CODEXBAR_SESSION_QUOTA_NONE);
    g_return_val_if_fail(observation->source != NULL, CODEXBAR_SESSION_QUOTA_NONE);
    g_return_val_if_fail(isfinite(observation->remaining), CODEXBAR_SESSION_QUOTA_NONE);
    g_return_val_if_fail(next != NULL, CODEXBAR_SESSION_QUOTA_NONE);

    if (force_baseline) {
        baseline_state(observation, next);
        return CODEXBAR_SESSION_QUOTA_BASELINE_CHANGED;
    }
    if (!previous) {
        baseline_state(observation, next);
        return notifications_enabled && is_depleted(observation->remaining) ? CODEXBAR_SESSION_QUOTA_DEPLETED
                                                                            : CODEXBAR_SESSION_QUOTA_NONE;
    }

    gboolean codex = is_codex_observation(observation);
    gboolean owner_changed = codex && g_strcmp0(previous->codex_owner_key, observation->codex_owner_key) != 0;
    if (g_strcmp0(previous->source, observation->source) != 0 || owner_changed) {
        baseline_state(observation, next);
        return CODEXBAR_SESSION_QUOTA_BASELINE_CHANGED;
    }
    if (codex && observation->observed_at_milliseconds <= previous->observed_at_milliseconds) {
        *next = (CodexBarSessionQuotaState){
            .remaining = previous->remaining,
            .source = g_strdup(previous->source),
            .observed_at_milliseconds = previous->observed_at_milliseconds,
            .codex_owner_key = g_strdup(previous->codex_owner_key),
            .has_trusted_reset_boundary = previous->has_trusted_reset_boundary,
            .trusted_reset_boundary_milliseconds = previous->trusted_reset_boundary_milliseconds,
            .has_pending_codex_restore_observation = previous->has_pending_codex_restore_observation,
            .pending_codex_restore_observation_milliseconds = previous->pending_codex_restore_observation_milliseconds,
        };
        return CODEXBAR_SESSION_QUOTA_STALE_CODEX_OBSERVATION;
    }
    if (!notifications_enabled) {
        updated_state(previous, observation, FALSE, next);
        return CODEXBAR_SESSION_QUOTA_NONE;
    }

    gboolean was_depleted = is_depleted(previous->remaining);
    gboolean now_depleted = is_depleted(observation->remaining);
    gboolean restored = was_depleted && !now_depleted;
    if (!restored || !codex) {
        gboolean preserve_depleted_boundary = codex && previous->has_trusted_reset_boundary && was_depleted && now_depleted;
        gboolean preserve_codex_boundary = preserve_depleted_boundary ||
            (codex && previous->has_trusted_reset_boundary &&
             (observation->evaluation_time_milliseconds < previous->trusted_reset_boundary_milliseconds ||
              observation->observed_at_milliseconds < previous->trusted_reset_boundary_milliseconds));
        updated_state(previous, observation, preserve_codex_boundary, next);
        if (!was_depleted && now_depleted) return CODEXBAR_SESSION_QUOTA_DEPLETED;
        if (restored) return CODEXBAR_SESSION_QUOTA_RESTORED;
        return CODEXBAR_SESSION_QUOTA_NONE;
    }

    if (previous->has_trusted_reset_boundary) {
        if (observation->evaluation_time_milliseconds < previous->trusted_reset_boundary_milliseconds ||
            observation->observed_at_milliseconds < previous->trusted_reset_boundary_milliseconds) {
            preserved_depleted_state(previous, observation, FALSE, next);
            return CODEXBAR_SESSION_QUOTA_SUPPRESSED_CODEX_RESTORE;
        }
        if (valid_reset_boundary(observation) &&
            !equivalent_reset_boundaries(previous->trusted_reset_boundary_milliseconds,
                                         observation->reset_boundary_milliseconds) &&
            observation->reset_boundary_milliseconds > previous->trusted_reset_boundary_milliseconds) {
            updated_state(previous, observation, FALSE, next);
            return CODEXBAR_SESSION_QUOTA_RESTORED;
        }
    }

    if (previous->has_pending_codex_restore_observation &&
        observation->observed_at_milliseconds > previous->pending_codex_restore_observation_milliseconds) {
        updated_state(previous, observation, FALSE, next);
        return CODEXBAR_SESSION_QUOTA_RESTORED;
    }
    preserved_depleted_state(previous, observation, TRUE, next);
    return CODEXBAR_SESSION_QUOTA_AWAITING_CODEX_RESTORE_CONFIRMATION;
}
