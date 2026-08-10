#include "pace.h"

#include <math.h>

static CodexBarPaceStage pace_stage(double delta) {
    double distance = fabs(delta);
    if (distance <= 2) return CODEXBAR_PACE_ON_TRACK;
    if (distance <= 6) return delta >= 0 ? CODEXBAR_PACE_SLIGHTLY_AHEAD : CODEXBAR_PACE_SLIGHTLY_BEHIND;
    if (distance <= 12) return delta >= 0 ? CODEXBAR_PACE_AHEAD : CODEXBAR_PACE_BEHIND;
    return delta >= 0 ? CODEXBAR_PACE_FAR_AHEAD : CODEXBAR_PACE_FAR_BEHIND;
}

static char *pace_summary(const CodexBarPace *pace) {
    int distance = (int)round(fabs(pace->delta_percent));
    const char *position = distance == 0 || pace->stage == CODEXBAR_PACE_ON_TRACK
                               ? "On pace"
                           : pace->delta_percent > 0 ? "in deficit"
                                                    : "in reserve";
    char *left = distance == 0 || pace->stage == CODEXBAR_PACE_ON_TRACK
                     ? g_strdup(position)
                     : g_strdup_printf("%d%% %s", distance, position);
    char *summary = NULL;
    if (pace->will_last) {
        if (pace->has_speed_multiplier && pace->delta_percent < -15 && pace->speed_multiplier >= 1.5) {
            summary = g_strdup_printf("%s · Lasts until reset · 1.5x headroom", left);
        } else {
            summary = g_strdup_printf("%s · Lasts until reset", left);
        }
    } else if (pace->has_eta) {
        gint64 seconds = MAX((gint64)0, (gint64)round(pace->eta_seconds));
        if (seconds == 0) summary = g_strdup_printf("%s · Runs out now", left);
        else if (seconds < 3600) summary = g_strdup_printf("%s · Runs out in %" G_GINT64_FORMAT "m", left, MAX((gint64)1, seconds / 60));
        else if (seconds < 86400) summary = g_strdup_printf("%s · Runs out in %" G_GINT64_FORMAT "h", left, seconds / 3600);
        else summary = g_strdup_printf("%s · Runs out in %" G_GINT64_FORMAT "d", left, seconds / 86400);
    } else {
        summary = g_strdup(left);
    }
    g_free(left);
    return summary;
}

CodexBarPace *codexbar_pace_calculate(const CodexBarQuotaWindow *window,
                                      gint64 now_ms,
                                      gint64 default_window_minutes) {
    if (!window || !window->usage_known || !window->has_resets_at || window->used_percent >= 100) return NULL;
    gint64 minutes = window->has_window_minutes ? window->window_minutes : default_window_minutes;
    if (minutes <= 0) return NULL;
    double duration = (double)minutes * 60;
    double until_reset = (double)(window->resets_at_ms - now_ms) / 1000;
    if (until_reset <= 0 || until_reset > duration) return NULL;
    double elapsed = CLAMP(duration - until_reset, 0, duration);
    double actual = CLAMP(window->used_percent, 0, 100);
    if (elapsed == 0 && actual > 0) return NULL;
    double expected = CLAMP(elapsed / duration * 100, 0, 100);
    if (expected < 3) return NULL;

    CodexBarPace *pace = g_new0(CodexBarPace, 1);
    pace->expected_used_percent = expected;
    pace->delta_percent = actual - expected;
    pace->stage = pace_stage(pace->delta_percent);
    double projected_remaining = elapsed > 0 ? actual * until_reset / elapsed : 0;
    double remaining_capacity = 100 - actual;
    if (remaining_capacity > 0 && projected_remaining > 0) {
        pace->has_speed_multiplier = TRUE;
        pace->speed_multiplier = remaining_capacity / projected_remaining;
    }
    if (elapsed > 0 && actual > 0) {
        double eta = remaining_capacity / (actual / elapsed);
        if (eta >= until_reset) pace->will_last = TRUE;
        else {
            pace->has_eta = TRUE;
            pace->eta_seconds = eta;
        }
    } else if (elapsed > 0) {
        pace->will_last = TRUE;
    }
    pace->summary = pace_summary(pace);
    return pace;
}

static gboolean session_pace_supported(const char *provider, const CodexBarQuotaWindow *window) {
    if (!provider || !window->has_window_minutes || window->window_minutes > 360) return FALSE;
    if (g_str_equal(provider, "codex") || g_str_equal(provider, "claude") ||
        g_str_equal(provider, "ollama") || g_str_equal(provider, "kimi") ||
        g_str_equal(provider, "notion")) return TRUE;
    return g_str_equal(provider, "antigravity") && window->window_minutes == 300;
}

void codexbar_pace_attach_snapshot(CodexBarSnapshot *snapshot, gint64 now_ms) {
    if (!snapshot) return;
    for (guint provider_index = 0; provider_index < snapshot->providers->len; provider_index++) {
        CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, provider_index);
        if (provider->error) continue;
        for (guint window_index = 0; window_index < provider->quota_windows->len; window_index++) {
            CodexBarQuotaWindow *window = g_ptr_array_index(provider->quota_windows, window_index);
            gboolean weekly = window->has_window_minutes && window->window_minutes > 360;
            gint64 fallback = !window->has_window_minutes && g_str_equal(provider->provider, "codex") &&
                                     window_index > 0
                                 ? 10080
                                 : 0;
            if (!weekly && fallback == 0 && !session_pace_supported(provider->provider, window)) continue;
            CodexBarPace *pace = codexbar_pace_calculate(window, now_ms, fallback);
            if (!pace) continue;
            codexbar_pace_free(window->pace);
            window->pace = pace;
        }
    }
}
