#include "refresh_coordinator.h"
#include "refresh_policy.h"

#include <glib.h>

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
    g_test_add_func("/refresh/coordinator", test_coordinator);
    return g_test_run();
}
