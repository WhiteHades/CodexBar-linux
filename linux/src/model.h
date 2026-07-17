#pragma once

#include <glib.h>

typedef struct {
    gboolean available;
    char *label;
    double used_percent;
    char *reset_description;
    char *resets_at;
} CodexBarRateWindow;

typedef struct {
    char *provider;
    char *account;
    char *plan;
    char *source;
    char *note;
    char *error;
    CodexBarRateWindow primary;
    CodexBarRateWindow secondary;
    CodexBarRateWindow tertiary;
    gboolean has_credits;
    char *credits_label;
    double credits_remaining;
} CodexBarProvider;

typedef struct {
    GPtrArray *providers;
} CodexBarSnapshot;

CodexBarSnapshot *codexbar_snapshot_parse(const char *json, GError **error);
void codexbar_provider_free(CodexBarProvider *provider);
void codexbar_snapshot_free(CodexBarSnapshot *snapshot);
double codexbar_snapshot_highest_used(const CodexBarSnapshot *snapshot);
