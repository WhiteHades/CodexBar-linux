#include "opencode_go.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <sqlite3.h>
#include <string.h>

#define FIVE_HOURS_MILLISECONDS (5LL * 60LL * 60LL * 1000LL)
#define WEEK_MILLISECONDS (7LL * 24LL * 60LL * 60LL * 1000LL)
#define WEB_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)
#define OPENCODE_GO_WORKSPACES_SERVER_ID "def39973159c7f0483d8793a822b8dbb10d067e12c65455fcb4608459ba0234f"
#define OPENCODE_GO_BILLING_SERVER_ID "c83b78a614689c38ebee981f9b39a8b377716db85c1fd7dbab604adc02d3313d"
#define OPENCODE_GO_BILLING_SCALE 100000000.0

typedef struct {
    gint64 created_ms;
    double cost;
} UsageRow;

typedef struct {
    gint64 start_ms;
    gint64 end_ms;
} MonthBounds;

typedef struct {
    double percent;
    gint64 reset_seconds;
    char *path;
    guint order;
} WebWindowCandidate;

static const char message_usage_sql[] =
    "SELECT "
    "CAST(COALESCE(json_extract(data, '$.time.created'), time_created) AS INTEGER), "
    "CAST(json_extract(data, '$.cost') AS REAL) "
    "FROM message "
    "WHERE json_valid(data) "
    "AND json_extract(data, '$.providerID') = 'opencode-go' "
    "AND json_extract(data, '$.role') = 'assistant' "
    "AND json_type(data, '$.cost') IN ('integer', 'real')";

static const char message_and_part_usage_sql[] =
    "WITH message_costs AS ("
    "SELECT id AS messageID, "
    "CAST(COALESCE(json_extract(data, '$.time.created'), time_created) AS INTEGER) AS createdMs, "
    "CAST(json_extract(data, '$.cost') AS REAL) AS cost "
    "FROM message "
    "WHERE json_valid(data) "
    "AND json_extract(data, '$.providerID') = 'opencode-go' "
    "AND json_extract(data, '$.role') = 'assistant' "
    "AND json_type(data, '$.cost') IN ('integer', 'real')"
    ") "
    "SELECT createdMs, cost FROM message_costs "
    "UNION ALL "
    "SELECT "
    "CAST(COALESCE(json_extract(p.data, '$.time.created'), p.time_created, m.time_created) AS INTEGER), "
    "CAST(json_extract(p.data, '$.cost') AS REAL) "
    "FROM part p "
    "JOIN message m ON m.id = p.message_id "
    "WHERE json_valid(p.data) "
    "AND json_valid(m.data) "
    "AND json_extract(p.data, '$.type') = 'step-finish' "
    "AND json_type(p.data, '$.cost') IN ('integer', 'real') "
    "AND json_extract(m.data, '$.providerID') = 'opencode-go' "
    "AND json_extract(m.data, '$.role') = 'assistant' "
    "AND NOT EXISTS ("
    "SELECT 1 FROM message_costs WHERE message_costs.messageID = p.message_id"
    ")";

static gint64 date_time_ms(GDateTime *date_time) {
    return g_date_time_to_unix(date_time) * 1000 + g_date_time_get_microsecond(date_time) / 1000;
}

static GDateTime *date_time_from_ms(gint64 milliseconds) {
    GDateTime *seconds = g_date_time_new_from_unix_utc(milliseconds / 1000);
    if (!seconds) return NULL;
    GDateTime *result = g_date_time_add(seconds, (milliseconds % 1000) * 1000);
    g_date_time_unref(seconds);
    return result;
}

static gboolean has_auth_key(const char *path) {
    char *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &contents, &length, NULL)) return FALSE;
    if (length > G_MAXINT) {
        g_free(contents);
        return FALSE;
    }

    struct json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
    json_object *root = json_tokener_parse_ex(tokener, contents, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && g_ascii_isspace(contents[consumed])) consumed++;
    json_tokener_free(tokener);
    g_free(contents);
    if (parse_error != json_tokener_success || consumed != length || !root) {
        if (root) json_object_put(root);
        return FALSE;
    }

    json_object *entry = NULL;
    json_object *key = NULL;
    gboolean result = FALSE;
    if (json_object_is_type(root, json_type_object) &&
        json_object_object_get_ex(root, "opencode-go", &entry) &&
        json_object_is_type(entry, json_type_object) &&
        json_object_object_get_ex(entry, "key", &key) &&
        json_object_is_type(key, json_type_string)) {
        char *value = g_strdup(json_object_get_string(key));
        result = value && g_strstrip(value)[0] != '\0';
        g_free(value);
    }
    json_object_put(root);
    return result;
}

static gboolean database_has_table(sqlite3 *database, const char *name) {
    sqlite3_stmt *statement = NULL;
    const char sql[] = "SELECT 1 FROM sqlite_master WHERE type = 'table' AND name = ? LIMIT 1";
    if (sqlite3_prepare_v2(database, sql, -1, &statement, NULL) != SQLITE_OK) return FALSE;
    sqlite3_bind_text(statement, 1, name, -1, SQLITE_TRANSIENT);
    gboolean result = sqlite3_step(statement) == SQLITE_ROW;
    sqlite3_finalize(statement);
    return result;
}

