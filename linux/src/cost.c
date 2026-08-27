#define _POSIX_C_SOURCE 200809L

#include "cost.h"

#include <errno.h>
#include <json-c/json.h>
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

enum {
    MAX_JSONL_LINE_BYTES = 512 * 1024,
    MAX_JSONL_FILE_BYTES = 512 * 1024 * 1024,
    MAX_DIRECTORY_DEPTH = 64,
    MAX_JSONL_FILES = 4096,
    MAX_TRACE_ROWS = 4096,
    MAX_CSV_COLUMNS = 32,
};

typedef struct {
    const char *name;
    double input;
    double cached;
    double output;
    double cache_write;
} Pricing;

typedef struct {
    gint64 input;
    gint64 cached;
    gint64 cache_create;
    gint64 output;
    gint64 reasoning;
} TokenTotals;

typedef struct {
    char *day;
    char *model;
    char *path;
    char *key;
    gboolean sidechain;
    gboolean subagent_path;
    gint64 input;
    gint64 cached;
    gint64 cache_create;
    gint64 cache_create_1h;
    gint64 output;
    gint64 reasoning;
} ClaudeRow;

typedef struct {
    CodexBarCostReport *report;
    const char *since;
    const char *current_since;
    GHashTable *keyed_rows;
    GPtrArray *unkeyed_rows;
} ClaudeScan;

typedef struct {
    CodexBarCostReport *report;
    const char *since;
    const char *current_since;
    GHashTable *baselines;
    GHashTable *priority_turns;
} CodexScan;

typedef struct {
    char *session_id;
    char *parent_id;
    gboolean subagent;
    gboolean compact_candidate;
    TokenTotals compact_prefix;
    gboolean has_totals;
    TokenTotals totals;
    GArray *totals_history;
    guint line_index;
    guint boundary_line;
    guint pending_turn_line;
    gboolean saw_turn;
    gboolean has_last_total;
    TokenTotals last_total;
} CodexBaseline;

typedef struct {
    char *thread_id;
    char *model;
} PriorityTurn;

typedef struct {
    CodexBarCostReport *report;
    const char *since;
    const char *current_since;
    GHashTable *seen_entries;
} PiScan;

typedef struct {
    CodexBarCostReport *report;
    const char *since;
    const char *current_since;
    char *session_id;
    char *model;
    GHashTable *response_ids;
} AntigravityScan;

typedef gboolean (*JsonlFileCallback)(const char *path, gpointer user_data, GError **error);

static gint compare_string(gconstpointer left, gconstpointer right);

static const Pricing codex_pricing[] = {
    {"gpt-5", 1.25e-6, 1.25e-7, 10e-6, 1.25e-6},
    {"gpt-5-codex", 1.25e-6, 1.25e-7, 10e-6, 1.25e-6},
    {"gpt-5-mini", 0.25e-6, 0.025e-6, 2e-6, 0.25e-6},
    {"gpt-5-nano", 0.05e-6, 0.005e-6, 0.4e-6, 0.05e-6},
    {"gpt-5-pro", 15e-6, 15e-6, 120e-6, 15e-6},
    {"gpt-5.1", 1.25e-6, 0.125e-6, 10e-6, 1.25e-6},
    {"gpt-5.1-codex", 1.25e-6, 0.125e-6, 10e-6, 1.25e-6},
    {"gpt-5.1-codex-max", 1.25e-6, 0.125e-6, 10e-6, 1.25e-6},
    {"gpt-5.1-codex-mini", 0.25e-6, 0.025e-6, 2e-6, 0.25e-6},
    {"gpt-5.2", 1.75e-6, 0.175e-6, 14e-6, 1.75e-6},
    {"gpt-5.2-codex", 1.75e-6, 0.175e-6, 14e-6, 1.75e-6},
    {"gpt-5.2-pro", 21e-6, 21e-6, 168e-6, 21e-6},
    {"gpt-5.3-codex", 1.75e-6, 0.175e-6, 14e-6, 1.75e-6},
    {"gpt-5.3-codex-spark", 0, 0, 0, 0},
    {"gpt-5.4", 2.5e-6, 0.25e-6, 15e-6, 2.5e-6},
    {"gpt-5.4-mini", 0.75e-6, 0.075e-6, 4.5e-6, 0.75e-6},
    {"gpt-5.4-nano", 0.2e-6, 0.02e-6, 1.25e-6, 0.2e-6},
    {"gpt-5.4-pro", 30e-6, 30e-6, 180e-6, 30e-6},
    {"gpt-5.5", 5e-6, 0.5e-6, 30e-6, 5e-6},
    {"gpt-5.5-pro", 30e-6, 30e-6, 180e-6, 30e-6},
    {"gpt-5.6-sol", 5e-6, 0.5e-6, 30e-6, 6.25e-6},
    {"gpt-5.6-terra", 2e-6, 0.2e-6, 12e-6, 2.5e-6},
    {"gpt-5.6-luna", 0.2e-6, 0.02e-6, 1.2e-6, 0.25e-6},
};

static const Pricing claude_pricing[] = {
    {"claude-fable-5", 10e-6, 1e-6, 50e-6, 12.5e-6},
    {"claude-mythos-5", 10e-6, 1e-6, 50e-6, 12.5e-6},
    {"claude-opus-4-8", 5e-6, 0.5e-6, 25e-6, 6.25e-6},
    {"claude-opus-4-7", 5e-6, 0.5e-6, 25e-6, 6.25e-6},
    {"claude-opus-4-6", 5e-6, 0.5e-6, 25e-6, 6.25e-6},
    {"claude-opus-4-5", 5e-6, 0.5e-6, 25e-6, 6.25e-6},
    {"claude-sonnet-5", 2e-6, 0.2e-6, 10e-6, 2.5e-6},
    {"claude-sonnet-4-6", 3e-6, 0.3e-6, 15e-6, 3.75e-6},
    {"claude-sonnet-4-5", 3e-6, 0.3e-6, 15e-6, 3.75e-6},
    {"claude-sonnet-4", 3e-6, 0.3e-6, 15e-6, 3.75e-6},
    {"claude-haiku-4-5", 1e-6, 0.1e-6, 5e-6, 1.25e-6},
    {"claude-opus-4-1", 15e-6, 1.5e-6, 75e-6, 18.75e-6},
    {"claude-opus-4", 15e-6, 1.5e-6, 75e-6, 18.75e-6},
    {"claude-sonnet-3-7", 3e-6, 0.3e-6, 15e-6, 3.75e-6},
    {"claude-sonnet-3-5", 3e-6, 0.3e-6, 15e-6, 3.75e-6},
    {"claude-haiku-3-5", 0.8e-6, 0.08e-6, 4e-6, 1e-6},
    {"claude-opus-3", 15e-6, 1.5e-6, 75e-6, 18.75e-6},
    {"claude-haiku-3", 0.25e-6, 0.03e-6, 1.25e-6, 0.3e-6},
};

static void cost_day_free(gpointer data) {
    CodexBarCostDay *day = data;
    if (!day) return;
    g_free(day->date);
    g_free(day);
}

static void cost_project_free(gpointer data) {
    CodexBarCostProject *project = data;
    if (!project) return;
    g_free(project->name);
    g_free(project->path);
    g_ptr_array_unref(project->days);
    g_free(project);
}

static void cost_model_free(gpointer data) {
    CodexBarCostModel *model = data;
    if (!model) return;
    g_free(model->id);
    g_free(model->display_name);
    g_ptr_array_unref(model->raw_aliases);
    g_ptr_array_unref(model->session_ids);
    g_ptr_array_unref(model->days);
    g_hash_table_unref(model->previous_session_ids);
    g_free(model);
}

static void claude_row_free(gpointer data) {
    ClaudeRow *row = data;
    if (!row) return;
    g_free(row->day);
    g_free(row->model);
    g_free(row->path);
    g_free(row->key);
    g_free(row);
}

static void codex_baseline_free(gpointer data) {
    CodexBaseline *baseline = data;
    if (!baseline) return;
    g_free(baseline->session_id);
    g_free(baseline->parent_id);
    g_array_unref(baseline->totals_history);
    g_free(baseline);
}

static void priority_turn_free(gpointer data) {
    PriorityTurn *turn = data;
    if (!turn) return;
    g_free(turn->thread_id);
    g_free(turn->model);
    g_free(turn);
}

void codexbar_cost_report_free(CodexBarCostReport *report) {
    if (!report) return;
    g_free(report->provider);
    g_free(report->today);
    g_ptr_array_unref(report->days);
    g_ptr_array_unref(report->projects);
    g_ptr_array_unref(report->models);
    g_free(report);
}

static CodexBarCostDay *find_day(GPtrArray *days, const char *date) {
    for (guint index = 0; index < days->len; index++) {
        CodexBarCostDay *day = g_ptr_array_index(days, index);
        if (g_str_equal(day->date, date)) return day;
    }
    CodexBarCostDay *day = g_new0(CodexBarCostDay, 1);
    day->date = g_strdup(date);
    day->cost_known = TRUE;
    g_ptr_array_add(days, day);
    return day;
}

static CodexBarCostProject *find_project(CodexBarCostReport *report, const char *path) {
    const char *key = path ? path : "";
    for (guint index = 0; index < report->projects->len; index++) {
        CodexBarCostProject *project = g_ptr_array_index(report->projects, index);
        if (g_strcmp0(project->path, key) == 0) return project;
    }
    CodexBarCostProject *project = g_new0(CodexBarCostProject, 1);
    project->path = g_strdup(key);
    project->name = key[0] == '\0' ? g_strdup("Unknown project") : g_path_get_basename(key);
    project->days = g_ptr_array_new_with_free_func(cost_day_free);
    project->cost_known = TRUE;
    g_ptr_array_add(report->projects, project);
    return project;
}

static void add_usage(GPtrArray *days,
                      const char *date,
                      gint64 input,
                      gint64 cached,
                      gint64 cache_create,
                      gint64 output,
                      gboolean cached_is_input_subset,
                      gboolean cost_known,
                      double cost) {
    CodexBarCostDay *day = find_day(days, date);
    day->input_tokens += input;
    day->cache_read_tokens += cached;
    day->cache_creation_tokens += cache_create;
    day->output_tokens += output;
    day->total_tokens += input + cache_create + output + (cached_is_input_subset ? 0 : cached);
    if (cost_known) {
        day->cost_usd += cost;
    } else {
        day->cost_known = FALSE;
    }
}

static gint compare_day(gconstpointer left, gconstpointer right) {
    const CodexBarCostDay *lhs = *(CodexBarCostDay *const *)left;
    const CodexBarCostDay *rhs = *(CodexBarCostDay *const *)right;
    return g_strcmp0(lhs->date, rhs->date);
}

