#pragma once

#include "model.h"

#include <gio/gio.h>

typedef struct CodexBarTransitionState CodexBarTransitionState;

typedef enum {
    CODEXBAR_PROVIDER_TRANSITION_NONE,
    CODEXBAR_PROVIDER_TRANSITION_UNAVAILABLE,
    CODEXBAR_PROVIDER_TRANSITION_RECOVERED,
} CodexBarProviderTransition;

typedef enum {
    CODEXBAR_SESSION_QUOTA_NONE,
    CODEXBAR_SESSION_QUOTA_DEPLETED,
    CODEXBAR_SESSION_QUOTA_RESTORED,
    CODEXBAR_SESSION_QUOTA_BASELINE_CHANGED,
    CODEXBAR_SESSION_QUOTA_STALE_CODEX_OBSERVATION,
    CODEXBAR_SESSION_QUOTA_SUPPRESSED_CODEX_RESTORE,
    CODEXBAR_SESSION_QUOTA_AWAITING_CODEX_RESTORE_CONFIRMATION,
} CodexBarSessionQuotaOutcome;

typedef struct {
    double remaining;
    char *source;
    gint64 observed_at_milliseconds;
    char *codex_owner_key;
    gboolean has_trusted_reset_boundary;
    gint64 trusted_reset_boundary_milliseconds;
    gboolean has_pending_codex_restore_observation;
    gint64 pending_codex_restore_observation_milliseconds;
} CodexBarSessionQuotaState;

typedef struct {
    const char *provider;
    double remaining;
    const char *source;
    gint64 observed_at_milliseconds;
    gint64 evaluation_time_milliseconds;
    const char *codex_owner_key;
    gboolean has_reset_boundary;
    gint64 reset_boundary_milliseconds;
} CodexBarSessionQuotaObservation;

CodexBarTransitionState *codexbar_transition_state_new(void);
void codexbar_transition_state_free(CodexBarTransitionState *state);

void codexbar_transition_state_set_config_revision(CodexBarTransitionState *state, const char *revision);
void codexbar_transition_forget_quota(CodexBarTransitionState *state,
                                      const char *provider,
                                      const char *account,
                                      const char *window,
                                      const char *window_id);
gboolean codexbar_transition_quota_observe(CodexBarTransitionState *state,
                                           const char *provider,
                                           const char *account,
                                           const char *window,
                                           const char *window_id,
                                           double current_usage,
                                           double *previous_usage);
CodexBarProviderTransition codexbar_transition_provider_status(CodexBarTransitionState *state,
                                                               const char *provider,
                                                               CodexBarServiceStatusIndicator indicator);
gboolean codexbar_transition_rate_limit_allow(CodexBarTransitionState *state,
                                              const char *event,
                                              const char *provider,
                                              const char *account,
                                              const char *window,
                                              gint64 now_milliseconds);
const char *codexbar_transition_refresh_failure_status(const GError *error);
void codexbar_session_quota_state_clear(CodexBarSessionQuotaState *state);
CodexBarSessionQuotaOutcome codexbar_session_quota_evaluate(const CodexBarSessionQuotaState *previous,
                                                            const CodexBarSessionQuotaObservation *observation,
                                                            gboolean notifications_enabled,
                                                            gboolean force_baseline,
                                                            CodexBarSessionQuotaState *next);