static GArray *read_rows(const char *path, GError **error) {
    sqlite3 *database = NULL;
    int status = sqlite3_open_v2(path, &database, SQLITE_OPEN_READONLY, NULL);
    if (status != SQLITE_OK) {
        const char *message = database ? sqlite3_errmsg(database) : "unknown error";
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "SQLite error reading OpenCode Go usage: %s", message);
        sqlite3_close(database);
        return NULL;
    }
    sqlite3_busy_timeout(database, 250);

    const char *sql = database_has_table(database, "part") ? message_and_part_usage_sql : message_usage_sql;
    sqlite3_stmt *statement = NULL;
    status = sqlite3_prepare_v2(database, sql, -1, &statement, NULL);
    if (status != SQLITE_OK) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "SQLite error reading OpenCode Go usage: %s",
                    sqlite3_errmsg(database));
        sqlite3_close(database);
        return NULL;
    }

    GArray *rows = g_array_new(FALSE, FALSE, sizeof(UsageRow));
    while ((status = sqlite3_step(statement)) == SQLITE_ROW) {
        UsageRow row = {
            .created_ms = sqlite3_column_int64(statement, 0),
            .cost = sqlite3_column_double(statement, 1),
        };
        if (row.created_ms > 0 && row.cost >= 0 && isfinite(row.cost)) g_array_append_val(rows, row);
    }
    if (status != SQLITE_DONE) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "SQLite error reading OpenCode Go usage: %s",
                    sqlite3_errmsg(database));
        g_array_unref(rows);
        rows = NULL;
    }
    sqlite3_finalize(statement);
    sqlite3_close(database);
    return rows;
}

static gint days_in_month(gint year, gint month) {
    return g_date_get_days_in_month((GDateMonth)month, (GDateYear)year);
}

static GDateTime *anchored_month(gint year,
                                 gint month,
                                 gint anchor_day,
                                 gint anchor_hour,
                                 gint anchor_minute,
                                 double anchor_second) {
    gint day = MIN(anchor_day, days_in_month(year, month));
    return g_date_time_new_utc(year, month, day, anchor_hour, anchor_minute, anchor_second);
}

static void previous_month(gint *year, gint *month) {
    if (*month == 1) {
        *month = 12;
        (*year)--;
    } else {
        (*month)--;
    }
}

static void next_month(gint *year, gint *month) {
    if (*month == 12) {
        *month = 1;
        (*year)++;
    } else {
        (*month)++;
    }
}

static MonthBounds month_bounds(gint64 now_ms, gint64 anchor_ms) {
    GDateTime *now = date_time_from_ms(now_ms);
    GDateTime *anchor = date_time_from_ms(anchor_ms);
    gint year = g_date_time_get_year(now);
    gint month = g_date_time_get_month(now);
    gint anchor_day = g_date_time_get_day_of_month(anchor);
    gint anchor_hour = g_date_time_get_hour(anchor);
    gint anchor_minute = g_date_time_get_minute(anchor);
    double anchor_second = g_date_time_get_second(anchor) + g_date_time_get_microsecond(anchor) / 1000000.0;

    GDateTime *start = anchored_month(
        year, month, anchor_day, anchor_hour, anchor_minute, anchor_second);
    if (date_time_ms(start) > now_ms) {
        g_date_time_unref(start);
        previous_month(&year, &month);
        start = anchored_month(year, month, anchor_day, anchor_hour, anchor_minute, anchor_second);
    }
    gint end_year = year;
    gint end_month = month;
    next_month(&end_year, &end_month);
    GDateTime *end = anchored_month(
        end_year, end_month, anchor_day, anchor_hour, anchor_minute, anchor_second);
    MonthBounds bounds = {.start_ms = date_time_ms(start), .end_ms = date_time_ms(end)};
    g_date_time_unref(start);
    g_date_time_unref(end);
    g_date_time_unref(anchor);
    g_date_time_unref(now);
    return bounds;
}

static gint64 utc_week_start_ms(gint64 now_ms) {
    GDateTime *now = date_time_from_ms(now_ms);
    GDateTime *day = g_date_time_new_utc(g_date_time_get_year(now),
                                        g_date_time_get_month(now),
                                        g_date_time_get_day_of_month(now),
                                        0,
                                        0,
                                        0);
    GDateTime *monday = g_date_time_add_days(day, -(g_date_time_get_day_of_week(now) - 1));
    gint64 result = date_time_ms(monday);
    g_date_time_unref(monday);
    g_date_time_unref(day);
    g_date_time_unref(now);
    return result;
}

static double sum_rows(const GArray *rows, gint64 start_ms, gint64 end_ms) {
    double total = 0;
    for (guint index = 0; index < rows->len; index++) {
        UsageRow row = g_array_index(rows, UsageRow, index);
        if (row.created_ms >= start_ms && row.created_ms < end_ms) total += row.cost;
    }
    return total;
}

static double used_percent(double used, double limit) {
    if (!isfinite(used) || limit <= 0) return 0;
    double display = codexbar_usage_percent_display(codexbar_usage_percent_from_ratio(used, limit));
    return round(display * 10) / 10;
}

static gint64 rolling_reset_seconds(const GArray *rows, gint64 now_ms) {
    gint64 start_ms = now_ms - FIVE_HOURS_MILLISECONDS;
    gint64 oldest_ms = now_ms;
    for (guint index = 0; index < rows->len; index++) {
        UsageRow row = g_array_index(rows, UsageRow, index);
        if (row.created_ms >= start_ms && row.created_ms < now_ms && row.created_ms < oldest_ms) {
            oldest_ms = row.created_ms;
        }
    }
    return MAX(0, (oldest_ms + FIVE_HOURS_MILLISECONDS - now_ms) / 1000);
}

static CodexBarQuotaWindow *make_window(const char *id,
                                        const char *title,
                                        double percent,
                                        gint64 minutes,
                                        gint64 reset_seconds,
                                        gint64 now_ms) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = percent;
    window->has_window_minutes = TRUE;
    window->window_minutes = minutes;
    if (reset_seconds >= 0 && reset_seconds <= G_MAXINT64 / 1000) {
        gint64 reset_delta_ms = reset_seconds * 1000;
        if (now_ms <= G_MAXINT64 - reset_delta_ms) {
            window->has_resets_at = TRUE;
            window->resets_at_ms = now_ms + reset_delta_ms;
        }
    }
    return window;
}

