#include "opencode_go.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <sqlite3.h>

#define FIVE_HOURS_MILLISECONDS (5LL * 60LL * 60LL * 1000LL)
#define WEEK_MILLISECONDS (7LL * 24LL * 60LL * 60LL * 1000LL)

typedef struct {
    gint64 created_ms;
    double cost;
} UsageRow;

typedef struct {
    gint64 start_ms;
    gint64 end_ms;
} MonthBounds;

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
    window->has_resets_at = TRUE;
    window->resets_at_ms = now_ms + reset_seconds * 1000;
    return window;
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