static gint compare_project(gconstpointer left, gconstpointer right) {
    const CodexBarCostProject *lhs = *(CodexBarCostProject *const *)left;
    const CodexBarCostProject *rhs = *(CodexBarCostProject *const *)right;
    if (lhs->cost_known && rhs->cost_known && lhs->total_cost_usd != rhs->total_cost_usd) {
        return lhs->total_cost_usd > rhs->total_cost_usd ? -1 : 1;
    }
    if (lhs->total_tokens != rhs->total_tokens) return lhs->total_tokens > rhs->total_tokens ? -1 : 1;
    return g_strcmp0(lhs->name, rhs->name);
}

static gint compare_model(gconstpointer left, gconstpointer right) {
    const CodexBarCostModel *lhs = *(CodexBarCostModel *const *)left;
    const CodexBarCostModel *rhs = *(CodexBarCostModel *const *)right;
    if (lhs->total_tokens != rhs->total_tokens) return lhs->total_tokens > rhs->total_tokens ? -1 : 1;
    return g_strcmp0(lhs->id, rhs->id);
}

static void finalize_days(GPtrArray *days, gint64 *tokens, gboolean *cost_known, double *cost) {
    *tokens = 0;
    *cost = 0;
    *cost_known = days->len > 0;
    g_ptr_array_sort(days, compare_day);
    for (guint index = 0; index < days->len; index++) {
        const CodexBarCostDay *day = g_ptr_array_index(days, index);
        *tokens += day->total_tokens;
        if (day->cost_known) {
            *cost += day->cost_usd;
        } else {
            *cost_known = FALSE;
        }
    }
}

static void finalize_report(CodexBarCostReport *report) {
    finalize_days(report->days, &report->total_tokens, &report->cost_known, &report->total_cost_usd);
    for (guint index = 0; index < report->projects->len; index++) {
        CodexBarCostProject *project = g_ptr_array_index(report->projects, index);
        finalize_days(project->days, &project->total_tokens, &project->cost_known, &project->total_cost_usd);
    }
    g_ptr_array_sort(report->projects, compare_project);
    for (guint index = 0; index < report->models->len; index++) {
        CodexBarCostModel *model = g_ptr_array_index(report->models, index);
        gint64 ignored_tokens = 0;
        gboolean ignored_known = FALSE;
        double ignored_cost = 0;
        finalize_days(model->days, &ignored_tokens, &ignored_known, &ignored_cost);
        g_ptr_array_sort(model->raw_aliases, compare_string);
        g_ptr_array_sort(model->session_ids, compare_string);
        model->previous_session_references = g_hash_table_size(model->previous_session_ids);
    }
    g_ptr_array_sort(report->models, compare_model);
}

static json_object *object_field(json_object *object, const char *name) {
    json_object *value = NULL;
    return object && json_object_object_get_ex(object, name, &value) ? value : NULL;
}

static const char *string_field(json_object *object, const char *name) {
    json_object *value = object_field(object, name);
    return value && json_object_is_type(value, json_type_string) ? json_object_get_string(value) : NULL;
}

static gint64 integer_field(json_object *object, const char *name) {
    json_object *value = object_field(object, name);
    if (!value || (!json_object_is_type(value, json_type_int) && !json_object_is_type(value, json_type_double))) {
        return 0;
    }
    return MAX((gint64)0, json_object_get_int64(value));
}

static gboolean boolean_field(json_object *object, const char *name) {
    json_object *value = object_field(object, name);
    return value && json_object_get_boolean(value);
}

static char *timestamp_day(const char *timestamp) {
    if (!timestamp) return NULL;
    GDateTime *parsed = g_date_time_new_from_iso8601(timestamp, NULL);
    if (!parsed) return NULL;
    GDateTime *local = g_date_time_to_local(parsed);
    char *day = g_date_time_format(local, "%Y-%m-%d");
    g_date_time_unref(local);
    g_date_time_unref(parsed);
    return day;
}

static GDateTime *cost_now(void) {
    const char *override = g_getenv("CODEXBAR_COST_NOW");
    if (override && override[0] != '\0') {
        GDateTime *parsed = g_date_time_new_from_iso8601(override, NULL);
        if (parsed) return parsed;
    }
    return g_date_time_new_now_local();
}

static gboolean day_in_range(const char *day, const char *since, const char *today) {
    return day && g_strcmp0(day, since) >= 0 && g_strcmp0(day, today) <= 0;
}

static const Pricing *find_pricing(const Pricing *table, guint count, const char *model) {
    for (guint index = 0; index < count; index++) {
        if (g_str_equal(table[index].name, model)) return &table[index];
    }
    return NULL;
}

static char *normalize_codex_model(const char *raw) {
    if (!raw || raw[0] == '\0') return g_strdup("unknown");
    char *clean = g_ascii_strdown(raw, -1);
    g_strstrip(clean);
    char *model = g_str_has_prefix(clean, "openai/") ? clean + strlen("openai/") : clean;
    if (g_str_equal(model, "gpt-5.6")) {
        g_free(clean);
        return g_strdup("gpt-5.6-sol");
    }
    if (find_pricing(codex_pricing, G_N_ELEMENTS(codex_pricing), model)) {
        char *result = g_strdup(model);
        g_free(clean);
        return result;
    }
    gsize length = strlen(model);
    if (length > 11 && model[length - 11] == '-' && model[length - 8] == '-' && model[length - 5] == '-') {
        char *base = g_strndup(model, length - 11);
        if (find_pricing(codex_pricing, G_N_ELEMENTS(codex_pricing), base)) {
            g_free(clean);
            return base;
        }
        g_free(base);
    }
    char *result = g_strdup(model);
    g_free(clean);
    return result;
}

static double codex_fast_multiplier(const char *model) {
    char *normalized = normalize_codex_model(model);
    double multiplier = 0;
    if (g_str_equal(normalized, "gpt-5.4") || g_str_equal(normalized, "gpt-5.4-mini") ||
        g_str_equal(normalized, "gpt-5.6-sol") || g_str_equal(normalized, "gpt-5.6-terra") ||
        g_str_equal(normalized, "gpt-5.6-luna")) {
        multiplier = 2;
    } else if (g_str_equal(normalized, "gpt-5.5")) {
        multiplier = 2.5;
    }
    g_free(normalized);
    return multiplier;
}

static char *normalize_claude_model(const char *raw) {
    if (!raw || raw[0] == '\0') return g_strdup("unknown");
    char *clean = g_ascii_strdown(raw, -1);
    g_strstrip(clean);
    const char *model = clean;
    const char *embedded = g_strrstr(clean, ".claude-");
    if (embedded) model = embedded + 1;
    char *normalized = g_strdup(model);
    g_free(clean);
    char *version = g_strrstr(normalized, "-v");
    if (version && strchr(version, ':')) *version = '\0';
    char *at = strrchr(normalized, '@');
    if (at) *at = '\0';
    if (find_pricing(claude_pricing, G_N_ELEMENTS(claude_pricing), normalized)) return normalized;
    gsize length = strlen(normalized);
    if (length > 9 && normalized[length - 9] == '-') {
        char *base = g_strndup(normalized, length - 9);
        if (find_pricing(claude_pricing, G_N_ELEMENTS(claude_pricing), base)) {
            g_free(normalized);
            return base;
        }
        g_free(base);
    }
    return normalized;
}

static gint compare_string(gconstpointer left, gconstpointer right) {
    const char *lhs = *(char *const *)left;
    const char *rhs = *(char *const *)right;
    return g_strcmp0(lhs, rhs);
}

static CodexBarCostModel *find_model(CodexBarCostReport *report, const char *raw_model) {
    char *id = NULL;
    if (g_str_equal(report->provider, "codex")) {
        id = normalize_codex_model(raw_model);
    } else if (g_str_equal(report->provider, "claude")) {
        id = normalize_claude_model(raw_model);
    } else {
        id = g_ascii_strdown(raw_model && raw_model[0] != '\0' ? raw_model : "unknown", -1);
        g_strstrip(id);
    }
    for (guint index = 0; index < report->models->len; index++) {
        CodexBarCostModel *model = g_ptr_array_index(report->models, index);
        if (g_str_equal(model->id, id)) {
            g_free(id);
            return model;
        }
    }
    CodexBarCostModel *model = g_new0(CodexBarCostModel, 1);
    model->id = id;
    model->display_name = g_strdup(id);
    model->raw_aliases = g_ptr_array_new_with_free_func(g_free);
    model->session_ids = g_ptr_array_new_with_free_func(g_free);
    model->days = g_ptr_array_new_with_free_func(cost_day_free);
    model->previous_session_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    g_ptr_array_add(report->models, model);
    return model;
}

static void add_unique_string(GPtrArray *values, const char *value) {
    if (!value || value[0] == '\0') return;
    for (guint index = 0; index < values->len; index++) {
        if (g_str_equal(g_ptr_array_index(values, index), value)) return;
    }
    g_ptr_array_add(values, g_strdup(value));
}

static void add_model_usage(CodexBarCostReport *report,
                            const char *day,
                            const char *raw_model,
                            const char *session_id,
                            TokenTotals usage,
                            gboolean priced,
                            double cost,
                            gboolean priority,
                            gboolean current) {
    CodexBarCostModel *model = find_model(report, raw_model);
    add_unique_string(model->raw_aliases, raw_model ? raw_model : "unknown");
    gboolean claude = g_str_equal(report->provider, "claude");
    gint64 effective_input = usage.input + (claude ? usage.cache_create : 0);
    gint64 tokens = effective_input + usage.output + (claude ? usage.cached : 0);
    if (current) {
        add_unique_string(model->session_ids, session_id);
        model->input_tokens += effective_input;
        model->cached_input_tokens += MIN(usage.cached, usage.input);
        model->output_tokens += usage.output;
        model->reasoning_tokens += MIN(usage.reasoning, usage.output);
        model->has_reasoning_tokens = model->has_reasoning_tokens || usage.reasoning > 0;
        model->total_tokens += tokens;
        if (priced) {
            model->known_cost_usd += cost;
            model->priced_tokens += tokens;
            if (priority) {
                model->priority_tokens += tokens;
                model->priority_cost_usd += cost;
            } else {
                model->standard_tokens += tokens;
                model->standard_cost_usd += cost;
            }
        } else {
            model->unpriced_tokens += tokens;
        }
        add_usage(model->days,
                  day,
                  usage.input,
                  usage.cached,
                  usage.cache_create,
                  usage.output,
                  !claude,
                  priced,
                  cost);
    } else {
        if (session_id && session_id[0] != '\0') {
            g_hash_table_add(model->previous_session_ids, g_strdup(session_id));
        }
        model->previous_total_tokens += tokens;
        if (priced) {
            model->previous_known_cost_usd += cost;
            model->previous_priced_tokens += tokens;
        } else {
            model->previous_unpriced_tokens += tokens;
        }
    }
}

