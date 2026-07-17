#pragma once

#include <glib.h>

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
    char *provider;
    char *account;
    char *plan;
    char *source;
    char *note;
    char *error;
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
void codexbar_quota_window_free(CodexBarQuotaWindow *window);
CodexBarBalance *codexbar_balance_new(const char *id, const char *title, double remaining, const char *unit);
void codexbar_provider_add_balance(CodexBarProvider *provider, CodexBarBalance *balance);
CodexBarBalance *codexbar_provider_balance(const CodexBarProvider *provider, guint index);
void codexbar_balance_free(CodexBarBalance *balance);
void codexbar_provider_free(CodexBarProvider *provider);
void codexbar_snapshot_free(CodexBarSnapshot *snapshot);
double codexbar_snapshot_highest_used(const CodexBarSnapshot *snapshot);
