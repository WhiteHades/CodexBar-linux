#include "transitions.h"

#include <math.h>

static void test_quota_crossings(void) {
    CodexBarTransitionState *state = codexbar_transition_state_new();
    double previous = 0;
    g_assert_false(codexbar_transition_quota_observe(state, "codex", "one", "session", "five-hour", 0.75, &previous));
    g_assert_true(isnan(previous));
    g_assert_true(codexbar_transition_quota_observe(state, "codex", "one", "session", "five-hour", 0.8, &previous));
    g_assert_cmpfloat(previous, ==, 0.75);
    g_assert_true(previous < 0.8 && 0.8 >= 0.8);
    g_assert_true(codexbar_transition_quota_observe(state, "codex", "one", "session", "five-hour", 0.9, &previous));
    g_assert_false(previous < 0.8 && 0.9 >= 0.8);
    g_assert_false(codexbar_transition_quota_observe(state, "codex", "two", "session", "five-hour", 0.9, NULL));

    codexbar_transition_state_set_config_revision(state, "first");
    g_assert_false(codexbar_transition_quota_observe(state, "codex", "one", "session", "five-hour", 0.9, NULL));
    g_assert_true(codexbar_transition_quota_observe(state, "codex", "one", "session", "five-hour", 0.7, NULL));
    codexbar_transition_forget_quota(state, "codex", "one", "session", "five-hour");
    g_assert_false(codexbar_transition_quota_observe(state, "codex", "one", "session", "five-hour", 0.9, NULL));
    codexbar_transition_state_free(state);
}

static void test_provider_status_transitions(void) {
    CodexBarTransitionState *state = codexbar_transition_state_new();
    g_assert_cmpint(codexbar_transition_provider_status(state, "codex", CODEXBAR_STATUS_NONE), ==,
                    CODEXBAR_PROVIDER_TRANSITION_NONE);
    g_assert_cmpint(codexbar_transition_provider_status(state, "codex", CODEXBAR_STATUS_MINOR), ==,
                    CODEXBAR_PROVIDER_TRANSITION_UNAVAILABLE);
    g_assert_cmpint(codexbar_transition_provider_status(state, "codex", CODEXBAR_STATUS_CRITICAL), ==,
                    CODEXBAR_PROVIDER_TRANSITION_NONE);
    g_assert_cmpint(codexbar_transition_provider_status(state, "codex", CODEXBAR_STATUS_UNKNOWN), ==,
                    CODEXBAR_PROVIDER_TRANSITION_NONE);
    g_assert_cmpint(codexbar_transition_provider_status(state, "codex", CODEXBAR_STATUS_MAINTENANCE), ==,
                    CODEXBAR_PROVIDER_TRANSITION_NONE);
    g_assert_cmpint(codexbar_transition_provider_status(state, "codex", CODEXBAR_STATUS_NONE), ==,
                    CODEXBAR_PROVIDER_TRANSITION_RECOVERED);
    codexbar_transition_state_free(state);
}

static void test_rate_limit(void) {
    CodexBarTransitionState *state = codexbar_transition_state_new();
    g_assert_true(codexbar_transition_rate_limit_allow(state, "refresh_failed", "codex", "one", NULL, 1000));
    g_assert_false(codexbar_transition_rate_limit_allow(state, "refresh_failed", "codex", "one", NULL, 600999));
    g_assert_true(codexbar_transition_rate_limit_allow(state, "refresh_failed", "codex", "one", NULL, 601000));
    g_assert_true(codexbar_transition_rate_limit_allow(state, "refresh_failed", "codex", "two", NULL, 601000));
    codexbar_transition_state_free(state);
}

static void test_failure_categories(void) {
    GError *error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_CANCELLED, "secret");
    g_assert_cmpstr(codexbar_transition_refresh_failure_status(error), ==, "cancelled");
    g_clear_error(&error);
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "secret");
    g_assert_cmpstr(codexbar_transition_refresh_failure_status(error), ==, "timeout");
    g_clear_error(&error);
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_NOT_CONNECTED, "secret");
    g_assert_cmpstr(codexbar_transition_refresh_failure_status(error), ==, "offline");
    g_clear_error(&error);
    error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_FAILED, "secret");
    g_assert_cmpstr(codexbar_transition_refresh_failure_status(error), ==, "network_error");
    g_clear_error(&error);
    error = g_error_new_literal(G_KEY_FILE_ERROR, G_KEY_FILE_ERROR_PARSE, "secret");
    g_assert_cmpstr(codexbar_transition_refresh_failure_status(error), ==, "error");
    g_clear_error(&error);
}

static CodexBarSessionQuotaObservation quota_observation(const char *provider,
                                                         double remaining,
                                                         gint64 observed_at,
                                                         gint64 evaluation_time,
                                                         gboolean has_reset,
                                                         gint64 reset_at) {
    return (CodexBarSessionQuotaObservation){
        .provider = provider,
        .remaining = remaining,
        .source = "cli",
        .observed_at_milliseconds = observed_at,
        .evaluation_time_milliseconds = evaluation_time,
        .codex_owner_key = g_str_equal(provider, "codex") ? "owner" : NULL,
        .has_reset_boundary = has_reset,
        .reset_boundary_milliseconds = reset_at,
    };
}

