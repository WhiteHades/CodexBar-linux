#define _POSIX_C_SOURCE 200809L

#include "cost.h"

#include <errno.h>
#include <json-c/json.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

enum {
    MAX_JSONL_LINE_BYTES = 512 * 1024,
    MAX_DIRECTORY_DEPTH = 64,
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
    gint64 output;
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
} ClaudeRow;

typedef struct {
    CodexBarCostReport *report;
    const char *since;
    GHashTable *keyed_rows;
    GPtrArray *unkeyed_rows;
} ClaudeScan;

typedef struct {
    CodexBarCostReport *report;
    const char *since;
} CodexScan;

typedef gboolean (*JsonlFileCallback)(const char *path, gpointer user_data, GError **error);

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
    {"gpt-5.6-terra", 2.5e-6, 0.25e-6, 15e-6, 3.125e-6},
    {"gpt-5.6-luna", 1e-6, 0.1e-6, 6e-6, 1.25e-6},
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

static void claude_row_free(gpointer data) {
    ClaudeRow *row = data;
    if (!row) return;
    g_free(row->day);
    g_free(row->model);
    g_free(row->path);
    g_free(row->key);
    g_free(row);
}

void codexbar_cost_report_free(CodexBarCostReport *report) {
    if (!report) return;
    g_free(report->provider);
    g_free(report->today);
    g_ptr_array_unref(report->days);
    g_ptr_array_unref(report->projects);
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
    const char *model = g_str_has_prefix(raw, "openai/") ? raw + strlen("openai/") : raw;
    if (g_str_equal(model, "gpt-5.6")) return g_strdup("gpt-5.6-sol");
    if (find_pricing(codex_pricing, G_N_ELEMENTS(codex_pricing), model)) return g_strdup(model);
    gsize length = strlen(model);
    if (length > 11 && model[length - 11] == '-' && model[length - 8] == '-' && model[length - 5] == '-') {
        char *base = g_strndup(model, length - 11);
        if (find_pricing(codex_pricing, G_N_ELEMENTS(codex_pricing), base)) return base;
        g_free(base);
    }
    return g_strdup(model);
}