static gboolean opencode_go_capture_number(const char *text,
                                           const char *label,
                                           const char *key,
                                           double *result) {
    char *pattern = g_strdup_printf(
        "%s[^}]*?%s\\s*:\\s*([0-9]+(?:\\.[0-9]+)?)", label, key);
    GRegex *regex = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
    g_free(pattern);
    if (!regex) return FALSE;
    GMatchInfo *match = NULL;
    gboolean found = g_regex_match(regex, text, 0, &match);
    char *capture = found ? g_match_info_fetch(match, 1) : NULL;
    g_match_info_free(match);
    g_regex_unref(regex);
    if (!capture) return FALSE;
    char *end = NULL;
    double value = g_ascii_strtod(capture, &end);
    gboolean valid = capture[0] && end && *end == '\0' && isfinite(value);
    g_free(capture);
    if (valid) *result = value;
    return valid;
}

static gboolean opencode_go_web_window(const char *text,
                                       const char *label,
                                       double *percent,
                                       gint64 *reset_seconds) {
    double reset = 0;
    if (!opencode_go_capture_number(text, label, "usagePercent", percent) ||
        !opencode_go_capture_number(text, label, "resetInSec", &reset) || reset < 0 || reset >= 0x1p63) {
        return FALSE;
    }
    *percent = CLAMP(*percent, 0.0, 100.0);
    *reset_seconds = (gint64)llround(reset);
    return TRUE;
}

static void web_window_candidate_free(gpointer data) {
    WebWindowCandidate *candidate = data;
    if (!candidate) return;
    g_free(candidate->path);
    g_free(candidate);
}

static gboolean opencode_go_json_number(json_object *object, const char *key, double *result) {
    json_object *value = NULL;
    if (!object || !json_object_is_type(object, json_type_object) ||
        !json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null) ||
        json_object_is_type(value, json_type_boolean)) {
        return FALSE;
    }
    double number = 0;
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        number = json_object_get_double(value);
    } else if (json_object_is_type(value, json_type_string)) {
        const char *raw = json_object_get_string(value);
        char *end = NULL;
        number = g_ascii_strtod(raw, &end);
        if (!raw[0] || !end || *end) return FALSE;
    } else {
        return FALSE;
    }
    if (!isfinite(number)) return FALSE;
    *result = number;
    return TRUE;
}

static gboolean opencode_go_reset_at(json_object *object,
                                     const char *key,
                                     gint64 now_ms,
                                     gint64 *result) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) {
        return FALSE;
    }
    gint64 reset_ms = 0;
    if (json_object_is_type(value, json_type_string)) {
        const char *raw = json_object_get_string(value);
        GDateTime *time = g_date_time_new_from_iso8601(raw, NULL);
        if (time) {
            reset_ms = date_time_ms(time);
            g_date_time_unref(time);
        } else {
            char *end = NULL;
            double numeric = g_ascii_strtod(raw, &end);
            if (!raw[0] || !end || *end || !isfinite(numeric)) return FALSE;
            double milliseconds = numeric > 100000000000.0 ? numeric : numeric * 1000.0;
            if (!isfinite(milliseconds) || milliseconds < -0x1p63 || milliseconds >= 0x1p63) return FALSE;
            reset_ms = (gint64)llround(milliseconds);
        }
    } else if (json_object_is_type(value, json_type_int)) {
        gint64 numeric = json_object_get_int64(value);
        if (numeric > 100000000000LL) {
            reset_ms = numeric;
        } else {
            if (numeric > G_MAXINT64 / 1000 || numeric < G_MININT64 / 1000) return FALSE;
            reset_ms = numeric * 1000;
        }
    } else if (json_object_is_type(value, json_type_double)) {
        double numeric = json_object_get_double(value);
        if (!isfinite(numeric)) return FALSE;
        double milliseconds = numeric > 100000000000.0 ? numeric : numeric * 1000.0;
        if (!isfinite(milliseconds) || milliseconds < -0x1p63 || milliseconds >= 0x1p63) return FALSE;
        reset_ms = (gint64)llround(milliseconds);
    } else {
        return FALSE;
    }
    if (reset_ms <= now_ms) {
        *result = 0;
    } else {
        if (now_ms < 0 && reset_ms > G_MAXINT64 + now_ms) return FALSE;
        *result = (reset_ms - now_ms) / 1000;
    }
    return TRUE;
}

static gboolean opencode_go_json_window(json_object *object,
                                        gint64 now_ms,
                                        double *percent,
                                        gint64 *reset_seconds) {
    static const char *const percent_keys[] = {
        "usagePercent", "usedPercent", "percentUsed", "percent", "usage_percent", "used_percent",
        "utilization", "utilizationPercent", "utilization_percent", "usage",
    };
    static const char *const used_keys[] = {"used", "usage", "consumed", "count", "usedTokens"};
    static const char *const limit_keys[] = {"limit", "total", "quota", "max", "cap", "tokenLimit"};
    static const char *const reset_in_keys[] = {
        "resetInSec", "resetInSeconds", "resetSeconds", "reset_sec", "reset_in_sec",
        "resetsInSec", "resetsInSeconds", "resetIn", "resetSec",
    };
    static const char *const reset_at_keys[] = {
        "resetAt", "resetsAt", "reset_at", "resets_at", "nextReset", "next_reset", "renewAt", "renew_at",
    };
    gboolean direct = FALSE;
    for (guint index = 0; index < G_N_ELEMENTS(percent_keys) && !direct; index++) {
        direct = opencode_go_json_number(object, percent_keys[index], percent);
    }
    if (!direct) {
        double used = 0, limit = 0;
        gboolean has_used = FALSE, has_limit = FALSE;
        for (guint index = 0; index < G_N_ELEMENTS(used_keys) && !has_used; index++) {
            has_used = opencode_go_json_number(object, used_keys[index], &used);
        }
        for (guint index = 0; index < G_N_ELEMENTS(limit_keys) && !has_limit; index++) {
            has_limit = opencode_go_json_number(object, limit_keys[index], &limit);
        }
        if (!has_used || !has_limit || limit <= 0) return FALSE;
        *percent = used / limit * 100.0;
    } else if (*percent >= 0 && *percent <= 1.0) {
        *percent *= 100.0;
    }
    *percent = CLAMP(*percent, 0.0, 100.0);
    double reset = 0;
    gboolean has_reset = FALSE;
    for (guint index = 0; index < G_N_ELEMENTS(reset_in_keys) && !has_reset; index++) {
        has_reset = opencode_go_json_number(object, reset_in_keys[index], &reset);
    }
    if (has_reset) {
        *reset_seconds = reset <= 0 ? 0 : (reset >= 0x1p63 ? G_MAXINT64 : (gint64)llround(reset));
        return TRUE;
    }
    for (guint index = 0; index < G_N_ELEMENTS(reset_at_keys); index++) {
        if (opencode_go_reset_at(object, reset_at_keys[index], now_ms, reset_seconds)) return TRUE;
    }
    *reset_seconds = 0;
    return TRUE;
}

