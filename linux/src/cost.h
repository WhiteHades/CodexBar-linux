#pragma once

#include <glib.h>

typedef struct {
    char *date;
    gint64 input_tokens;
    gint64 output_tokens;
    gint64 cache_read_tokens;
    gint64 cache_creation_tokens;
    gint64 total_tokens;
    gboolean cost_known;
    double cost_usd;
} CodexBarCostDay;

typedef struct {
    char *name;
    char *path;
    GPtrArray *days;
    gint64 total_tokens;
    gboolean cost_known;
    double total_cost_usd;
} CodexBarCostProject;

typedef struct {
    char *id;
    char *display_name;
    GPtrArray *raw_aliases;
    GPtrArray *session_ids;
    GPtrArray *days;
    gint64 input_tokens;
    gint64 cached_input_tokens;
    gint64 output_tokens;
    gint64 reasoning_tokens;
    gboolean has_reasoning_tokens;
    gint64 total_tokens;
    gint64 priced_tokens;
    gint64 unpriced_tokens;
    double known_cost_usd;
    gint64 previous_total_tokens;
    gint64 previous_priced_tokens;
    gint64 previous_unpriced_tokens;
    double previous_known_cost_usd;
    guint previous_session_references;
    GHashTable *previous_session_ids;
} CodexBarCostModel;

typedef struct {
    char *provider;
    int history_days;
    char *today;
    GPtrArray *days;
    GPtrArray *projects;
    GPtrArray *models;
    gint64 total_tokens;
    gboolean cost_known;
    double total_cost_usd;
    guint skipped_fork_files;
} CodexBarCostReport;

CodexBarCostReport *codexbar_cost_scan(const char *provider, int history_days, GError **error);
void codexbar_cost_report_free(CodexBarCostReport *report);