static char *normalize_claude_model(const char *raw) {
    if (!raw || raw[0] == '\0') return g_strdup("unknown");
    const char *model = raw;
    const char *embedded = g_strrstr(raw, ".claude-");
    if (embedded) model = embedded + 1;
    char *normalized = g_strdup(model);
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
            ok = walk_jsonl(path, depth + 1, callback, user_data, error);
        } else if (S_ISREG(status.st_mode) && g_str_has_suffix(name, ".jsonl") && status.st_size > 0) {
            ok = callback(path, user_data, error);
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

static gboolean scan_roots(GPtrArray *roots,
                           JsonlFileCallback callback,
                           gpointer user_data,
                           GError **error) {
    for (guint index = 0; index < roots->len; index++) {
        const char *root = g_ptr_array_index(roots, index);
        struct stat status;
        if (lstat(root, &status) != 0) {
            if (errno == ENOENT) continue;
            g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not inspect %s: %s", root, g_strerror(errno));
            return FALSE;
        }
        if (!S_ISDIR(status.st_mode)) continue;
        if (!walk_jsonl(root, 0, callback, user_data, error)) return FALSE;
    }
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

static TokenTotals token_totals(json_object *usage) {
    gint64 cached = integer_field(usage, "cached_input_tokens");
    if (cached == 0) cached = integer_field(usage, "cache_read_input_tokens");
    return (TokenTotals){
        .input = integer_field(usage, "input_tokens"),
        .cached = cached,
        .output = integer_field(usage, "output_tokens"),
    };
}

static gboolean totals_equal(TokenTotals left, TokenTotals right) {
    return left.input == right.input && left.cached == right.cached && left.output == right.output;
}

static TokenTotals totals_delta(TokenTotals current, TokenTotals baseline) {
    return (TokenTotals){
        .input = MAX((gint64)0, current.input - baseline.input),
        .cached = MAX((gint64)0, current.cached - baseline.cached),
        .output = MAX((gint64)0, current.output - baseline.output),
    };
}

static TokenTotals totals_min(TokenTotals left, TokenTotals right) {
    return (TokenTotals){MIN(left.input, right.input), MIN(left.cached, right.cached), MIN(left.output, right.output)};
}

static TokenTotals totals_max(TokenTotals left, TokenTotals right) {
    return (TokenTotals){MAX(left.input, right.input), MAX(left.cached, right.cached), MAX(left.output, right.output)};
}

typedef struct {
    CodexScan *scan;
    char *model;
    char *project;
    gboolean forked;
    gboolean has_counted;
    TokenTotals counted;
    gboolean has_baseline;
    TokenTotals baseline;
    gboolean has_watermark;
    TokenTotals watermark;
    gboolean divergent;
    gboolean interleaved;
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
    (void)path;
    CodexFile *file = user_data;
    const char *type = string_field(object, "type");
    json_object *payload = object_field(object, "payload");
    if (g_strcmp0(type, "session_meta") == 0) {
        const char *project = string_field(payload, "cwd");
        if (project && project[0] == '/') {
            g_free(file->project);
            file->project = g_canonicalize_filename(project, NULL);
        }
        if (string_field(payload, "forked_from_id") || string_field(payload, "forkedFromId") ||
            string_field(payload, "parent_session_id") || string_field(payload, "parentSessionId")) {
            file->forked = TRUE;
        }
        const char *source = string_field(payload, "source");
        if (source && g_ascii_strcasecmp(source, "subagent") == 0) file->forked = TRUE;
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
        return TRUE;
    }
    if (g_strcmp0(type, "event_msg") != 0 || g_strcmp0(string_field(payload, "type"), "token_count") != 0) {
        return TRUE;
    }
    if (file->forked) return TRUE;
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
        (total.input < file->watermark.input || total.cached < file->watermark.cached || total.output < file->watermark.output)) {
        file->interleaved = TRUE;
    }
    TokenTotals baseline = file->has_watermark ? file->watermark : (file->has_baseline ? file->baseline : (TokenTotals){0});
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
                total.output >= baseline.output && from_total.input <= last.input && from_total.cached <= last.cached &&
                from_total.output <= last.output) {
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
    file->counted.output += delta.output;
    file->has_counted = TRUE;
    if (has_total && !totals_equal(file->counted, total)) file->divergent = TRUE;

    const char *model = string_field(info, "model");
    if (!model) model = string_field(info, "model_name");
    if (!model) model = string_field(payload, "model");
    if (!model) model = file->model;
    double cost = 0;
    gboolean priced = codex_cost(model, delta.input, delta.cached, delta.output, &cost);
    add_usage(file->scan->report->days, day, delta.input, delta.cached, 0, delta.output, TRUE, priced, cost);
    CodexBarCostProject *project = find_project(file->scan->report, file->project);
    add_usage(project->days, day, delta.input, delta.cached, 0, delta.output, TRUE, priced, cost);
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
    if (file.forked) scan->report->skipped_fork_files++;
    g_array_unref(file.seen);
    g_free(file.model);
    g_free(file.project);
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

static CodexBarCostReport *new_report(const char *provider, int history_days) {
    CodexBarCostReport *report = g_new0(CodexBarCostReport, 1);
    report->provider = g_strdup(provider);
    report->history_days = history_days;
    report->days = g_ptr_array_new_with_free_func(cost_day_free);
    report->projects = g_ptr_array_new_with_free_func(cost_project_free);
    GDateTime *now = g_date_time_new_now_local();
    report->today = g_date_time_format(now, "%Y-%m-%d");
    g_date_time_unref(now);
    return report;
}

static char *since_day(int history_days) {
    GDateTime *now = g_date_time_new_now_local();
    GDateTime *since = g_date_time_add_days(now, -(history_days - 1));
    char *value = g_date_time_format(since, "%Y-%m-%d");
    g_date_time_unref(since);
    g_date_time_unref(now);
    return value;
}

CodexBarCostReport *codexbar_cost_scan(const char *provider, int history_days, GError **error) {
    if (!g_str_equal(provider, "codex") && !g_str_equal(provider, "claude")) {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Cost is not supported for %s.", provider);
        return NULL;
    }
    history_days = CLAMP(history_days, 1, 365);
    CodexBarCostReport *report = new_report(provider, history_days);
    char *since = since_day(history_days);
    gboolean ok = FALSE;
    if (g_str_equal(provider, "codex")) {
        CodexScan scan = {.report = report, .since = since};
        GPtrArray *roots = codex_roots();
        ok = scan_roots(roots, scan_codex_file, &scan, error);
        g_ptr_array_unref(roots);
    } else {
        ClaudeScan scan = {
            .report = report,
            .since = since,
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
        g_ptr_array_unref(roots);
        g_hash_table_unref(scan.keyed_rows);
        g_ptr_array_unref(scan.unkeyed_rows);
    }
    g_free(since);
    if (!ok) {
        codexbar_cost_report_free(report);
        return NULL;
    }
    finalize_report(report);
    return report;
}