static void opencode_go_collect_windows(json_object *value,
                                        gint64 now_ms,
                                        const char *path,
                                        guint depth,
                                        GPtrArray *candidates) {
    if (!value || depth > 20) return;
    if (json_object_is_type(value, json_type_object)) {
        double percent = 0;
        gint64 reset = 0;
        if (opencode_go_json_window(value, now_ms, &percent, &reset)) {
            WebWindowCandidate *candidate = g_new0(WebWindowCandidate, 1);
            candidate->percent = percent;
            candidate->reset_seconds = reset;
            candidate->path = g_ascii_strdown(path ? path : "", -1);
            candidate->order = candidates->len;
            g_ptr_array_add(candidates, candidate);
        }
        json_object_object_foreach(value, key, child) {
            char *child_path = path && path[0] ? g_strdup_printf("%s.%s", path, key) : g_strdup(key);
            opencode_go_collect_windows(child, now_ms, child_path, depth + 1, candidates);
            g_free(child_path);
        }
    } else if (json_object_is_type(value, json_type_array)) {
        for (size_t index = 0; index < json_object_array_length(value); index++) {
            char *child_path = g_strdup_printf("%s[%zu]", path ? path : "", index);
            opencode_go_collect_windows(
                json_object_array_get_idx(value, index), now_ms, child_path, depth + 1, candidates);
            g_free(child_path);
        }
    }
}

static gboolean path_has(const char *path, const char *const *needles, guint count) {
    for (guint index = 0; index < count; index++) {
        if (strstr(path, needles[index])) return TRUE;
    }
    return FALSE;
}

static WebWindowCandidate *opencode_go_pick_window(GPtrArray *candidates,
                                                    int family,
                                                    gboolean allow_fallback,
                                                    const WebWindowCandidate *exclude_a,
                                                    const WebWindowCandidate *exclude_b) {
    static const char *const rolling_names[] = {"rolling", "hour", "5h", "5-hour"};
    static const char *const weekly_names[] = {"weekly", "week"};
    static const char *const monthly_names[] = {"monthly", "month"};
    const char *const *names = family == 0 ? rolling_names : family == 1 ? weekly_names : monthly_names;
    guint count = family == 0 ? G_N_ELEMENTS(rolling_names)
                              : family == 1 ? G_N_ELEMENTS(weekly_names) : G_N_ELEMENTS(monthly_names);
    WebWindowCandidate *best = NULL;
    for (guint pass = 0; pass < (allow_fallback ? 2U : 1U); pass++) {
        for (guint index = 0; index < candidates->len; index++) {
            WebWindowCandidate *candidate = g_ptr_array_index(candidates, index);
            if (candidate == exclude_a || candidate == exclude_b) continue;
            gboolean named = path_has(candidate->path, names, count);
            if ((pass == 0 && !named) || (pass == 1 && named)) continue;
            if (!best || (family == 0 ? candidate->reset_seconds < best->reset_seconds
                                      : candidate->reset_seconds > best->reset_seconds) ||
                (candidate->reset_seconds == best->reset_seconds && candidate->percent > best->percent)) {
                best = candidate;
            }
        }
        if (best) return best;
    }
    return NULL;
}

static gboolean opencode_go_json_windows(const char *text,
                                         size_t length,
                                         gint64 now_ms,
                                         WebWindowCandidate **rolling,
                                         WebWindowCandidate **weekly,
                                         WebWindowCandidate **monthly,
                                         GPtrArray **storage) {
    if (length > G_MAXINT) return FALSE;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, text, (int)length);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && g_ascii_isspace(text[consumed])) consumed++;
    gboolean valid = json_tokener_get_error(tokener) == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (!valid) {
        if (root) json_object_put(root);
        return FALSE;
    }
    GPtrArray *candidates = g_ptr_array_new_with_free_func(web_window_candidate_free);
    opencode_go_collect_windows(root, now_ms, "", 0, candidates);
    json_object_put(root);
    *rolling = opencode_go_pick_window(candidates, 0, TRUE, NULL, NULL);
    *weekly = opencode_go_pick_window(candidates, 1, FALSE, *rolling, NULL);
    *monthly = opencode_go_pick_window(candidates, 2, FALSE, *rolling, *weekly);
    *storage = candidates;
    return *rolling != NULL;
}

