#pragma once

#include <glib.h>

typedef struct {
    gboolean available;
    double used_percent;
    char *reset_description;
    char *resets_at;
} CodexBarRateWindow;

typedef struct {
    char *provider;
    char *account;
    char *source;
    char *error;
    CodexBarRateWindow primary;
    CodexBarRateWindow secondary;
    CodexBarRateWindow tertiary;
    gboolean has_credits;
    double credits_remaining;
} CodexBarProvider;

typedef struct {
    GPtrArray *providers;
} CodexBarSnapshot;

CodexBarSnapshot *codexbar_snapshot_parse(const char *json, GError **error);
void codexbar_snapshot_free(CodexBarSnapshot *snapshot);
double codexbar_snapshot_highest_used(const CodexBarSnapshot *snapshot);
