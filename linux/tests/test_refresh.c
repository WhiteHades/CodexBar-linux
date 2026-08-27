#include "refresh_coordinator.h"
#include "refresh_policy.h"
#include "model.h"

#include <glib.h>

static CodexBarSnapshot *snapshot_with_resets(gint64 updated_at_ms,
                                              const gint64 *resets_at_ms,
                                              guint count) {
    CodexBarSnapshot *snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("codex");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = updated_at_ms;
    for (guint index = 0; index < count; index++) {
        char *id = g_strdup_printf("window-%u", index);
        CodexBarQuotaWindow *window = codexbar_quota_window_new(id, id);
        window->has_resets_at = TRUE;
        window->resets_at_ms = resets_at_ms[index];
        codexbar_provider_add_quota_window(provider, window);
        g_free(id);
    }
    g_ptr_array_add(snapshot->providers, provider);
    return snapshot;
}

static void test_frequency_migration(void) {
    g_assert_cmpint(codexbar_refresh_frequency_parse(NULL, FALSE), ==, CODEXBAR_REFRESH_ADAPTIVE);
    g_assert_cmpint(codexbar_refresh_frequency_parse(NULL, TRUE), ==, CODEXBAR_REFRESH_FIVE_MINUTES);
    g_assert_cmpint(codexbar_refresh_frequency_parse("invalid", TRUE), ==, CODEXBAR_REFRESH_FIVE_MINUTES);
    g_assert_cmpint(codexbar_refresh_frequency_parse("adaptiveAgentAware", TRUE),
                    ==,
                    CODEXBAR_REFRESH_ADAPTIVE_AGENT_AWARE);
    g_assert_cmpstr(codexbar_refresh_frequency_raw(CODEXBAR_REFRESH_TWO_MINUTES), ==, "twoMinutes");
}

static CodexBarRefreshDecision adaptive(CodexBarRefreshPolicyInput input) {
    return codexbar_refresh_policy_decide(CODEXBAR_REFRESH_ADAPTIVE, input);
}

static void test_adaptive_policy(void) {
    g_assert_cmpuint(adaptive((CodexBarRefreshPolicyInput){}).delay_seconds, ==, 1800);
    g_assert_cmpuint(adaptive((CodexBarRefreshPolicyInput){
                                 .has_last_menu_open = TRUE,
                                 .menu_age_seconds = -1,
                             }).delay_seconds,
                     ==,
                     120);
    g_assert_cmpuint(adaptive((CodexBarRefreshPolicyInput){
                                 .has_last_menu_open = TRUE,
                                 .menu_age_seconds = 300,
                             }).delay_seconds,
                     ==,
                     120);
    g_assert_cmpuint(adaptive((CodexBarRefreshPolicyInput){
                                 .has_last_menu_open = TRUE,
                                 .menu_age_seconds = 301,
                             }).delay_seconds,
                     ==,
                     300);
    g_assert_cmpuint(adaptive((CodexBarRefreshPolicyInput){
                                 .has_last_menu_open = TRUE,
                                 .menu_age_seconds = 3601,
                             }).delay_seconds,
                     ==,
                     900);
    g_assert_cmpuint(adaptive((CodexBarRefreshPolicyInput){
                                 .has_last_menu_open = TRUE,
                                 .menu_age_seconds = 14400,
                             }).delay_seconds,
                     ==,
                     1800);
    CodexBarRefreshDecision active = adaptive((CodexBarRefreshPolicyInput){
        .has_recent_activity = TRUE,
        .activity_age_seconds = 299,
    });
    g_assert_cmpuint(active.delay_seconds, ==, 300);
    g_assert_cmpint(active.reason, ==, CODEXBAR_REFRESH_REASON_RECENT_ACTIVITY);
    CodexBarRefreshDecision constrained = adaptive((CodexBarRefreshPolicyInput){
        .has_last_menu_open = TRUE,
        .menu_age_seconds = 0,
        .low_power = TRUE,
    });
    g_assert_cmpuint(constrained.delay_seconds, ==, 1800);
    g_assert_cmpint(constrained.reason, ==, CODEXBAR_REFRESH_REASON_CONSTRAINED);
}

static void test_fixed_policy(void) {
    g_assert_cmpuint(codexbar_refresh_policy_decide(
                         CODEXBAR_REFRESH_ONE_MINUTE, (CodexBarRefreshPolicyInput){.low_power = TRUE})
                         .delay_seconds,
                     ==,
                     60);
    g_assert_cmpuint(codexbar_refresh_policy_decide(CODEXBAR_REFRESH_MANUAL, (CodexBarRefreshPolicyInput){})
                         .delay_seconds,
                     ==,
                     0);
    g_assert_cmpint(codexbar_refresh_next_fixed_deadline(100, 120, 50), ==, 150);
    g_assert_cmpint(codexbar_refresh_next_fixed_deadline(100, 150, 50), ==, 200);
    g_assert_cmpint(codexbar_refresh_next_fixed_deadline(100, 275, 50), ==, 300);
}