static gboolean codex_cost(const char *model,
                           gint64 input,
                           gint64 cached,
                           gint64 output,
                           double *cost) {
    char *normalized = normalize_codex_model(model);
    const Pricing *pricing = find_pricing(codex_pricing, G_N_ELEMENTS(codex_pricing), normalized);
    g_free(normalized);
    if (!pricing) return FALSE;
    cached = MIN(MAX(cached, 0), MAX(input, 0));
    *cost = (double)(input - cached) * pricing->input + (double)cached * pricing->cached +
            (double)MAX(output, 0) * pricing->output;
    return TRUE;
}

static gboolean claude_cost(const char *model,
                            const char *day,
                            gint64 input,
                            gint64 cached,
                            gint64 cache_create,
                            gint64 cache_create_1h,
                            gint64 output,
                            double *cost) {
    char *normalized = normalize_claude_model(model);
    const Pricing *pricing = find_pricing(claude_pricing, G_N_ELEMENTS(claude_pricing), normalized);
    if (pricing && g_str_equal(normalized, "claude-sonnet-5") && g_strcmp0(day, "2026-09-01") >= 0) {
        static const Pricing sonnet5_regular = {"claude-sonnet-5", 3e-6, 0.3e-6, 15e-6, 3.75e-6};
        pricing = &sonnet5_regular;
    }
    g_free(normalized);
    if (!pricing) return FALSE;
    cache_create_1h = MIN(MAX(cache_create_1h, 0), MAX(cache_create, 0));
    gint64 cache_create_5m = MAX(cache_create, 0) - cache_create_1h;
    *cost = (double)MAX(input, 0) * pricing->input + (double)MAX(cached, 0) * pricing->cached +
            (double)cache_create_5m * pricing->cache_write +
            (double)cache_create_1h * pricing->input * 2.0 + (double)MAX(output, 0) * pricing->output;
    return TRUE;
}

static gboolean scan_jsonl_file(const char *path,
                                gboolean (*line_callback)(json_object *, const char *, gpointer),
                                gpointer user_data,
                                GError **error) {
    FILE *file = fopen(path, "r");
    if (!file) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not read %s: %s", path, g_strerror(errno));
        return FALSE;
    }
    char *line = g_malloc(MAX_JSONL_LINE_BYTES + 2);
    while (fgets(line, MAX_JSONL_LINE_BYTES + 2, file)) {
        gsize length = strlen(line);
        gboolean complete = length > 0 && line[length - 1] == '\n';
        if (!complete && !feof(file)) {
            int character = 0;
            while ((character = fgetc(file)) != '\n' && character != EOF) {}
            continue;
        }
        json_tokener *tokener = json_tokener_new();
        json_object *object = json_tokener_parse_ex(tokener, line, (int)length);
        enum json_tokener_error parse_error = json_tokener_get_error(tokener);
        if (parse_error == json_tokener_success && object && json_object_is_type(object, json_type_object)) {
            if (!line_callback(object, path, user_data)) {
                json_object_put(object);
                json_tokener_free(tokener);
                g_free(line);
                fclose(file);
                return FALSE;
            }
        }
        if (object) json_object_put(object);
        json_tokener_free(tokener);
    }
    gboolean ok = !ferror(file);
    if (!ok) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not read %s: %s", path, g_strerror(errno));
    }
    g_free(line);
    fclose(file);
    return ok;
}

static gboolean walk_jsonl(const char *directory,
                            guint depth,
                            guint *file_count,
                           JsonlFileCallback callback,
                           gpointer user_data,
                           GError **error) {
    if (depth > MAX_DIRECTORY_DEPTH) {
        g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Directory tree is too deep: %s", directory);
        return FALSE;
    }
    GError *directory_error = NULL;
    GDir *dir = g_dir_open(directory, 0, &directory_error);
    if (!dir) {
        g_propagate_prefixed_error(error, directory_error, "Could not scan %s: ", directory);
        return FALSE;
    }
    const char *name = NULL;
    while ((name = g_dir_read_name(dir))) {
        char *path = g_build_filename(directory, name, NULL);
        struct stat status;
        if (lstat(path, &status) != 0) {
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not inspect %s: %s", path, g_strerror(errno));
            g_free(path);
            g_dir_close(dir);
            return FALSE;
        }
        gboolean ok = TRUE;
        if (S_ISDIR(status.st_mode)) {
            ok = walk_jsonl(path, depth + 1, file_count, callback, user_data, error);
        } else if (S_ISREG(status.st_mode) && g_str_has_suffix(name, ".jsonl") && status.st_size > 0 &&
                   status.st_size <= MAX_JSONL_FILE_BYTES) {
            if (*file_count >= MAX_JSONL_FILES) {
                g_set_error(error, G_FILE_ERROR, G_FILE_ERROR_FAILED, "Too many JSONL files under %s", directory);
                ok = FALSE;
            } else {
                (*file_count)++;
                ok = callback(path, user_data, error);
            }
        }
        g_free(path);
        if (!ok) {
            g_dir_close(dir);
            return FALSE;
        }
    }
    g_dir_close(dir);
    return TRUE;
}

static gboolean collect_jsonl_path(const char *path, gpointer user_data, GError **error) {
    (void)error;
    g_ptr_array_add(user_data, g_strdup(path));
    return TRUE;
}

static gboolean scan_roots(GPtrArray *roots,
                           JsonlFileCallback callback,
                           gpointer user_data,
                           GError **error) {
    guint file_count = 0;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GHashTable *seen_files = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    for (guint index = 0; index < roots->len; index++) {
        const char *root = g_ptr_array_index(roots, index);
        char *canonical = g_canonicalize_filename(root, NULL);
        if (g_hash_table_contains(seen, canonical)) {
            g_free(canonical);
            continue;
        }
        g_hash_table_add(seen, canonical);
        struct stat status;
        if (lstat(root, &status) != 0) {
            if (errno == ENOENT) continue;
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not inspect %s: %s", root, g_strerror(errno));
            g_hash_table_unref(seen);
            g_hash_table_unref(seen_files);
            return FALSE;
        }
        if (!S_ISDIR(status.st_mode)) continue;
        GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
        if (!walk_jsonl(root, 0, &file_count, collect_jsonl_path, files, error)) {
            g_ptr_array_unref(files);
            g_hash_table_unref(seen);
            g_hash_table_unref(seen_files);
            return FALSE;
        }
        for (guint file_index = 0; file_index < files->len; file_index++) {
            const char *path = g_ptr_array_index(files, file_index);
            char *canonical_file = g_canonicalize_filename(path, NULL);
            if (!g_hash_table_contains(seen_files, canonical_file)) {
                g_hash_table_add(seen_files, canonical_file);
                if (!callback(path, user_data, error)) {
                    g_ptr_array_unref(files);
                    g_hash_table_unref(seen);
                    g_hash_table_unref(seen_files);
                    return FALSE;
                }
            } else {
                g_free(canonical_file);
            }
        }
        g_ptr_array_unref(files);
    }
    g_hash_table_unref(seen);
    g_hash_table_unref(seen_files);
    return TRUE;
}

static GPtrArray *codex_roots(void) {
    GPtrArray *roots = g_ptr_array_new_with_free_func(g_free);
    const char *override = g_getenv("CODEXBAR_COST_CODEX_ROOT");
    if (override && override[0] != '\0') {
        g_ptr_array_add(roots, g_strdup(override));
        return roots;
    }
    const char *codex_home = g_getenv("CODEX_HOME");
    if (codex_home && codex_home[0] != '\0') {
        g_ptr_array_add(roots, g_build_filename(codex_home, "sessions", NULL));
    } else {
        g_ptr_array_add(roots, g_build_filename(g_get_home_dir(), ".codex", "sessions", NULL));
    }
    return roots;
}

static void add_claude_root(GPtrArray *roots, const char *raw) {
    char *trimmed = g_strstrip(g_strdup(raw));
    if (trimmed[0] == '\0') {
        g_free(trimmed);
        return;
    }
    char *root = g_str_has_suffix(trimmed, "projects") ? g_strdup(trimmed) : g_build_filename(trimmed, "projects", NULL);
    for (guint index = 0; index < roots->len; index++) {
        if (g_str_equal(g_ptr_array_index(roots, index), root)) {
            g_free(root);
            g_free(trimmed);
            return;
        }
    }
    g_ptr_array_add(roots, root);
    g_free(trimmed);
}

static GPtrArray *claude_roots(void) {
    GPtrArray *roots = g_ptr_array_new_with_free_func(g_free);
    const char *override = g_getenv("CODEXBAR_COST_CLAUDE_ROOT");
    if (override && override[0] != '\0') {
        add_claude_root(roots, override);
        return roots;
    }
    const char *configured = g_getenv("CLAUDE_CONFIG_DIR");
    if (configured && configured[0] != '\0') {
        char **parts = g_strsplit(configured, ",", -1);
        for (guint index = 0; parts[index]; index++) add_claude_root(roots, parts[index]);
        g_strfreev(parts);
    } else {
        const char *config_home = g_get_user_config_dir();
        g_ptr_array_add(roots, g_build_filename(config_home, "claude", "projects", NULL));
        g_ptr_array_add(roots, g_build_filename(g_get_home_dir(), ".claude", "projects", NULL));
    }
    return roots;
}

static GPtrArray *pi_roots(void) {
    GPtrArray *roots = g_ptr_array_new_with_free_func(g_free);
    const char *pi = g_getenv("CODEXBAR_COST_PI_ROOT");
    const char *omp = g_getenv("CODEXBAR_COST_OMP_ROOT");
    if ((pi && pi[0] != '\0') || (omp && omp[0] != '\0')) {
        if (pi && pi[0] != '\0') g_ptr_array_add(roots, g_strdup(pi));
        if (omp && omp[0] != '\0') g_ptr_array_add(roots, g_strdup(omp));
        return roots;
    }
    g_ptr_array_add(roots, g_build_filename(g_get_home_dir(), ".pi", "agent", "sessions", NULL));
    g_ptr_array_add(roots, g_build_filename(g_get_home_dir(), ".omp", "agent", "sessions", NULL));
    return roots;
}

static TokenTotals token_totals(json_object *usage) {
    gint64 cached = integer_field(usage, "cached_input_tokens");
    if (cached == 0) cached = integer_field(usage, "cache_read_input_tokens");
    return (TokenTotals){
        .input = integer_field(usage, "input_tokens"),
        .cached = cached,
        .cache_create = integer_field(usage, "cache_creation_input_tokens"),
        .output = integer_field(usage, "output_tokens"),
        .reasoning = integer_field(usage, "reasoning_output_tokens"),
    };
}