CodexBarProvider *codexbar_opencode_go_parse_web_usage(const char *text,
                                                       size_t length,
                                                       gint64 now_ms,
                                                       GError **error) {
    if (!text || length == 0 || length > WEB_MAXIMUM_RESPONSE_BYTES || memchr(text, '\0', length) ||
        !g_utf8_validate(text, (gssize)length, NULL)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenCode Go usage response is invalid");
        return NULL;
    }
    char *document = g_strndup(text, length);
    double rolling = 0, weekly = 0, monthly = 0;
    gint64 rolling_reset = 0, weekly_reset = 0, monthly_reset = 0;
    WebWindowCandidate *rolling_json = NULL, *weekly_json = NULL, *monthly_json = NULL;
    GPtrArray *json_candidates = NULL;
    gboolean has_rolling = opencode_go_json_windows(
        document, length, now_ms, &rolling_json, &weekly_json, &monthly_json, &json_candidates);
    gboolean has_weekly = FALSE, has_monthly = FALSE;
    if (has_rolling) {
        rolling = rolling_json->percent;
        rolling_reset = rolling_json->reset_seconds;
        if (weekly_json) {
            has_weekly = TRUE;
            weekly = weekly_json->percent;
            weekly_reset = weekly_json->reset_seconds;
        }
        if (monthly_json) {
            has_monthly = TRUE;
            monthly = monthly_json->percent;
            monthly_reset = monthly_json->reset_seconds;
        }
    } else {
        if (json_candidates) g_ptr_array_unref(json_candidates);
        json_candidates = NULL;
        has_rolling = opencode_go_web_window(document, "rollingUsage", &rolling, &rolling_reset);
        has_weekly = opencode_go_web_window(document, "weeklyUsage", &weekly, &weekly_reset);
        has_monthly = opencode_go_web_window(document, "monthlyUsage", &monthly, &monthly_reset);
    }
    if (!has_rolling) {
        char *lower = g_ascii_strdown(document, -1);
        gboolean signed_out = strstr(lower, "login") || strstr(lower, "sign in") ||
                              strstr(lower, "auth/authorize") ||
                              strstr(lower, "not associated with an account") ||
                              strstr(lower, "actor of type \"public\"");
        g_free(lower);
        if (json_candidates) g_ptr_array_unref(json_candidates);
        g_free(document);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            signed_out ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_INVALID_DATA,
                            signed_out ? "OpenCode Go session is invalid or expired"
                                       : "OpenCode Go usage response is missing usage fields");
        return NULL;
    }
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("opencodego");
    provider->source = g_strdup("web");
    provider->dashboard_url = g_strdup("https://opencode.ai");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    codexbar_provider_add_quota_window(
        provider, make_window("primary", "5-hour", rolling, 300, rolling_reset, now_ms));
    if (has_weekly) {
        codexbar_provider_add_quota_window(
            provider, make_window("secondary", "Weekly", weekly, 10080, weekly_reset, now_ms));
    }
    if (has_monthly) {
        codexbar_provider_add_quota_window(
            provider, make_window("tertiary", "Monthly", monthly, 43200, monthly_reset, now_ms));
    }
    if (json_candidates) g_ptr_array_unref(json_candidates);
    g_free(document);
    return provider;
}