static void test_session_depletion_and_generic_restore(void) {
    CodexBarSessionQuotaState first = {0};
    CodexBarSessionQuotaObservation observation = quota_observation("claude", 0, 1000, 1000, FALSE, 0);
    g_assert_cmpint(codexbar_session_quota_evaluate(NULL, &observation, TRUE, FALSE, &first), ==,
                    CODEXBAR_SESSION_QUOTA_DEPLETED);

    CodexBarSessionQuotaState restored = {0};
    observation.remaining = 25;
    observation.observed_at_milliseconds = 2000;
    observation.evaluation_time_milliseconds = 2000;
    g_assert_cmpint(codexbar_session_quota_evaluate(&first, &observation, TRUE, FALSE, &restored), ==,
                    CODEXBAR_SESSION_QUOTA_RESTORED);
    codexbar_session_quota_state_clear(&restored);
    codexbar_session_quota_state_clear(&first);
}

static void test_session_source_and_owner_changes_rebaseline(void) {
    CodexBarSessionQuotaObservation observation = quota_observation("codex", 50, 1000, 1000, FALSE, 0);
    CodexBarSessionQuotaState first = {0};
    g_assert_cmpint(codexbar_session_quota_evaluate(NULL, &observation, TRUE, FALSE, &first), ==,
                    CODEXBAR_SESSION_QUOTA_NONE);

    observation.observed_at_milliseconds = 2000;
    observation.evaluation_time_milliseconds = 2000;
    observation.source = "oauth";
    CodexBarSessionQuotaState changed = {0};
    g_assert_cmpint(codexbar_session_quota_evaluate(&first, &observation, TRUE, FALSE, &changed), ==,
                    CODEXBAR_SESSION_QUOTA_BASELINE_CHANGED);
    codexbar_session_quota_state_clear(&changed);

    observation.source = "cli";
    observation.codex_owner_key = "other";
    g_assert_cmpint(codexbar_session_quota_evaluate(&first, &observation, TRUE, FALSE, &changed), ==,
                    CODEXBAR_SESSION_QUOTA_BASELINE_CHANGED);
    codexbar_session_quota_state_clear(&changed);
    codexbar_session_quota_state_clear(&first);
}

static void test_codex_restore_confirmation(void) {
    CodexBarSessionQuotaObservation depleted = quota_observation("codex", 0, 1000, 1000, TRUE, 5000);
    CodexBarSessionQuotaState state = {0};
    g_assert_cmpint(codexbar_session_quota_evaluate(NULL, &depleted, TRUE, FALSE, &state), ==,
                    CODEXBAR_SESSION_QUOTA_DEPLETED);

    CodexBarSessionQuotaObservation early = quota_observation("codex", 25, 2000, 2000, TRUE, 7000);
    CodexBarSessionQuotaState next = {0};
    g_assert_cmpint(codexbar_session_quota_evaluate(&state, &early, TRUE, FALSE, &next), ==,
                    CODEXBAR_SESSION_QUOTA_SUPPRESSED_CODEX_RESTORE);
    codexbar_session_quota_state_clear(&state);
    state = next;
    next = (CodexBarSessionQuotaState){0};

    CodexBarSessionQuotaObservation ambiguous = quota_observation("codex", 25, 6000, 6000, FALSE, 0);
    g_assert_cmpint(codexbar_session_quota_evaluate(&state, &ambiguous, TRUE, FALSE, &next), ==,
                    CODEXBAR_SESSION_QUOTA_AWAITING_CODEX_RESTORE_CONFIRMATION);
    codexbar_session_quota_state_clear(&state);
    state = next;
    next = (CodexBarSessionQuotaState){0};

    ambiguous.observed_at_milliseconds = 7000;
    ambiguous.evaluation_time_milliseconds = 7000;
    g_assert_cmpint(codexbar_session_quota_evaluate(&state, &ambiguous, TRUE, FALSE, &next), ==,
                    CODEXBAR_SESSION_QUOTA_RESTORED);
    codexbar_session_quota_state_clear(&next);
    codexbar_session_quota_state_clear(&state);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/transitions/quota-crossings", test_quota_crossings);
    g_test_add_func("/transitions/provider-status", test_provider_status_transitions);
    g_test_add_func("/transitions/rate-limit", test_rate_limit);
    g_test_add_func("/transitions/failure-categories", test_failure_categories);
    g_test_add_func("/transitions/session-depletion", test_session_depletion_and_generic_restore);
    g_test_add_func("/transitions/session-rebaseline", test_session_source_and_owner_changes_rebaseline);
    g_test_add_func("/transitions/codex-restore-confirmation", test_codex_restore_confirmation);
    return g_test_run();
}
