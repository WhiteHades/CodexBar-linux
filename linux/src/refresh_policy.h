#pragma once

#include <glib.h>

typedef enum {
    CODEXBAR_REFRESH_MANUAL,
    CODEXBAR_REFRESH_ONE_MINUTE,
    CODEXBAR_REFRESH_TWO_MINUTES,
    CODEXBAR_REFRESH_FIVE_MINUTES,
    CODEXBAR_REFRESH_FIFTEEN_MINUTES,
    CODEXBAR_REFRESH_THIRTY_MINUTES,
    CODEXBAR_REFRESH_ADAPTIVE,
    CODEXBAR_REFRESH_ADAPTIVE_AGENT_AWARE,
} CodexBarRefreshFrequency;

typedef enum {
    CODEXBAR_REFRESH_REASON_FIXED,
    CODEXBAR_REFRESH_REASON_RECENT_MENU,
    CODEXBAR_REFRESH_REASON_WARM_MENU,
    CODEXBAR_REFRESH_REASON_IDLE_MENU,
    CODEXBAR_REFRESH_REASON_LONG_IDLE,
    CODEXBAR_REFRESH_REASON_RECENT_ACTIVITY,
    CODEXBAR_REFRESH_REASON_CONSTRAINED,
    CODEXBAR_REFRESH_REASON_MANUAL,
} CodexBarRefreshReason;

typedef struct {
    gboolean has_last_menu_open;
    double menu_age_seconds;
    gboolean has_recent_activity;
    double activity_age_seconds;
    gboolean low_power;
    gboolean constrained;
} CodexBarRefreshPolicyInput;

typedef struct {
    guint delay_seconds;
    CodexBarRefreshReason reason;
} CodexBarRefreshDecision;

CodexBarRefreshFrequency codexbar_refresh_frequency_parse(const char *raw, gboolean existing_state);
const char *codexbar_refresh_frequency_raw(CodexBarRefreshFrequency frequency);
gboolean codexbar_refresh_frequency_is_adaptive(CodexBarRefreshFrequency frequency);
CodexBarRefreshDecision codexbar_refresh_policy_decide(CodexBarRefreshFrequency frequency,
                                                       CodexBarRefreshPolicyInput input);
gint64 codexbar_refresh_next_fixed_deadline(gint64 previous_deadline_us,
                                            gint64 completed_at_us,
                                            gint64 interval_us);