static char *opencode_go_config_string(const CodexBarProviderConfig *config, const char *key) {
    json_object *value = NULL;
    if (!config || !config->raw || !json_object_object_get_ex(config->raw, key, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    const char *raw = json_object_get_string(value);
    if (!raw || strlen(raw) > 16384 || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *clean = g_strdup(raw);
    g_strstrip(clean);
    for (const unsigned char *cursor = (const unsigned char *)clean; *cursor; cursor++) {
        if (*cursor < 32 || *cursor == 127) {
            g_free(clean);
            return NULL;
        }
    }
    if (clean[0]) return clean;
    g_free(clean);
    return NULL;
}

static char *opencode_go_cookie(const CodexBarProviderConfig *config) {
    char *cookie = opencode_go_config_string(config, "cookieHeader");
    if (!cookie) cookie = opencode_go_config_string(config, "manualCookieHeader");
    if (!cookie) {
        const char *raw = g_getenv("OPENCODE_GO_COOKIE");
        if (!raw) raw = g_getenv("OPENCODEGO_COOKIE");
        if (raw) {
            CodexBarProviderConfig environment = {0};
            environment.raw = json_object_new_object();
            json_object_object_add(environment.raw, "cookieHeader", json_object_new_string(raw));
            cookie = opencode_go_config_string(&environment, "cookieHeader");
            json_object_put(environment.raw);
        }
    }
    return cookie;
}

static char *opencode_go_workspace(const CodexBarProviderConfig *config) {
    char *raw = opencode_go_config_string(config, "workspaceID");
    if (!raw) raw = g_strdup(g_getenv("CODEXBAR_OPENCODEGO_WORKSPACE_ID"));
    if (!raw) return NULL;
    GRegex *regex = g_regex_new("wrk_[A-Za-z0-9]+", 0, 0, NULL);
    GMatchInfo *match = NULL;
    char *workspace = regex && g_regex_match(regex, raw, 0, &match) ? g_match_info_fetch(match, 0) : NULL;
    if (match) g_match_info_free(match);
    if (regex) g_regex_unref(regex);
    g_free(raw);
    return workspace;
}

static CodexBarHttpResponse *opencode_go_send(const CodexBarHttpRequest *request,
                                              CodexBarOpenCodeGoTransport transport,
                                              GError **error) {
    if (request->cancellable && g_cancellable_set_error_if_cancelled(request->cancellable, error)) return NULL;
    CodexBarHttpResponse *response = transport(request, error);
    if (request->cancellable && g_cancellable_is_cancelled(request->cancellable)) {
        codexbar_http_response_free(response);
        if (error && *error) g_clear_error(error);
        g_cancellable_set_error_if_cancelled(request->cancellable, error);
        return NULL;
    }
    if (!response && error && !*error) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OpenCode Go request failed");
    }
    return response;
}

static char *opencode_go_workspace_from_response(CodexBarHttpResponse *response,
                                                 const char *endpoint,
                                                 GError **error) {
    if (response->status != 200 || !g_utf8_validate(response->body, (gssize)response->body_length, NULL)) {
        g_set_error(error,
                    G_IO_ERROR,
                    response->status == 401 || response->status == 403 ? G_IO_ERROR_PERMISSION_DENIED
                                                                       : G_IO_ERROR_FAILED,
                    "OpenCode Go %s returned HTTP %ld",
                    endpoint,
                    response->status);
        return NULL;
    }
    char *body = g_strndup(response->body, response->body_length);
    GRegex *regex = g_regex_new("wrk_[A-Za-z0-9]+", 0, 0, NULL);
    GMatchInfo *match = NULL;
    char *workspace = regex && g_regex_match(regex, body, 0, &match) ? g_match_info_fetch(match, 0) : NULL;
    if (match) g_match_info_free(match);
    if (regex) g_regex_unref(regex);
    g_free(body);
    return workspace;
}

static char *opencode_go_discover_workspace(const char *cookie,
                                            CodexBarOpenCodeGoTransport transport,
                                            GCancellable *cancellable,
                                            GError **error) {
    char *instance = g_uuid_string_random();
    char *server_instance = g_strconcat("server-fn:", instance, NULL);
    g_free(instance);
    const CodexBarHttpRequestHeader headers[] = {
        {"Cookie", cookie},
        {"X-Server-Id", OPENCODE_GO_WORKSPACES_SERVER_ID},
        {"X-Server-Instance", server_instance},
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/143.0.0.0 Safari/537.36"},
        {"Origin", "https://opencode.ai"},
        {"Referer", "https://opencode.ai"},
        {"Accept", "text/javascript, application/json;q=0.9, */*;q=0.8"},
    };
    const CodexBarHttpRequest request = {
        .url = "https://opencode.ai/_server?id=" OPENCODE_GO_WORKSPACES_SERVER_ID,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 15,
        .maximum_response_bytes = WEB_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = opencode_go_send(&request, transport, error);
    if (!response) {
        g_free(server_instance);
        return NULL;
    }
    char *workspace = opencode_go_workspace_from_response(response, "workspace endpoint", error);
    codexbar_http_response_free(response);
    if (workspace || (error && *error)) {
        g_free(server_instance);
        return workspace;
    }

    const CodexBarHttpRequestHeader post_headers[] = {
        {"Cookie", cookie},
        {"X-Server-Id", OPENCODE_GO_WORKSPACES_SERVER_ID},
        {"X-Server-Instance", server_instance},
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/143.0.0.0 Safari/537.36"},
        {"Origin", "https://opencode.ai"},
        {"Referer", "https://opencode.ai"},
        {"Accept", "text/javascript, application/json;q=0.9, */*;q=0.8"},
        {"Content-Type", "application/json"},
    };
    static const char post_body[] = "[]";
    const CodexBarHttpRequest post_request = {
        .url = "https://opencode.ai/_server?id=" OPENCODE_GO_WORKSPACES_SERVER_ID,
        .method = "POST",
        .headers = post_headers,
        .header_count = G_N_ELEMENTS(post_headers),
        .body = post_body,
        .body_length = sizeof(post_body) - 1,
        .timeout_seconds = 15,
        .maximum_response_bytes = WEB_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    response = opencode_go_send(&post_request, transport, error);
    g_free(server_instance);
    if (!response) return NULL;
    workspace = opencode_go_workspace_from_response(response, "workspace fallback", error);
    codexbar_http_response_free(response);
    if (!workspace && (!error || !*error)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "OpenCode Go workspace response is missing workspace id");
    }
    return workspace;
}

static gboolean opencode_go_number(json_object *value, double *number) {
    if (!value || json_object_is_type(value, json_type_boolean) || json_object_is_type(value, json_type_null)) {
        return FALSE;
    }
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        *number = json_object_get_double(value);
        return isfinite(*number);
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    char *clean = g_strdup(json_object_get_string(value));
    g_strdelimit(clean, ",", ' ');
    g_strstrip(clean);
    char *end = NULL;
    *number = g_ascii_strtod(clean, &end);
    gboolean valid = clean[0] && end && *end == '\0' && isfinite(*number);
    g_free(clean);
    return valid;
}

static gboolean opencode_go_explicit_balance_key(const char *key) {
    char *lower = g_ascii_strdown(key, -1);
    GString *normalized = g_string_new(NULL);
    for (const char *cursor = lower; *cursor; cursor++) {
        if (g_ascii_isalnum(*cursor)) g_string_append_c(normalized, *cursor);
    }
    const char *value = normalized->str;
    gboolean matches = g_str_equal(value, "zenbalance") || g_str_equal(value, "zencurrentbalance") ||
                       g_str_equal(value, "currentbalance") || g_str_equal(value, "currentbalanceusd") ||
                       g_str_equal(value, "balanceusd") || g_str_equal(value, "usdbalance");
    g_string_free(normalized, TRUE);
    g_free(lower);
    return matches;
}

static gboolean opencode_go_find_balance(json_object *value, gboolean billing, guint depth, double *balance) {
    if (!value || depth > 20) return FALSE;
    if (json_object_is_type(value, json_type_object)) {
        if (billing) {
            json_object *customer = NULL, *raw_balance = NULL;
            if (json_object_object_get_ex(value, "customerID", &customer) &&
                json_object_is_type(customer, json_type_string) && json_object_get_string(customer)[0] &&
                json_object_object_get_ex(value, "balance", &raw_balance) &&
                opencode_go_number(raw_balance, balance)) {
                *balance /= OPENCODE_GO_BILLING_SCALE;
                return TRUE;
            }
        }
        json_object_object_foreach(value, key, child) {
            if (!billing && opencode_go_explicit_balance_key(key) && opencode_go_number(child, balance)) return TRUE;
            if (opencode_go_find_balance(child, billing, depth + 1, balance)) return TRUE;
        }
    } else if (json_object_is_type(value, json_type_array)) {
        for (size_t index = 0; index < json_object_array_length(value); index++) {
            if (opencode_go_find_balance(json_object_array_get_idx(value, index), billing, depth + 1, balance)) {
                return TRUE;
            }
        }
    }
    return FALSE;
}

gboolean codexbar_opencode_go_parse_zen_balance(const char *text,
                                                size_t length,
                                                gboolean billing_response,
                                                double *balance) {
    if (!text || !balance || length == 0 || length > WEB_MAXIMUM_RESPONSE_BYTES || memchr(text, '\0', length) ||
        !g_utf8_validate(text, (gssize)length, NULL) || length > G_MAXINT) return FALSE;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, text, (int)length);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && g_ascii_isspace(text[consumed])) consumed++;
    gboolean valid = json_tokener_get_error(tokener) == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (valid && opencode_go_find_balance(root, billing_response, 0, balance)) {
        json_object_put(root);
        return isfinite(*balance);
    }
    if (root) json_object_put(root);

    const char *pattern = billing_response
                              ? "(?:customerID|\\\"customerID\\\")[^}]{0,240}(?:balance|\\\"balance\\\")\\s*:\\s*(-?[0-9]+(?:\\.[0-9]+)?)"
                              : "(?:current\\s+balance|zen\\s+balance|balance)[^$]{0,120}\\$\\s*([0-9][0-9,]*(?:\\.[0-9]+)?)";
    GRegex *regex = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_DOTALL, 0, NULL);
    GMatchInfo *match = NULL;
    gboolean found = regex && g_regex_match_full(regex, text, (gssize)length, 0, 0, &match, NULL);
    char *capture = found ? g_match_info_fetch(match, 1) : NULL;
    if (match) g_match_info_free(match);
    if (regex) g_regex_unref(regex);
    if (!capture) return FALSE;
    g_strdelimit(capture, ",", ' ');
    g_strstrip(capture);
    char *end = NULL;
    double parsed = g_ascii_strtod(capture, &end);
    gboolean parsed_ok = capture[0] && end && *end == '\0' && isfinite(parsed);
    g_free(capture);
    if (!parsed_ok) return FALSE;
    *balance = billing_response ? parsed / OPENCODE_GO_BILLING_SCALE : parsed;
    return TRUE;
}