static void test_reset_boundary_policy(void) {
    const gint64 now_ms = 1000000;
    const gint64 resets[] = {now_ms + 10 * 60 * 1000, now_ms + 4 * 60 * 1000};
    CodexBarSnapshot *snapshot = snapshot_with_resets(now_ms, resets, G_N_ELEMENTS(resets));
    CodexBarResetBoundaryDecision decision = codexbar_refresh_next_reset_boundary(
        snapshot, now_ms, 30 * 60, NULL, 0);
    g_assert_true(decision.scheduled);
    g_assert_cmpint(decision.boundary_ms, ==, resets[1] + CODEXBAR_RESET_BOUNDARY_GRACE_MILLISECONDS);
    g_assert_cmpint(decision.refresh_at_ms, ==, decision.boundary_ms);

    const gint64 attempted[] = {decision.boundary_ms};
    decision = codexbar_refresh_next_reset_boundary(snapshot, now_ms, 30 * 60, attempted, 1);
    g_assert_true(decision.scheduled);
    g_assert_cmpint(decision.boundary_ms, ==, resets[0] + CODEXBAR_RESET_BOUNDARY_GRACE_MILLISECONDS);
    const gint64 all_attempted[] = {
        resets[1] + CODEXBAR_RESET_BOUNDARY_GRACE_MILLISECONDS,
        resets[0] + CODEXBAR_RESET_BOUNDARY_GRACE_MILLISECONDS,
    };
    decision = codexbar_refresh_next_reset_boundary(snapshot, now_ms, 30 * 60, all_attempted, 2);
    g_assert_false(decision.scheduled);
    decision = codexbar_refresh_next_reset_boundary(snapshot, now_ms, 60, NULL, 0);
    g_assert_false(decision.scheduled);
    decision = codexbar_refresh_next_reset_boundary(snapshot, now_ms, 0, NULL, 0);
    g_assert_false(decision.scheduled);
    codexbar_snapshot_free(snapshot);

    const gint64 passed_reset[] = {now_ms - 3 * 60 * 1000};
    snapshot = snapshot_with_resets(now_ms - 4 * 60 * 1000, passed_reset, 1);
    decision = codexbar_refresh_next_reset_boundary(snapshot, now_ms, 30 * 60, NULL, 0);
    g_assert_true(decision.scheduled);
    g_assert_cmpint(decision.refresh_at_ms,
                    ==,
                    now_ms + CODEXBAR_RESET_BOUNDARY_MINIMUM_DELAY_MILLISECONDS);
    CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, 0);
    provider->updated_at_ms = now_ms;
    decision = codexbar_refresh_next_reset_boundary(snapshot, now_ms, 30 * 60, NULL, 0);
    g_assert_false(decision.scheduled);
    codexbar_snapshot_free(snapshot);
}

static void test_coordinator(void) {
    CodexBarRefreshCoordinator *coordinator = codexbar_refresh_coordinator_new();
    CodexBarRefreshRequest first = codexbar_refresh_coordinator_request(coordinator, "codex", FALSE);
    g_assert_true(first.should_start);
    g_assert_false(first.replaced_active);
    CodexBarRefreshRequest coalesced = codexbar_refresh_coordinator_request(coordinator, "codex", FALSE);
    g_assert_false(coalesced.should_start);
    g_assert_cmpuint(coalesced.generation, ==, first.generation);
    CodexBarRefreshRequest replacement = codexbar_refresh_coordinator_request(coordinator, "codex", TRUE);
    g_assert_true(replacement.should_start);
    g_assert_true(replacement.replaced_active);
    g_assert_false(codexbar_refresh_coordinator_complete(coordinator, "codex", first.generation));
    g_assert_true(codexbar_refresh_coordinator_is_current(coordinator, "codex", replacement.generation));
    g_assert_true(codexbar_refresh_coordinator_complete(coordinator, "codex", replacement.generation));
    g_assert_false(codexbar_refresh_coordinator_is_current(coordinator, "codex", replacement.generation));
    CodexBarRefreshRequest next = codexbar_refresh_coordinator_request(coordinator, "codex", FALSE);
    codexbar_refresh_coordinator_invalidate(coordinator, "codex");
    g_assert_false(codexbar_refresh_coordinator_complete(coordinator, "codex", next.generation));
    codexbar_refresh_coordinator_free(coordinator);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/refresh/frequency-migration", test_frequency_migration);
    g_test_add_func("/refresh/adaptive-policy", test_adaptive_policy);
    g_test_add_func("/refresh/fixed-policy", test_fixed_policy);
    g_test_add_func("/refresh/reset-boundary", test_reset_boundary_policy);
    g_test_add_func("/refresh/coordinator", test_coordinator);
    return g_test_run();
}
