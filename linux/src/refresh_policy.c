#include "refresh_policy.h"

#include <string.h>

typedef struct {
    const char *raw;
    CodexBarRefreshFrequency frequency;
    guint seconds;
} FrequencyEntry;

static const FrequencyEntry frequencies[] = {
    {"manual", CODEXBAR_REFRESH_MANUAL, 0},
    {"oneMinute", CODEXBAR_REFRESH_ONE_MINUTE, 60},
    {"twoMinutes", CODEXBAR_REFRESH_TWO_MINUTES, 120},
    {"fiveMinutes", CODEXBAR_REFRESH_FIVE_MINUTES, 300},
    {"fifteenMinutes", CODEXBAR_REFRESH_FIFTEEN_MINUTES, 900},
    {"thirtyMinutes", CODEXBAR_REFRESH_THIRTY_MINUTES, 1800},
    {"adaptive", CODEXBAR_REFRESH_ADAPTIVE, 0},
    {"adaptiveAgentAware", CODEXBAR_REFRESH_ADAPTIVE_AGENT_AWARE, 0},
};

CodexBarRefreshFrequency codexbar_refresh_frequency_parse(const char *raw, gboolean existing_state) {
    if (raw) {
        for (guint index = 0; index < G_N_ELEMENTS(frequencies); index++) {
            if (g_str_equal(raw, frequencies[index].raw)) return frequencies[index].frequency;
        }
    }
    return existing_state ? CODEXBAR_REFRESH_FIVE_MINUTES : CODEXBAR_REFRESH_ADAPTIVE;
}

const char *codexbar_refresh_frequency_raw(CodexBarRefreshFrequency frequency) {
    for (guint index = 0; index < G_N_ELEMENTS(frequencies); index++) {
        if (frequency == frequencies[index].frequency) return frequencies[index].raw;
    }
    return "fiveMinutes";
}

gboolean codexbar_refresh_frequency_is_adaptive(CodexBarRefreshFrequency frequency) {
    return frequency == CODEXBAR_REFRESH_ADAPTIVE || frequency == CODEXBAR_REFRESH_ADAPTIVE_AGENT_AWARE;
}

CodexBarRefreshDecision codexbar_refresh_policy_decide(CodexBarRefreshFrequency frequency,
                                                       CodexBarRefreshPolicyInput input) {
    if (frequency == CODEXBAR_REFRESH_MANUAL) {
        return (CodexBarRefreshDecision){.delay_seconds = 0, .reason = CODEXBAR_REFRESH_REASON_MANUAL};
    }
    if (!codexbar_refresh_frequency_is_adaptive(frequency)) {
        for (guint index = 0; index < G_N_ELEMENTS(frequencies); index++) {
            if (frequency == frequencies[index].frequency) {
                return (CodexBarRefreshDecision){
                    .delay_seconds = frequencies[index].seconds,
                    .reason = CODEXBAR_REFRESH_REASON_FIXED,
                };
            }
        }
    }
    if (input.low_power || input.constrained) {
        return (CodexBarRefreshDecision){.delay_seconds = 1800, .reason = CODEXBAR_REFRESH_REASON_CONSTRAINED};
    }

    guint delay = 1800;
    CodexBarRefreshReason reason = CODEXBAR_REFRESH_REASON_LONG_IDLE;
    if (input.has_last_menu_open && input.menu_age_seconds <= 300) {
        delay = 120;
        reason = CODEXBAR_REFRESH_REASON_RECENT_MENU;
    } else if (input.has_last_menu_open && input.menu_age_seconds <= 3600) {
        delay = 300;
        reason = CODEXBAR_REFRESH_REASON_WARM_MENU;
    } else if (input.has_last_menu_open && input.menu_age_seconds < 14400) {
        delay = 900;
        reason = CODEXBAR_REFRESH_REASON_IDLE_MENU;
    }
    if (input.has_recent_activity && input.activity_age_seconds < 300 && delay > 300) {
        delay = 300;
        reason = CODEXBAR_REFRESH_REASON_RECENT_ACTIVITY;
    }
    return (CodexBarRefreshDecision){.delay_seconds = delay, .reason = reason};
}

gint64 codexbar_refresh_next_fixed_deadline(gint64 previous_deadline_us,
                                            gint64 completed_at_us,
                                            gint64 interval_us) {
    g_return_val_if_fail(interval_us > 0, completed_at_us);
    gint64 deadline = previous_deadline_us + interval_us;
    if (deadline <= completed_at_us) {
        gint64 missed = (completed_at_us - deadline) / interval_us + 1;
        deadline += missed * interval_us;
    }
    return deadline;
}