static gboolean totals_equal(TokenTotals left, TokenTotals right) {
    return left.input == right.input && left.cached == right.cached && left.output == right.output &&
           left.cache_create == right.cache_create && left.reasoning == right.reasoning;
}

static gboolean prefix_totals_equal(TokenTotals left, TokenTotals right) {
    return left.input == right.input && left.cached == right.cached && left.output == right.output &&
        left.cache_create == right.cache_create;
}

static TokenTotals totals_delta(TokenTotals current, TokenTotals baseline) {
    return (TokenTotals){
        .input = MAX((gint64)0, current.input - baseline.input),
        .cached = MAX((gint64)0, current.cached - baseline.cached),
        .cache_create = MAX((gint64)0, current.cache_create - baseline.cache_create),
        .output = MAX((gint64)0, current.output - baseline.output),
        .reasoning = MAX((gint64)0, current.reasoning - baseline.reasoning),
    };
}

static TokenTotals totals_min(TokenTotals left, TokenTotals right) {
    return (TokenTotals){MIN(left.input, right.input),
                         MIN(left.cached, right.cached),
                         MIN(left.cache_create, right.cache_create),
                         MIN(left.output, right.output),
                         MIN(left.reasoning, right.reasoning)};
}

static TokenTotals totals_max(TokenTotals left, TokenTotals right) {
    return (TokenTotals){MAX(left.input, right.input),
                         MAX(left.cached, right.cached),
                         MAX(left.cache_create, right.cache_create),
                         MAX(left.output, right.output),
                         MAX(left.reasoning, right.reasoning)};
}

static gboolean baseline_line(json_object *object, const char *path, gpointer user_data) {
    (void)path;
    CodexBaseline *baseline = user_data;
    guint line = baseline->line_index++;
    const char *type = string_field(object, "type");
    json_object *payload = object_field(object, "payload");
    if (g_strcmp0(type, "session_meta") == 0) {
        const char *id = string_field(payload, "id");
        if (!id) id = string_field(payload, "session_id");
        if (id && !baseline->session_id) baseline->session_id = g_strdup(id);
        const char *parent = string_field(payload, "forked_from_id");
        if (!parent) parent = string_field(payload, "parent_session_id");
        if (!parent) parent = string_field(payload, "parent_thread_id");
        if (parent && !baseline->parent_id) baseline->parent_id = g_strdup(parent);
        json_object *source = object_field(payload, "source");
        if (source && json_object_is_type(source, json_type_object) && object_field(source, "subagent")) {
            baseline->subagent = TRUE;
        }
        return TRUE;
    }
    if (g_strcmp0(type, "turn_context") == 0) {
        gboolean first = !baseline->saw_turn;
        baseline->saw_turn = TRUE;
        baseline->pending_turn_line = first && baseline->has_last_total ? line : G_MAXUINT;
        return TRUE;
    }
    if (g_strcmp0(type, "inter_agent_communication_metadata") == 0) {
        if (baseline->subagent && boolean_field(payload, "trigger_turn") &&
            baseline->pending_turn_line != G_MAXUINT && line == baseline->pending_turn_line + 1) {
            baseline->compact_candidate = TRUE;
            baseline->compact_prefix = baseline->last_total;
            baseline->boundary_line = baseline->pending_turn_line;
        }
        baseline->pending_turn_line = G_MAXUINT;
        return TRUE;
    }
    if (g_strcmp0(type, "event_msg") != 0 || g_strcmp0(string_field(payload, "type"), "token_count") != 0) {
        return TRUE;
    }
    json_object *info = object_field(payload, "info");
    json_object *total_object = object_field(info, "total_token_usage");
    if (total_object && json_object_is_type(total_object, json_type_object)) {
        baseline->last_total = token_totals(total_object);
        baseline->totals = baseline->last_total;
        g_array_append_val(baseline->totals_history, baseline->last_total);
        baseline->has_last_total = TRUE;
        baseline->has_totals = TRUE;
    }
    return TRUE;
}

static gboolean collect_codex_baseline(const char *path, gpointer user_data, GError **error) {
    GHashTable *baselines = user_data;
    CodexBaseline *baseline = g_new0(CodexBaseline, 1);
    baseline->totals_history = g_array_new(FALSE, FALSE, sizeof(TokenTotals));
    baseline->pending_turn_line = G_MAXUINT;
    if (!scan_jsonl_file(path, baseline_line, baseline, error)) {
        codex_baseline_free(baseline);
        return FALSE;
    }
    if (!baseline->session_id) {
        codex_baseline_free(baseline);
        return TRUE;
    }
    CodexBaseline *existing = g_hash_table_lookup(baselines, baseline->session_id);
    if (!existing || (baseline->has_totals && !existing->has_totals)) {
        g_hash_table_replace(baselines, g_strdup(baseline->session_id), baseline);
    } else {
        codex_baseline_free(baseline);
    }
    return TRUE;
}

static char *trace_value(const char *body, const char *name) {
    char *needle = g_strdup_printf("%s=", name);
    const char *start = strstr(body, needle);
    g_free(needle);
    if (!start) return NULL;
    start = strchr(start, '=') + 1;
    const char *end = start;
    while (*end && !g_ascii_isspace(*end) && !strchr(",])}:", *end)) end++;
    return end > start ? g_strndup(start, (gsize)(end - start)) : NULL;
}

static char *trace_quoted_value(const char *body, const char *name) {
    char *needle = g_strdup_printf("%s: \"", name);
    const char *start = strstr(body, needle);
    if (start) start += strlen(needle);
    g_free(needle);
    if (!start) return NULL;
    const char *end = strchr(start, '\"');
    return end && end > start ? g_strndup(start, (gsize)(end - start)) : NULL;
}