static void opencode_go_attach_balance(CodexBarProvider *provider, double balance, gint64 now_ms) {
    if (!provider || !isfinite(balance)) return;
    codexbar_provider_cost_free(provider->provider_cost);
    provider->provider_cost = g_new0(CodexBarProviderCost, 1);
    provider->provider_cost->used = balance;
    provider->provider_cost->currency = g_strdup("USD");
    provider->provider_cost->period = g_strdup("Zen balance");
    provider->provider_cost->has_updated_at = TRUE;
    provider->provider_cost->updated_at_ms = now_ms;
}

static gboolean opencode_go_response_balance(CodexBarHttpResponse *response,
                                             gboolean billing,
                                             double *balance) {
    return response && response->status == 200 &&
           codexbar_opencode_go_parse_zen_balance(response->body, response->body_length, billing, balance);
}

static gboolean opencode_go_fetch_balance(const char *workspace,
                                          const char *cookie,
                                          CodexBarOpenCodeGoTransport transport,
                                          GCancellable *cancellable,
                                          double *balance) {
    char *url = g_strdup_printf("https://opencode.ai/workspace/%s", workspace);
    const CodexBarHttpRequestHeader page_headers[] = {
        {"Cookie", cookie},
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/143.0.0.0 Safari/537.36"},
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
    };
    CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = page_headers,
        .header_count = G_N_ELEMENTS(page_headers),
        .timeout_seconds = 5,
        .maximum_response_bytes = WEB_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = opencode_go_send(&request, transport, NULL);
    gboolean found = opencode_go_response_balance(response, FALSE, balance);
    codexbar_http_response_free(response);
    g_free(url);
    if (found || (cancellable && g_cancellable_is_cancelled(cancellable))) return found;

    char *escaped = g_uri_escape_string(workspace, NULL, TRUE);
    url = g_strdup_printf("https://opencode.ai/_server?id=%s&args=%%5B%%22%s%%22%%5D",
                          OPENCODE_GO_BILLING_SERVER_ID,
                          escaped);
    g_free(escaped);
    char *instance = g_uuid_string_random();
    char *server_instance = g_strconcat("server-fn:", instance, NULL);
    g_free(instance);
    const CodexBarHttpRequestHeader billing_headers[] = {
        {"Cookie", cookie},
        {"X-Server-Id", OPENCODE_GO_BILLING_SERVER_ID},
        {"X-Server-Instance", server_instance},
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/143.0.0.0 Safari/537.36"},
        {"Origin", "https://opencode.ai"},
        {"Referer", "https://opencode.ai"},
        {"Accept", "text/javascript, application/json;q=0.9, */*;q=0.8"},
    };
    request = (CodexBarHttpRequest){
        .url = url,
        .method = "GET",
        .headers = billing_headers,
        .header_count = G_N_ELEMENTS(billing_headers),
        .timeout_seconds = 5,
        .maximum_response_bytes = WEB_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    response = opencode_go_send(&request, transport, NULL);
    found = opencode_go_response_balance(response, TRUE, balance);
    codexbar_http_response_free(response);
    g_free(server_instance);
    g_free(url);
    return found;
}

static CodexBarProvider *opencode_go_fetch_web(const CodexBarProviderConfig *config,
                                               CodexBarOpenCodeGoTransport transport,
                                               GCancellable *cancellable,
                                               gint64 now_ms,
                                               GError **error) {
    char *cookie = opencode_go_cookie(config);
    if (!cookie) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "OpenCode Go web session is missing; configure cookieHeader");
        return NULL;
    }
    char *workspace = opencode_go_workspace(config);
    if (!workspace) workspace = opencode_go_discover_workspace(cookie, transport, cancellable, error);
    if (!workspace) {
        g_free(cookie);
        return NULL;
    }
    char *url = g_strdup_printf("https://opencode.ai/workspace/%s/go", workspace);
    const CodexBarHttpRequestHeader headers[] = {
        {"Cookie", cookie},
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/143.0.0.0 Safari/537.36"},
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 15,
        .maximum_response_bytes = WEB_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = opencode_go_send(&request, transport, error);
    g_free(url);
    if (!response) {
        g_free(workspace);
        g_free(cookie);
        return NULL;
    }
    if (response->status != 200) {
        g_set_error(error,
                    G_IO_ERROR,
                    response->status == 401 || response->status == 403 ? G_IO_ERROR_PERMISSION_DENIED
                                                                       : G_IO_ERROR_FAILED,
                    "OpenCode Go usage page returned HTTP %ld",
                    response->status);
        codexbar_http_response_free(response);
        g_free(workspace);
        g_free(cookie);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_opencode_go_parse_web_usage(
        response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    double balance = 0;
    gboolean has_balance = opencode_go_fetch_balance(
        workspace, cookie, transport, cancellable, &balance);
    if (provider && has_balance) opencode_go_attach_balance(provider, balance, now_ms);
    if (!provider && has_balance && error && *error &&
        g_error_matches(*error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA)) {
        g_clear_error(error);
        provider = codexbar_provider_new();
        provider->provider = g_strdup("opencodego");
        provider->source = g_strdup("web");
        provider->dashboard_url = g_strdup("https://opencode.ai");
        provider->has_updated_at = TRUE;
        provider->updated_at_ms = now_ms;
        provider->explicit_quota_slots = TRUE;
        opencode_go_attach_balance(provider, balance, now_ms);
    }
    g_free(workspace);
    g_free(cookie);
    return provider;
}

CodexBarProvider *codexbar_opencode_go_fetch_for_source_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    CodexBarOpenCodeGoTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    const char *mode = source && source[0] ? source : "auto";
    if (g_str_equal(mode, "web")) return opencode_go_fetch_web(config, transport, cancellable, now_ms, error);
    if (!g_str_equal(mode, "auto")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "OpenCode Go source '%s' is unsupported", mode);
        return NULL;
    }
    char *cookie = opencode_go_cookie(config);
    if (cookie) {
        g_free(cookie);
        GError *web_error = NULL;
        CodexBarProvider *web = opencode_go_fetch_web(config, transport, cancellable, now_ms, &web_error);
        if (web) return web;
        if (web_error && g_error_matches(web_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            if (error) *error = web_error;
            else g_clear_error(&web_error);
            return NULL;
        }
        GError *local_error = NULL;
        CodexBarProvider *local = codexbar_opencode_go_fetch_from_home(g_get_home_dir(), now_ms, &local_error);
        if (local) {
            g_clear_error(&web_error);
            g_clear_error(&local_error);
            return local;
        }
        g_clear_error(&local_error);
        if (error) *error = web_error;
        else g_clear_error(&web_error);
        return NULL;
    }
    GError *local_error = NULL;
    CodexBarProvider *provider = codexbar_opencode_go_fetch_from_home(g_get_home_dir(), now_ms, &local_error);
    if (provider) g_clear_error(&local_error);
    else if (error) *error = local_error;
    else g_clear_error(&local_error);
    return provider;
}

CodexBarProvider *codexbar_opencode_go_fetch_for_source_with_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    GCancellable *cancellable,
    GError **error) {
    return codexbar_opencode_go_fetch_for_source_with_transport_and_cancellable(
        config, source, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_opencode_go_fetch_from_home(
    const char *home_directory, gint64 now_ms, GError **error) {
    g_return_val_if_fail(home_directory != NULL, NULL);
    char *directory = g_build_filename(home_directory, ".local", "share", "opencode", NULL);
    char *auth_path = g_build_filename(directory, "auth.json", NULL);
    char *database_path = g_build_filename(directory, "opencode.db", NULL);
    gboolean has_auth = has_auth_key(auth_path);
    if (!g_file_test(database_path, G_FILE_TEST_EXISTS)) {
        if (has_auth) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_NOT_FOUND,
                                "OpenCode Go local usage history is unavailable: database not found");
        } else {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_NOT_FOUND,
                                "OpenCode Go not detected. Log in with OpenCode Go or use it locally first.");
        }
        g_free(database_path);
        g_free(auth_path);
        g_free(directory);
        return NULL;
    }

    GArray *rows = read_rows(database_path, error);
    g_free(database_path);
    g_free(auth_path);
    g_free(directory);
    if (!rows) return NULL;
    if (rows->len == 0) {
        if (has_auth) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_NOT_FOUND,
                                "OpenCode Go local usage history is unavailable: no local usage rows");
        } else {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_NOT_FOUND,
                                "OpenCode Go not detected. Log in with OpenCode Go or use it locally first.");
        }
        g_array_unref(rows);
        return NULL;
    }

    gint64 earliest_ms = G_MAXINT64;
    for (guint index = 0; index < rows->len; index++) {
        UsageRow row = g_array_index(rows, UsageRow, index);
        earliest_ms = MIN(earliest_ms, row.created_ms);
    }
    gint64 week_start_ms = utc_week_start_ms(now_ms);
    MonthBounds month = month_bounds(now_ms, earliest_ms);
    double rolling = sum_rows(rows, now_ms - FIVE_HOURS_MILLISECONDS, now_ms);
    double weekly = sum_rows(rows, week_start_ms, week_start_ms + WEEK_MILLISECONDS);
    double monthly = sum_rows(rows, month.start_ms, month.end_ms);

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("opencodego");
    provider->source = g_strdup("local");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    codexbar_provider_add_quota_window(
        provider,
        make_window("primary", "5-hour", used_percent(rolling, 12), 5 * 60,
                    rolling_reset_seconds(rows, now_ms), now_ms));
    codexbar_provider_add_quota_window(
        provider,
        make_window("secondary", "Weekly", used_percent(weekly, 30), 7 * 24 * 60,
                    MAX(0, (week_start_ms + WEEK_MILLISECONDS - now_ms) / 1000), now_ms));
    codexbar_provider_add_quota_window(
        provider,
        make_window("tertiary", "Monthly", used_percent(monthly, 60), 30 * 24 * 60,
                    MAX(0, (month.end_ms - now_ms) / 1000), now_ms));
    g_array_unref(rows);
    return provider;
}

CodexBarProvider *codexbar_opencode_go_fetch(GError **error) {
    return codexbar_opencode_go_fetch_from_home(g_get_home_dir(), g_get_real_time() / 1000, error);
}
