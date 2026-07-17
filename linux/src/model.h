#pragma once

#include <glib.h>

typedef enum {
    CODEXBAR_STATUS_NONE,
    CODEXBAR_STATUS_MINOR,
    CODEXBAR_STATUS_MAJOR,
    CODEXBAR_STATUS_CRITICAL,
    CODEXBAR_STATUS_MAINTENANCE,
    CODEXBAR_STATUS_UNKNOWN,
} CodexBarServiceStatusIndicator;

typedef enum {
    CODEXBAR_PACE_ON_TRACK,
    CODEXBAR_PACE_SLIGHTLY_AHEAD,
    CODEXBAR_PACE_AHEAD,
    CODEXBAR_PACE_FAR_AHEAD,
    CODEXBAR_PACE_SLIGHTLY_BEHIND,
    CODEXBAR_PACE_BEHIND,
    CODEXBAR_PACE_FAR_BEHIND,
    CODEXBAR_PACE_UNKNOWN,
} CodexBarPaceStage;

typedef struct {
    CodexBarPaceStage stage;
    double delta_percent;
    double expected_used_percent;
    gboolean will_last;
    gboolean has_eta;
    double eta_seconds;
    gboolean has_runout_probability;
    double runout_probability;
    char *summary;
} CodexBarPace;

typedef struct {
    char *id;
    char *title;
    gboolean usage_known;
    double used_percent;
    gboolean has_window_minutes;
    gint64 window_minutes;
    gboolean has_resets_at;
    gint64 resets_at_ms;
    char *detail;
    char *reset_description;
    CodexBarPace *pace;
} CodexBarQuotaWindow;

typedef struct {
    char *id;
    char *title;
    double remaining;
    char *unit;
    gboolean has_used;
    double used;
    gboolean has_limit;
    double limit;
    gboolean has_expiry;
    gint64 expiry_ms;
    gboolean has_resets_at;
    gint64 resets_at_ms;
} CodexBarBalance;

typedef struct {
    CodexBarServiceStatusIndicator indicator;
    char *description;
    char *url;
    gboolean has_updated_at;
    gint64 updated_at_ms;
} CodexBarServiceStatus;

typedef struct {
    double used;
    double limit;
    char *currency;
    char *period;
    gboolean has_resets_at;
    gint64 resets_at_ms;
    gboolean has_next_regen;
    double next_regen;
    gboolean has_personal_used;
    double personal_used;
    gboolean has_updated_at;
    gint64 updated_at_ms;
} CodexBarProviderCost;

typedef struct {
    /* Top-level tokenCost uses CostUsageTokenSnapshot's session* and last30Days* JSON fields. */
    gboolean has_today_tokens;
    gint64 today_tokens;
    gboolean has_today_cost;
    double today_cost;
    gboolean has_today_requests;
    gint64 today_requests;
    gboolean has_last_days_tokens;
    gint64 last_days_tokens;
    gboolean has_last_days_cost;
    double last_days_cost;
    gboolean has_last_days_requests;
    gint64 last_days_requests;
    char *currency;
    char *history_label;
    gboolean has_history_days;
    gint64 history_days;
    gboolean has_updated_at;
    gint64 updated_at_ms;
} CodexBarTokenCost;

typedef struct {
    char *organization;
    char *account_id;
    char *login_method;
} CodexBarProviderIdentity;

typedef struct {
    char *provider;
    char *account;
    char *plan;
    char *source;
    char *note;
    char *error;
    CodexBarProviderIdentity *identity;
    CodexBarServiceStatus *status;
    CodexBarProviderCost *provider_cost;
    CodexBarTokenCost *token_cost;
    gboolean has_subscription_expires_at;
    gint64 subscription_expires_at_ms;
    gboolean has_subscription_renews_at;
    gint64 subscription_renews_at_ms;
    gboolean has_updated_at;
    gint64 updated_at_ms;
    GPtrArray *quota_windows;
    GPtrArray *balances;
} CodexBarProvider;

typedef struct {
    GPtrArray *providers;
} CodexBarSnapshot;

CodexBarSnapshot *codexbar_snapshot_parse(const char *json, GError **error);
CodexBarProvider *codexbar_provider_new(void);
CodexBarQuotaWindow *codexbar_quota_window_new(const char *id, const char *title);
void codexbar_provider_add_quota_window(CodexBarProvider *provider, CodexBarQuotaWindow *window);
CodexBarQuotaWindow *codexbar_provider_quota_window(const CodexBarProvider *provider, guint index);
void codexbar_pace_free(CodexBarPace *pace);
void codexbar_quota_window_free(CodexBarQuotaWindow *window);
CodexBarBalance *codexbar_balance_new(const char *id, const char *title, double remaining, const char *unit);
void codexbar_provider_add_balance(CodexBarProvider *provider, CodexBarBalance *balance);
CodexBarBalance *codexbar_provider_balance(const CodexBarProvider *provider, guint index);
void codexbar_balance_free(CodexBarBalance *balance);
void codexbar_service_status_free(CodexBarServiceStatus *status);
void codexbar_provider_cost_free(CodexBarProviderCost *cost);
void codexbar_token_cost_free(CodexBarTokenCost *cost);
void codexbar_provider_identity_free(CodexBarProviderIdentity *identity);
void codexbar_provider_free(CodexBarProvider *provider);
void codexbar_snapshot_free(CodexBarSnapshot *snapshot);
double codexbar_provider_highest_used(const CodexBarProvider *provider);
double codexbar_snapshot_highest_used(const CodexBarSnapshot *snapshot);