static void load_priority_turns(GHashTable *turns, const char *since) {
    const char *override = g_getenv("CODEXBAR_COST_CODEX_TRACE_DB");
    char *path = override && override[0] != '\0'
        ? g_strdup(override)
        : g_build_filename(g_getenv("CODEX_HOME") ? g_getenv("CODEX_HOME") : g_get_home_dir(),
                           g_getenv("CODEX_HOME") ? "logs_2.sqlite" : ".codex/logs_2.sqlite",
                           NULL);
    sqlite3 *database = NULL;
    if (sqlite3_open_v2(path, &database, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        sqlite3_close(database);
        g_free(path);
        return;
    }
    sqlite3_busy_timeout(database, 100);
    GDateTime *since_date = g_date_time_new_from_iso8601(since, NULL);
    gint64 since_epoch = since_date ? g_date_time_to_unix(since_date) : 0;
    if (since_date) g_date_time_unref(since_date);
    const char *query = "select feedback_log_body from logs where ts >= ?1 and feedback_log_body like ?2 "
                        "order by rowid limit ?3";
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, query, -1, &statement, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(statement, 1, since_epoch);
        sqlite3_bind_text(statement, 2, "%service_tier: Some(Some(\"priority\"))%", -1, SQLITE_STATIC);
        sqlite3_bind_int(statement, 3, MAX_TRACE_ROWS);
        while (sqlite3_step(statement) == SQLITE_ROW) {
            const char *body = (const char *)sqlite3_column_text(statement, 0);
            int body_length = sqlite3_column_bytes(statement, 0);
            if (!body || body_length <= 0 || body_length > 1024 * 1024 ||
                !strstr(body, "Submission sub=Submission {")) {
                continue;
            }
            const char *submission = strstr(body, "Submission sub=Submission {");
            char *turn_id = trace_quoted_value(submission, "id");
            if (!turn_id) continue;
            PriorityTurn *turn = g_new0(PriorityTurn, 1);
            turn->thread_id = trace_value(body, "thread_id");
            g_hash_table_replace(turns, turn_id, turn);
        }
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    g_free(path);
}

static gboolean baseline_contains_totals(const CodexBaseline *baseline, TokenTotals totals) {
    if (!baseline) return FALSE;
    for (guint index = 0; index < baseline->totals_history->len; index++) {
        TokenTotals candidate = g_array_index(baseline->totals_history, TokenTotals, index);
        if (prefix_totals_equal(candidate, totals)) return TRUE;
    }
    return FALSE;
}

typedef struct {
    CodexScan *scan;
    char *model;
    char *project;
    char *session_id;
    gboolean forked;
    gboolean independent_fork;
    gboolean has_counted;
    TokenTotals counted;
    gboolean has_baseline;
    TokenTotals baseline;
    gboolean has_watermark;
    TokenTotals watermark;
    gboolean divergent;
    gboolean interleaved;
    gboolean compact_confirmed;
    guint compact_boundary_line;
    guint line_index;
    TokenTotals compact_prefix;
    char *turn_id;
    GArray *seen;
} CodexFile;

static gboolean seen_totals(CodexFile *file, TokenTotals totals) {
    for (guint index = 0; index < file->seen->len; index++) {
        TokenTotals seen = g_array_index(file->seen, TokenTotals, index);
        if (totals_equal(seen, totals)) return TRUE;
    }
    return FALSE;
}

static void remember_totals(CodexFile *file, TokenTotals totals) {
    g_array_append_val(file->seen, totals);
    if (file->seen->len > 64) g_array_remove_index(file->seen, 0);
    file->watermark = file->has_watermark ? totals_max(file->watermark, totals) : totals;
    file->has_watermark = TRUE;
}

static gboolean codex_line(json_object *object, const char *path, gpointer user_data) {
    CodexFile *file = user_data;
    guint line = file->line_index++;
    const char *type = string_field(object, "type");
    json_object *payload = object_field(object, "payload");
    if (g_strcmp0(type, "session_meta") == 0) {
        const char *project = string_field(payload, "cwd");
        if (project && project[0] == '/') {
            g_free(file->project);
            file->project = g_canonicalize_filename(project, NULL);
        }
        const char *session_id = string_field(payload, "id");
        if (session_id) {
            g_free(file->session_id);
            file->session_id = g_strdup(session_id);
            CodexBaseline *shape = g_hash_table_lookup(file->scan->baselines, session_id);
            CodexBaseline *parent = shape && shape->parent_id
                ? g_hash_table_lookup(file->scan->baselines, shape->parent_id)
                : NULL;
            if (shape && parent && shape->compact_candidate &&
                baseline_contains_totals(parent, shape->compact_prefix)) {
                file->compact_confirmed = TRUE;
                file->compact_boundary_line = shape->boundary_line;
                file->compact_prefix = shape->compact_prefix;
            }
        }
        if (string_field(payload, "forked_from_id") || string_field(payload, "forkedFromId") ||
            string_field(payload, "parent_session_id") || string_field(payload, "parentSessionId") ||
            string_field(payload, "parent_thread_id") || string_field(payload, "parentThreadId")) {
            file->forked = TRUE;
        }
        json_object *source_value = object_field(payload, "source");
        const char *source = source_value && json_object_is_type(source_value, json_type_string)
                                 ? json_object_get_string(source_value)
                                 : NULL;
        gboolean subagent = source && g_ascii_strcasecmp(source, "subagent") == 0;
        if (source_value && json_object_is_type(source_value, json_type_object)) {
            json_object *nested = NULL;
            subagent = json_object_object_get_ex(source_value, "subagent", &nested);
        }
        if (subagent) {
            file->forked = TRUE;
            file->independent_fork = TRUE;
        }
        return TRUE;
    }
    if (g_strcmp0(type, "turn_context") == 0) {
        json_object *info = object_field(payload, "info");
        const char *model = string_field(payload, "model");
        if (!model) model = string_field(payload, "model_name");
        if (!model) model = string_field(info, "model");
        if (!model) model = string_field(info, "model_name");
        if (model) {
            g_free(file->model);
            file->model = g_strdup(model);
        }
        if (file->compact_confirmed && line == file->compact_boundary_line) {
            file->baseline = file->compact_prefix;
            file->watermark = file->compact_prefix;
            file->has_baseline = TRUE;
            file->has_watermark = TRUE;
            file->counted = (TokenTotals){0};
            file->has_counted = FALSE;
            file->divergent = FALSE;
        }
        return TRUE;
    }
    if (g_strcmp0(type, "event_msg") == 0 && g_strcmp0(string_field(payload, "type"), "task_started") == 0) {
        const char *turn_id = string_field(payload, "turn_id");
        if (turn_id) {
            g_free(file->turn_id);
            file->turn_id = g_strdup(turn_id);
        }
        return TRUE;
    }
    if (g_strcmp0(type, "event_msg") != 0 || g_strcmp0(string_field(payload, "type"), "token_count") != 0) {
        return TRUE;
    }
    if (file->compact_confirmed && line < file->compact_boundary_line) return TRUE;
    char *day = timestamp_day(string_field(object, "timestamp"));
    if (!day || !day_in_range(day, file->scan->since, file->scan->report->today)) {
        g_free(day);
        return TRUE;
    }
    json_object *info = object_field(payload, "info");
    json_object *last_object = object_field(info, "last_token_usage");
    json_object *total_object = object_field(info, "total_token_usage");
    gboolean has_last = last_object && json_object_is_type(last_object, json_type_object);
    gboolean has_total = total_object && json_object_is_type(total_object, json_type_object);
    if (!has_last && !has_total) {
        g_free(day);
        return TRUE;
    }
    TokenTotals last = has_last ? token_totals(last_object) : (TokenTotals){0};
    TokenTotals total = has_total ? token_totals(total_object) : (TokenTotals){0};
    if (has_total && seen_totals(file, total)) {
        g_free(day);
        return TRUE;
    }
    if (has_total && file->has_watermark &&
        (total.input < file->watermark.input || total.cached < file->watermark.cached ||
         total.cache_create < file->watermark.cache_create || total.output < file->watermark.output ||
         total.reasoning < file->watermark.reasoning)) {
        file->interleaved = TRUE;
    }
    TokenTotals baseline = file->has_watermark ? file->watermark : (file->has_baseline ? file->baseline : (TokenTotals){0});
    if (file->forked && !file->independent_fork && !has_last) {
        if (has_total) {
            remember_totals(file, total);
            file->baseline = total;
            file->has_baseline = TRUE;
        }
        g_free(day);
        return TRUE;
    }
    TokenTotals delta = {0};
    if (has_total && file->interleaved) {
        TokenTotals counted = file->has_counted ? file->counted : (TokenTotals){0};
        TokenTotals floor = totals_max(baseline, counted);
        delta = totals_delta(total, floor);
        if (has_last) delta = totals_min(delta, last);
    } else if (has_last) {
        delta = last;
        if (has_total) {
            TokenTotals from_total = totals_delta(total, baseline);
            if (!file->divergent && total.input >= baseline.input && total.cached >= baseline.cached &&
                total.output >= baseline.output && total.reasoning >= baseline.reasoning &&
                total.cache_create >= baseline.cache_create && from_total.input <= last.input &&
                from_total.cached <= last.cached && from_total.cache_create <= last.cache_create &&
                from_total.output <= last.output && from_total.reasoning <= last.reasoning) {
                delta = from_total;
            }
        }
    } else if (has_total) {
        delta = totals_delta(total, baseline);
    }
    if (has_total) {
        remember_totals(file, total);
        file->baseline = total;
        file->has_baseline = TRUE;
    }
    if (delta.input == 0 && delta.cached == 0 && delta.output == 0) {
        g_free(day);
        return TRUE;
    }
    file->counted.input += delta.input;
    file->counted.cached += delta.cached;
    file->counted.cache_create += delta.cache_create;
    file->counted.output += delta.output;
    file->counted.reasoning += delta.reasoning;
    file->has_counted = TRUE;
    if (has_total && !totals_equal(file->counted, total)) file->divergent = TRUE;

    const char *model = string_field(info, "model");
    if (!model) model = string_field(info, "model_name");
    if (!model) model = string_field(payload, "model");
    if (!model) model = file->model;
    double cost = 0;
    gboolean priced = codex_cost(model, delta.input, delta.cached, delta.output, &cost);
    gboolean priority = file->turn_id && g_hash_table_contains(file->scan->priority_turns, file->turn_id);
    if (priority && priced && delta.input <= 272000) {
        double multiplier = codex_fast_multiplier(model);
        if (multiplier > 0) cost *= multiplier;
    }
    gboolean current = g_strcmp0(day, file->scan->current_since) >= 0;
    add_model_usage(file->scan->report,
                    day,
                    model,
                    file->session_id ? file->session_id : path,
                     delta,
                     priced,
                     cost,
                     priority,
                     current);
    if (current) {
        add_usage(file->scan->report->days, day, delta.input, delta.cached, 0, delta.output, TRUE, priced, cost);
        CodexBarCostProject *project = find_project(file->scan->report, file->project);
        add_usage(project->days, day, delta.input, delta.cached, 0, delta.output, TRUE, priced, cost);
    }
    g_free(day);
    return TRUE;
}

static gboolean scan_codex_file(const char *path, gpointer user_data, GError **error) {
    CodexScan *scan = user_data;
    CodexFile file = {
        .scan = scan,
        .seen = g_array_new(FALSE, FALSE, sizeof(TokenTotals)),
    };
    gboolean result = scan_jsonl_file(path, codex_line, &file, error);
    g_array_unref(file.seen);
    g_free(file.model);
    g_free(file.project);
    g_free(file.session_id);
    g_free(file.turn_id);
    return result;
}

static gboolean claude_candidate_wins(const ClaudeRow *candidate, const ClaudeRow *existing) {
    if (candidate->sidechain != existing->sidechain) return existing->sidechain;
    if (candidate->subagent_path != existing->subagent_path) return existing->subagent_path;
    return g_strcmp0(candidate->path, existing->path) < 0;
}

static gboolean claude_line(json_object *object, const char *path, gpointer user_data) {
    ClaudeScan *scan = user_data;
    if (g_strcmp0(string_field(object, "type"), "assistant") != 0) return TRUE;
    json_object *message = object_field(object, "message");
    json_object *usage = object_field(message, "usage");
    const char *model = string_field(message, "model");
    if (!message || !usage || !model) return TRUE;
    char *day = timestamp_day(string_field(object, "timestamp"));
    if (!day || !day_in_range(day, scan->since, scan->report->today)) {
        g_free(day);
        return TRUE;
    }
    gint64 input = integer_field(usage, "input_tokens");
    gint64 cached = integer_field(usage, "cache_read_input_tokens");
    gint64 cache_create = integer_field(usage, "cache_creation_input_tokens");
    gint64 output = integer_field(usage, "output_tokens");
    if (input == 0 && cached == 0 && cache_create == 0 && output == 0) {
        g_free(day);
        return TRUE;
    }
    gint64 cache_create_1h = 0;
    json_object *cache_detail = object_field(usage, "cache_creation");
    if (cache_detail) cache_create_1h = MIN(cache_create, integer_field(cache_detail, "ephemeral_1h_input_tokens"));
    const char *message_id = string_field(message, "id");
    const char *request_id = string_field(object, "requestId");
    ClaudeRow *row = g_new0(ClaudeRow, 1);
    row->day = day;
    row->model = g_strdup(model);
    row->path = g_strdup(path);
    row->sidechain = boolean_field(object, "isSidechain");
    row->subagent_path = strstr(path, "/subagents/") != NULL;
    row->input = input;
    row->cached = cached;
    row->cache_create = cache_create;
    row->cache_create_1h = cache_create_1h;
    row->output = output;
    row->reasoning = integer_field(usage, "reasoning_output_tokens");
    if (message_id && request_id) {
        row->key = g_strdup_printf("%s:%s", message_id, request_id);
        ClaudeRow *existing = g_hash_table_lookup(scan->keyed_rows, row->key);
        if (!existing || g_str_equal(existing->path, row->path) || claude_candidate_wins(row, existing)) {
            g_hash_table_replace(scan->keyed_rows, g_strdup(row->key), row);
        } else {
            claude_row_free(row);
        }
    } else {
        g_ptr_array_add(scan->unkeyed_rows, row);
    }
    return TRUE;
}

static gboolean scan_claude_file(const char *path, gpointer user_data, GError **error) {
    return scan_jsonl_file(path, claude_line, user_data, error);
}

typedef struct {
    PiScan *scan;
    char *session_id;
    char *model;
    char *provider;
} PiFile;

static const char *pi_string(json_object *entry, json_object *message, const char *name) {
    const char *value = string_field(message, name);
    return value ? value : string_field(entry, name);
}

static gint64 pi_integer(json_object *usage, const char *first, const char *second, const char *third) {
    gint64 value = integer_field(usage, first);
    if (value == 0 && second) value = integer_field(usage, second);
    if (value == 0 && third) value = integer_field(usage, third);
    return value;
}

static gboolean pi_line(json_object *object, const char *path, gpointer user_data) {
    PiFile *file = user_data;
    const char *type = string_field(object, "type");
    if (g_strcmp0(type, "session") == 0) {
        const char *id = string_field(object, "id");
        if (id) {
            g_free(file->session_id);
            file->session_id = g_strdup(id);
        }
        return TRUE;
    }
    if (g_strcmp0(type, "model_change") == 0) {
        const char *provider = string_field(object, "provider");
        const char *model = string_field(object, "modelId");
        if (provider) {
            g_free(file->provider);
            file->provider = g_strdup(provider);
        }
        if (model) {
            g_free(file->model);
            file->model = g_strdup(model);
        }
        return TRUE;
    }
    if (g_strcmp0(type, "message") != 0) return TRUE;
    json_object *message = object_field(object, "message");
    if (!message || g_strcmp0(string_field(message, "role"), "assistant") != 0) return TRUE;
    const char *provider = pi_string(object, message, "provider");
    if (!provider) provider = file->provider;
    const char *model = pi_string(object, message, "model");
    if (!model) model = pi_string(object, message, "modelId");
    if (!model) model = file->model;
    gboolean codex = g_strcmp0(provider, "openai-codex") == 0;
    gboolean claude = g_strcmp0(provider, "anthropic") == 0;
    if ((!codex && !claude) || !model) return TRUE;
    if (!g_str_equal(file->scan->report->provider, codex ? "codex" : "claude")) return TRUE;
    const char *timestamp = string_field(message, "timestamp");
    if (!timestamp) timestamp = string_field(object, "timestamp");
    char *day = timestamp_day(timestamp);
    if (!day || !day_in_range(day, file->scan->since, file->scan->report->today)) {
        g_free(day);
        return TRUE;
    }
    json_object *usage_object = object_field(message, "usage");
    TokenTotals usage = {
        .input = pi_integer(usage_object, "input", "inputTokens", "input_tokens"),
        .cached = pi_integer(usage_object, "cacheRead", "cacheReadTokens", "cache_read_tokens"),
        .cache_create = pi_integer(usage_object, "cacheWrite", "cacheWriteTokens", "cache_creation_tokens"),
        .output = pi_integer(usage_object, "output", "outputTokens", "output_tokens"),
    };
    if (usage.input == 0 && usage.cached == 0 && usage.cache_create == 0 && usage.output == 0) {
        g_free(day);
        return TRUE;
    }
    const char *entry_id = string_field(object, "id");
    char *dedupe = file->session_id && entry_id
        ? g_strdup_printf("%s:%s", file->session_id, entry_id)
        : g_strdup_printf("%s:%s", path, entry_id ? entry_id : timestamp);
    if (g_hash_table_contains(file->scan->seen_entries, dedupe)) {
        g_free(dedupe);
        g_free(day);
        return TRUE;
    }
    g_hash_table_add(file->scan->seen_entries, dedupe);
    double cost = 0;
    gboolean priced = codex
        ? codex_cost(model, usage.input + usage.cached + usage.cache_create, usage.cached, usage.output, &cost)
        : claude_cost(model, day, usage.input, usage.cached, usage.cache_create, 0, usage.output, &cost);
    gboolean current = g_strcmp0(day, file->scan->current_since) >= 0;
    TokenTotals aggregate = usage;
    if (codex) aggregate.input += usage.cached + usage.cache_create;
    add_model_usage(file->scan->report,
                    day,
                    model,
                    file->session_id ? file->session_id : path,
                    aggregate,
                    priced,
                    cost,
                    FALSE,
                    current);
    if (current) {
        add_usage(file->scan->report->days,
                  day,
                  usage.input,
                  usage.cached,
                  usage.cache_create,
                  usage.output,
                  FALSE,
                  priced,
                  cost);
    }
    g_free(day);
    return TRUE;
}

static gboolean scan_pi_file(const char *path, gpointer user_data, GError **error) {
    PiFile file = {.scan = user_data};
    gboolean ok = scan_jsonl_file(path, pi_line, &file, error);
    g_free(file.session_id);
    g_free(file.model);
    g_free(file.provider);
    return ok;
}

static void aggregate_claude_row(gpointer key, gpointer value, gpointer user_data) {
    (void)key;
    ClaudeScan *scan = user_data;
    ClaudeRow *row = value;
    double cost = 0;
    gboolean priced = claude_cost(row->model,
                                  row->day,
                                  row->input,
                                  row->cached,
                                  row->cache_create,
                                  row->cache_create_1h,
                                  row->output,
                                  &cost);
    gboolean current = g_strcmp0(row->day, scan->current_since) >= 0;
    add_model_usage(scan->report,
                    row->day,
                    row->model,
                    row->path,
                     (TokenTotals){row->input, row->cached, row->cache_create, row->output, row->reasoning},
                     priced,
                     cost,
                     FALSE,
                     current);
    if (current) {
        add_usage(scan->report->days,
                  row->day,
                  row->input,
                  row->cached,
                  row->cache_create,
                  row->output,
                  FALSE,
                  priced,
                  cost);
    }
}

static gboolean parse_nonnegative(const char *raw, gint64 *value) {
    char *clean = g_strdup(raw ? raw : "");
    g_strstrip(clean);
    for (char *cursor = clean; *cursor; cursor++) {
        if (*cursor == ',' || *cursor == '$') memmove(cursor, cursor + 1, strlen(cursor));
    }
    if (clean[0] == '\0') {
        g_free(clean);
        *value = 0;
        return TRUE;
    }
    char *end = NULL;
    errno = 0;
    gint64 parsed = g_ascii_strtoll(clean, &end, 10);
    gboolean ok = errno == 0 && end && *end == '\0' && parsed >= 0;
    g_free(clean);
    if (ok) *value = parsed;
    return ok;
}

static GPtrArray *parse_csv_line(const char *line) {
    GPtrArray *columns = g_ptr_array_new_with_free_func(g_free);
    GString *field = g_string_new(NULL);
    gboolean quoted = FALSE;
    for (const char *cursor = line; *cursor; cursor++) {
        if (*cursor == '"') {
            if (quoted && cursor[1] == '"') {
                g_string_append_c(field, '"');
                cursor++;
            } else {
                quoted = !quoted;
            }
        } else if (*cursor == ',' && !quoted) {
            char *value = g_string_free(field, FALSE);
            g_strstrip(value);
            g_ptr_array_add(columns, value);
            if (columns->len > MAX_CSV_COLUMNS) break;
            field = g_string_new(NULL);
        } else if (*cursor != '\r' && *cursor != '\n') {
            g_string_append_c(field, *cursor);
        }
    }
    if (columns->len <= MAX_CSV_COLUMNS) {
        char *value = g_string_free(field, FALSE);
        g_strstrip(value);
        g_ptr_array_add(columns, value);
    } else {
        g_string_free(field, TRUE);
    }
    return columns;
}

static void add_local_usage(CodexBarCostReport *report,
                            const char *day,
                            const char *model_name,
                            const char *session_id,
                            gint64 input,
                            gint64 cache_read,
                            gint64 cache_write,
                            gint64 output,
                            gint64 total,
                            gboolean cost_known,
                            double cost,
                            gboolean current) {
    if (current) {
        CodexBarCostDay *report_day = find_day(report->days, day);
        report_day->input_tokens += input;
        report_day->cache_read_tokens += cache_read;
        report_day->cache_creation_tokens += cache_write;
        report_day->output_tokens += output;
        report_day->total_tokens += total;
        if (cost_known) report_day->cost_usd += cost;
        else report_day->cost_known = FALSE;
    }
    CodexBarCostModel *model = find_model(report, model_name);
    add_unique_string(model->raw_aliases, model_name ? model_name : "unknown");
    if (current) {
        add_unique_string(model->session_ids, session_id);
        model->input_tokens += input;
        model->cached_input_tokens += cache_read;
        model->output_tokens += output;
        model->total_tokens += total;
        if (cost_known) {
            model->known_cost_usd += cost;
            model->priced_tokens += total;
            model->standard_tokens += total;
            model->standard_cost_usd += cost;
        } else {
            model->unpriced_tokens += total;
        }
        CodexBarCostDay *model_day = find_day(model->days, day);
        model_day->input_tokens += input;
        model_day->cache_read_tokens += cache_read;
        model_day->cache_creation_tokens += cache_write;
        model_day->output_tokens += output;
        model_day->total_tokens += total;
        if (cost_known) model_day->cost_usd += cost;
        else model_day->cost_known = FALSE;
    } else {
        if (session_id) g_hash_table_add(model->previous_session_ids, g_strdup(session_id));
        model->previous_total_tokens += total;
        if (cost_known) {
            model->previous_priced_tokens += total;
            model->previous_known_cost_usd += cost;
        } else {
            model->previous_unpriced_tokens += total;
        }
    }
}

static gboolean scan_cursor_csv(CodexBarCostReport *report,
                                const char *root,
                                const char *since,
                                const char *current_since,
                                GError **error) {
    GDir *directory = g_dir_open(root, 0, NULL);
    if (!directory) return TRUE;
    const char *name = NULL;
    guint files = 0;
    while ((name = g_dir_read_name(directory))) {
        if (!g_str_has_prefix(name, "usage") || !g_str_has_suffix(name, ".csv") ||
            g_str_has_prefix(name, "usage.backup") || ++files > MAX_JSONL_FILES) continue;
        char *path = g_build_filename(root, name, NULL);
        struct stat info;
        if (lstat(path, &info) != 0 || !S_ISREG(info.st_mode) || info.st_size > MAX_JSONL_FILE_BYTES) {
            g_free(path);
            continue;
        }
        FILE *stream = fopen(path, "r");
        g_free(path);
        if (!stream) continue;
        char *line = g_malloc(MAX_JSONL_LINE_BYTES + 2);
        if (!fgets(line, MAX_JSONL_LINE_BYTES + 2, stream)) {
            g_free(line);
            fclose(stream);
            continue;
        }
        GPtrArray *header = parse_csv_line(line);
        gboolean has_kind = FALSE;
        for (guint index = 0; index < header->len; index++) {
            if (g_ascii_strcasecmp(g_ptr_array_index(header, index), "kind") == 0) has_kind = TRUE;
        }
        guint model = has_kind ? (header->len >= 11 ? 4 : 2) : 1;
        guint input_with = has_kind ? (header->len >= 11 ? 6 : 4) : 2;
        guint input_without = input_with + 1, cache_read = input_with + 2, output = input_with + 3;
        guint cost_index = input_with + 5;
        while (fgets(line, MAX_JSONL_LINE_BYTES + 2, stream)) {
            GPtrArray *columns = parse_csv_line(line);
            if (columns->len <= cost_index || columns->len <= output || strlen(g_ptr_array_index(columns, 0)) < 10) {
                g_ptr_array_unref(columns);
                continue;
            }
            char *day = g_strndup(g_ptr_array_index(columns, 0), 10);
            gint64 with_cache = 0, input = 0, read = 0, out = 0, total = 0;
            gboolean valid = parse_nonnegative(g_ptr_array_index(columns, input_with), &with_cache) &&
                             parse_nonnegative(g_ptr_array_index(columns, input_without), &input) &&
                             parse_nonnegative(g_ptr_array_index(columns, cache_read), &read) &&
                             parse_nonnegative(g_ptr_array_index(columns, output), &out);
            guint total_index = cost_index - 1;
            char *total_header = total_index < header->len
                                     ? g_ascii_strdown(g_ptr_array_index(header, total_index), -1)
                                     : NULL;
            gboolean has_total = total_header && g_strrstr(total_header, "total") != NULL;
            g_free(total_header);
            if (has_total) valid = valid && parse_nonnegative(g_ptr_array_index(columns, total_index), &total);
            gint64 write = MAX((gint64)0, with_cache - input);
            if (!has_total) total = input + read + write + out;
            char *cost_raw = g_strdup(g_ptr_array_index(columns, cost_index));
            g_strstrip(cost_raw);
            char *cost_clean = g_malloc(strlen(cost_raw) + 1);
            guint cost_length = 0;
            for (const char *cursor = cost_raw; *cursor; cursor++) {
                if (*cursor != '$' && *cursor != ',') cost_clean[cost_length++] = *cursor;
            }
            cost_clean[cost_length] = '\0';
            char *end = NULL;
            double cost = cost_clean[0] == '\0' ? 0 : g_ascii_strtod(cost_clean, &end);
            gboolean cost_valid = cost_clean[0] == '\0' || (end && *end == '\0' && cost >= 0);
            if (valid && cost_valid && total > 0 && day_in_range(day, since, report->today)) {
                add_local_usage(report, day, g_ptr_array_index(columns, model), name, input, read, write, out,
                                total, TRUE, cost, g_strcmp0(day, current_since) >= 0);
            }
            g_free(cost_clean);
            g_free(cost_raw);
            g_free(day);
            g_ptr_array_unref(columns);
        }
        g_ptr_array_unref(header);
        g_free(line);
        fclose(stream);
    }
    g_dir_close(directory);
    (void)error;
    return TRUE;
}

static char *millis_day(gint64 milliseconds) {
    if (milliseconds <= 0 || milliseconds > 253402300799999LL) return NULL;
    GDateTime *utc = g_date_time_new_from_unix_utc(milliseconds / 1000);
    if (!utc) return NULL;
    GDateTime *local = g_date_time_to_local(utc);
    char *day = g_date_time_format(local, "%Y-%m-%d");
    g_date_time_unref(local);
    g_date_time_unref(utc);
    return day;
}

static gboolean exact_json_counter(json_object *object, const char *first, const char *second,
                                   gboolean required, gint64 *value) {
    json_object *left = object_field(object, first);
    json_object *right = second ? object_field(object, second) : NULL;
    if (!left && !right) return !required;
    if ((left && !json_object_is_type(left, json_type_int)) ||
        (right && !json_object_is_type(right, json_type_int))) return FALSE;
    gint64 parsed = left ? json_object_get_int64(left) : json_object_get_int64(right);
    if (parsed < 0 || (left && right && parsed != json_object_get_int64(right))) return FALSE;
    *value = parsed;
    return TRUE;
}

static gboolean antigravity_line(json_object *object, const char *path, gpointer user_data) {
    AntigravityScan *scan = user_data;
    const char *type = string_field(object, "type");
    if (g_strcmp0(type, "session_meta") == 0) {
        const char *id = string_field(object, "sessionId");
        const char *model = string_field(object, "modelId");
        if (!model) model = string_field(object, "model_id");
        if (id && (!scan->session_id || g_str_equal(scan->session_id, id))) {
            g_free(scan->session_id);
            scan->session_id = g_strdup(id);
            g_free(scan->model);
            scan->model = g_strdup(model);
        }
        return TRUE;
    }
    if (g_strcmp0(type, "usage") != 0) return TRUE;
    const char *id = string_field(object, "sessionId");
    if (!id) id = scan->session_id;
    const char *model = string_field(object, "modelId");
    if (!model) model = string_field(object, "model_id");
    if (!model) model = scan->model;
    const char *response = string_field(object, "responseId");
    if (!response) response = string_field(object, "response_id");
    if (response && g_hash_table_contains(scan->response_ids, response)) return TRUE;
    gint64 timestamp = 0, input = 0, output = 0, read = 0, write = 0, reasoning = 0;
    gboolean valid = exact_json_counter(object, "timestamp", NULL, TRUE, &timestamp) &&
                     exact_json_counter(object, "input", NULL, TRUE, &input) &&
                     exact_json_counter(object, "output", NULL, TRUE, &output) &&
                     exact_json_counter(object, "cacheRead", "cache_read", FALSE, &read) &&
                     exact_json_counter(object, "cacheWrite", "cache_write", FALSE, &write) &&
                     exact_json_counter(object, "reasoning", NULL, FALSE, &reasoning);
    char *day = millis_day(timestamp);
    if (valid && id && day && reasoning == 0 && (input > 0 || output > 0 || read > 0 || write > 0) &&
        day_in_range(day, scan->since, scan->report->today)) {
        if (response) g_hash_table_add(scan->response_ids, g_strdup(response));
        add_local_usage(scan->report, day, model, id, input, read, write, output,
                        input + read + write + output, FALSE, 0,
                        g_strcmp0(day, scan->current_since) >= 0);
    }
    g_free(day);
    (void)path;
    return TRUE;
}

static gboolean scan_antigravity_file(const char *path, gpointer user_data, GError **error) {
    AntigravityScan *scan = user_data;
    g_clear_pointer(&scan->session_id, g_free);
    g_clear_pointer(&scan->model, g_free);
    return scan_jsonl_file(path, antigravity_line, scan, error);
}

typedef struct {
    guint number;
    guint wire;
    guint64 integer;
    const guint8 *data;
    gsize length;
} ProtoField;

typedef struct {
    const guint8 *data;
    gsize length;
    gsize offset;
    gboolean valid;
} ProtoReader;

static gboolean proto_varint(ProtoReader *reader, guint64 *value) {
    *value = 0;
    for (guint index = 0; index < 10 && reader->offset < reader->length; index++) {
        guint8 byte = reader->data[reader->offset++];
        if (index == 9 && byte > 1) break;
        *value |= (guint64)(byte & 0x7f) << (index * 7);
        if ((byte & 0x80) == 0) return TRUE;
    }
    reader->valid = FALSE;
    return FALSE;
}

static gboolean proto_field(ProtoReader *reader, ProtoField *field) {
    if (reader->offset >= reader->length) return FALSE;
    guint64 tag = 0;
    if (!proto_varint(reader, &tag) || tag >> 3 == 0 || tag >> 3 > 536870911) return FALSE;
    *field = (ProtoField){.number = (guint)(tag >> 3), .wire = (guint)(tag & 7)};
    if (field->wire == 0) return proto_varint(reader, &field->integer);
    guint64 count = 0;
    if (field->wire == 1) count = 8;
    else if (field->wire == 5) count = 4;
    else if (field->wire != 2 || !proto_varint(reader, &count)) {
        reader->valid = FALSE;
        return FALSE;
    }
    if (count > reader->length - reader->offset) {
        reader->valid = FALSE;
        return FALSE;
    }
    field->data = reader->data + reader->offset;
    field->length = (gsize)count;
    reader->offset += (gsize)count;
    return TRUE;
}

static char *proto_string(const ProtoField *field) {
    if (field->wire != 2 || !g_utf8_validate((const char *)field->data, (gssize)field->length, NULL)) return NULL;
    char *value = g_strndup((const char *)field->data, field->length);
    g_strstrip(value);
    if (value[0] == '\0') g_clear_pointer(&value, g_free);
    return value;
}

static gboolean parse_antigravity_timestamp(const ProtoField *generation, gint64 *milliseconds) {
    if (generation->wire != 2) return FALSE;
    ProtoReader outer = {generation->data, generation->length, 0, TRUE};
    ProtoField field;
    while (proto_field(&outer, &field)) {
        if (field.number != 4 || field.wire != 2) continue;
        ProtoReader stamp = {field.data, field.length, 0, TRUE};
        guint64 seconds = 0, nanos = 0;
        gboolean has_seconds = FALSE;
        ProtoField part;
        while (proto_field(&stamp, &part)) {
            if (part.wire != 0) continue;
            if (part.number == 1) {
                seconds = part.integer;
                has_seconds = TRUE;
            } else if (part.number == 2) {
                nanos = part.integer;
            }
        }
        if (!stamp.valid || !has_seconds || seconds == 0 || seconds > 253402300799ULL || nanos > 999999999) return FALSE;
        *milliseconds = (gint64)seconds * 1000 + (gint64)(nanos / 1000000);
        return TRUE;
    }
    return FALSE;
}

static gboolean parse_antigravity_usage(const ProtoField *usage,
                                        gint64 *input,
                                        gint64 *read,
                                        gint64 *output,
                                        gint64 *reasoning,
                                        char **response) {
    if (usage->wire != 2) return FALSE;
    ProtoReader reader = {usage->data, usage->length, 0, TRUE};
    ProtoField field;
    while (proto_field(&reader, &field)) {
        if (field.wire == 0 && field.integer <= G_MAXINT64) {
            if (field.number == 1 || field.number == 2) *input += (gint64)field.integer;
            else if (field.number == 5) *read = (gint64)field.integer;
            else if (field.number == 9) *output = (gint64)field.integer;
            else if (field.number == 10) *reasoning = (gint64)field.integer;
        } else if (field.number == 11) {
            char *value = proto_string(&field);
            if (value) {
                g_free(*response);
                *response = value;
            }
        }
    }
    return reader.valid;
}

static gboolean parse_antigravity_turn(const void *bytes,
                                       gsize length,
                                       gint64 *timestamp,
                                       gint64 *input,
                                       gint64 *read,
                                       gint64 *output,
                                       gint64 *reasoning,
                                       char **model,
                                       char **response) {
    ProtoReader root = {bytes, length, 0, TRUE};
    ProtoField root_field;
    gboolean found_chat = FALSE;
    while (proto_field(&root, &root_field)) {
        if (root_field.number != 1 || root_field.wire != 2) continue;
        found_chat = TRUE;
        ProtoReader chat = {root_field.data, root_field.length, 0, TRUE};
        ProtoField field;
        while (proto_field(&chat, &field)) {
            if (field.number == 4 && !parse_antigravity_usage(&field, input, read, output, reasoning, response)) return FALSE;
            if (field.number == 9) (void)parse_antigravity_timestamp(&field, timestamp);
            if (field.number == 19) {
                char *value = proto_string(&field);
                if (value) {
                    g_free(*model);
                    *model = value;
                }
            }
        }
        if (!chat.valid) return FALSE;
    }
    return root.valid && found_chat && *timestamp > 0 && (*input > 0 || *read > 0 || *output > 0);
}

static gboolean antigravity_schema_supported(sqlite3 *database) {
    sqlite3_stmt *statement = NULL;
    if (sqlite3_prepare_v2(database, "PRAGMA main.table_xinfo('gen_metadata')", -1, &statement, NULL) != SQLITE_OK) {
        return FALSE;
    }
    gboolean idx = FALSE, data = FALSE, valid = TRUE;
    guint columns = 0;
    while (sqlite3_step(statement) == SQLITE_ROW && ++columns <= 64) {
        if (sqlite3_column_type(statement, 1) != SQLITE_TEXT || sqlite3_column_int(statement, 6) != 0) {
            valid = FALSE;
            break;
        }
        const char *name = (const char *)sqlite3_column_text(statement, 1);
        if (g_ascii_strcasecmp(name, "idx") == 0) idx = TRUE;
        if (g_ascii_strcasecmp(name, "data") == 0) data = TRUE;
    }
    sqlite3_finalize(statement);
    return valid && columns <= 64 && idx && data;
}

static gboolean scan_antigravity_database(const char *path, AntigravityScan *scan, GError **error) {
    sqlite3 *database = NULL;
    if (sqlite3_open_v2(path, &database, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK || !database) {
        if (database) sqlite3_close(database);
        return TRUE;
    }
    sqlite3_limit(database, SQLITE_LIMIT_LENGTH, MAX_JSONL_LINE_BYTES);
    if (!antigravity_schema_supported(database)) {
        sqlite3_close(database);
        return TRUE;
    }
    sqlite3_stmt *statement = NULL;
    const char *query = "SELECT idx, data FROM main.gen_metadata NOT INDEXED LIMIT ?";
    if (sqlite3_prepare_v2(database, query, -1, &statement, NULL) != SQLITE_OK) {
        sqlite3_close(database);
        return TRUE;
    }
    sqlite3_bind_int(statement, 1, MAX_TRACE_ROWS + 1);
    char *session = g_path_get_basename(path);
    char *suffix = g_strrstr(session, ".db");
    if (suffix && suffix[3] == '\0') *suffix = '\0';
    guint rows = 0;
    while (sqlite3_step(statement) == SQLITE_ROW && ++rows <= MAX_TRACE_ROWS) {
        if (sqlite3_column_type(statement, 0) != SQLITE_INTEGER || sqlite3_column_int64(statement, 0) < 0 ||
            sqlite3_column_type(statement, 1) != SQLITE_BLOB) continue;
        int length = sqlite3_column_bytes(statement, 1);
        const void *bytes = sqlite3_column_blob(statement, 1);
        if (!bytes || length <= 0 || length > MAX_JSONL_LINE_BYTES) continue;
        gint64 timestamp = 0, input = 0, read = 0, output = 0, reasoning = 0;
        char *model = NULL, *response = NULL;
        if (parse_antigravity_turn(bytes, (gsize)length, &timestamp, &input, &read, &output, &reasoning,
                                   &model, &response)) {
            char *key = response ? g_strdup_printf("response:%s", response)
                                 : g_strdup_printf("row:%s:%" G_GINT64_FORMAT,
                                                   session, sqlite3_column_int64(statement, 0));
            char *day = millis_day(timestamp);
            if (day && !g_hash_table_contains(scan->response_ids, key) &&
                day_in_range(day, scan->since, scan->report->today)) {
                g_hash_table_add(scan->response_ids, key);
                key = NULL;
                add_local_usage(scan->report, day, model, session, input, read, 0, output,
                                input + read + output, FALSE, 0,
                                g_strcmp0(day, scan->current_since) >= 0);
            }
            g_free(day);
            g_free(key);
        }
        g_free(model);
        g_free(response);
    }
    g_free(session);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    (void)error;
    return TRUE;
}

static gboolean scan_antigravity_database_directory(const char *root, AntigravityScan *scan, GError **error) {
    GDir *directory = g_dir_open(root, 0, NULL);
    if (!directory) return TRUE;
    const char *name = NULL;
    guint files = 0;
    gboolean ok = TRUE;
    while (ok && (name = g_dir_read_name(directory))) {
        if (!g_str_has_suffix(name, ".db") || ++files > MAX_JSONL_FILES) continue;
        char *path = g_build_filename(root, name, NULL);
        struct stat info;
        if (lstat(path, &info) == 0 && S_ISREG(info.st_mode) && info.st_size <= MAX_JSONL_FILE_BYTES) {
            ok = scan_antigravity_database(path, scan, error);
        }
        g_free(path);
    }
    g_dir_close(directory);
    return ok;
}

static CodexBarCostReport *new_report(const char *provider, int history_days) {
    CodexBarCostReport *report = g_new0(CodexBarCostReport, 1);
    report->provider = g_strdup(provider);
    report->history_days = history_days;
    report->days = g_ptr_array_new_with_free_func(cost_day_free);
    report->projects = g_ptr_array_new_with_free_func(cost_project_free);
    report->models = g_ptr_array_new_with_free_func(cost_model_free);
    GDateTime *now = cost_now();
    report->today = g_date_time_format(now, "%Y-%m-%d");
    g_date_time_unref(now);
    return report;
}

static char *since_day(int history_days) {
    GDateTime *now = cost_now();
    GDateTime *since = g_date_time_add_days(now, -(history_days - 1));
    char *value = g_date_time_format(since, "%Y-%m-%d");
    g_date_time_unref(since);
    g_date_time_unref(now);
    return value;
}

CodexBarCostReport *codexbar_cost_scan(const char *provider, int history_days, GError **error) {
    if (!g_str_equal(provider, "codex") && !g_str_equal(provider, "claude") &&
        !g_str_equal(provider, "cursor") && !g_str_equal(provider, "antigravity")) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Cost is not supported for %s.", provider);
        return NULL;
    }
    history_days = CLAMP(history_days, 1, 365);
    CodexBarCostReport *report = new_report(provider, history_days);
    char *current_since = since_day(history_days);
    char *since = since_day(history_days * 2);
    gboolean ok = FALSE;
    if (g_str_equal(provider, "codex")) {
        CodexScan scan = {
            .report = report,
            .since = since,
            .current_since = current_since,
            .baselines = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, codex_baseline_free),
            .priority_turns = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, priority_turn_free),
        };
        GPtrArray *roots = codex_roots();
        ok = scan_roots(roots, collect_codex_baseline, scan.baselines, error);
        if (ok) {
            load_priority_turns(scan.priority_turns, since);
            ok = scan_roots(roots, scan_codex_file, &scan, error);
        }
        if (ok) {
            PiScan pi_scan = {
                .report = report,
                .since = since,
                .current_since = current_since,
                .seen_entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
            };
            GPtrArray *pi = pi_roots();
            ok = scan_roots(pi, scan_pi_file, &pi_scan, error);
            g_ptr_array_unref(pi);
            g_hash_table_unref(pi_scan.seen_entries);
        }
        g_ptr_array_unref(roots);
        g_hash_table_unref(scan.priority_turns);
        g_hash_table_unref(scan.baselines);
    } else if (g_str_equal(provider, "claude")) {
        ClaudeScan scan = {
            .report = report,
            .since = since,
            .current_since = current_since,
            .keyed_rows = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, claude_row_free),
            .unkeyed_rows = g_ptr_array_new_with_free_func(claude_row_free),
        };
        GPtrArray *roots = claude_roots();
        ok = scan_roots(roots, scan_claude_file, &scan, error);
        if (ok) {
            g_hash_table_foreach(scan.keyed_rows, aggregate_claude_row, &scan);
            for (guint index = 0; index < scan.unkeyed_rows->len; index++) {
                aggregate_claude_row(NULL, g_ptr_array_index(scan.unkeyed_rows, index), &scan);
            }
        }
        if (ok) {
            PiScan pi_scan = {
                .report = report,
                .since = since,
                .current_since = current_since,
                .seen_entries = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
            };
            GPtrArray *pi = pi_roots();
            ok = scan_roots(pi, scan_pi_file, &pi_scan, error);
            g_ptr_array_unref(pi);
            g_hash_table_unref(pi_scan.seen_entries);
        }
        g_ptr_array_unref(roots);
        g_hash_table_unref(scan.keyed_rows);
        g_ptr_array_unref(scan.unkeyed_rows);
    } else if (g_str_equal(provider, "cursor")) {
        const char *override = g_getenv("CODEXBAR_COST_CURSOR_ROOT");
        char *root = override && override[0] != '\0'
                         ? g_strdup(override)
                         : g_build_filename(g_get_user_config_dir(), "tokscale", "cursor-cache", NULL);
        ok = scan_cursor_csv(report, root, since, current_since, error);
        g_free(root);
    } else {
        AntigravityScan scan = {
            .report = report,
            .since = since,
            .current_since = current_since,
            .response_ids = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL),
        };
        const char *override = g_getenv("CODEXBAR_COST_ANTIGRAVITY_ROOT");
        char *root = override && override[0] != '\0'
                         ? g_strdup(override)
                         : g_build_filename(g_get_user_config_dir(), "tokscale", "antigravity-cache", "sessions", NULL);
        GPtrArray *roots = g_ptr_array_new_with_free_func(g_free);
        g_ptr_array_add(roots, root);
        ok = scan_roots(roots, scan_antigravity_file, &scan, error);
        g_ptr_array_unref(roots);
        if (ok) {
            const char *database_override = g_getenv("CODEXBAR_COST_ANTIGRAVITY_DB_ROOT");
            if (database_override && database_override[0] != '\0') {
                ok = scan_antigravity_database_directory(database_override, &scan, error);
            } else {
                const char *gemini_home = g_getenv("GEMINI_CLI_HOME");
                char *first = gemini_home && gemini_home[0] != '\0'
                                  ? g_build_filename(gemini_home, "antigravity-cli", "conversations", NULL)
                                  : g_build_filename(g_get_home_dir(), ".gemini", "antigravity-cli", "conversations", NULL);
                char *second = g_build_filename(g_get_home_dir(), ".gemini", "antigravity", NULL);
                char *third = g_build_filename(g_get_home_dir(), ".gemini", "antigravity", "conversations", NULL);
                ok = scan_antigravity_database_directory(first, &scan, error) &&
                     scan_antigravity_database_directory(second, &scan, error) &&
                     scan_antigravity_database_directory(third, &scan, error);
                g_free(first);
                g_free(second);
                g_free(third);
            }
        }
        g_hash_table_unref(scan.response_ids);
        g_free(scan.session_id);
        g_free(scan.model);
    }
    g_free(current_since);
    g_free(since);
    if (!ok) {
        codexbar_cost_report_free(report);
        return NULL;
    }
    finalize_report(report);
    return report;
}
