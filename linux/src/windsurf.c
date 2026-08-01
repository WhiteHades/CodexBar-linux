#include "windsurf.h"

#include <json-c/json.h>
#include <math.h>
#include <sqlite3.h>
#include <string.h>

#define RESPONSE_LIMIT (1024U * 1024U)
#define CREDENTIAL_LIMIT 16384U

typedef struct {
    const guint8 *data;
    size_t length;
    size_t offset;
} ProtoReader;

typedef struct {
    char *session_token;
    char *auth1_token;
    char *account_id;
    char *primary_org_id;
} Session;

typedef struct {
    char *plan_name;
    gboolean has_plan_start;
    gint64 plan_start_ms;
    gboolean has_plan_end;
    gint64 plan_end_ms;
    gboolean has_daily_remaining;
    guint64 daily_remaining;
    gboolean has_weekly_remaining;
    guint64 weekly_remaining;
    gboolean has_daily_reset;
    gint64 daily_reset_ms;
    gboolean has_weekly_reset;
    gint64 weekly_reset_ms;
} PlanStatus;

static gboolean read_varint(ProtoReader *reader, guint64 *result) {
    guint64 value = 0;
    for (guint byte_index = 0; byte_index < 10 && reader->offset < reader->length; byte_index++) {
        guint8 byte = reader->data[reader->offset++];
        if (byte_index == 9 && byte > 1) return FALSE;
        value |= (guint64)(byte & 0x7f) << (byte_index * 7);
        if (!(byte & 0x80)) {
            *result = value;
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean read_bytes(ProtoReader *reader, const guint8 **data, size_t *length) {
    guint64 raw_length = 0;
    if (!read_varint(reader, &raw_length) || raw_length > SIZE_MAX ||
        (size_t)raw_length > reader->length - reader->offset) return FALSE;
    *data = reader->data + reader->offset;
    *length = (size_t)raw_length;
    reader->offset += (size_t)raw_length;
    return TRUE;
}

static gboolean next_field(ProtoReader *reader, guint *field, guint *wire) {
    guint64 key = 0;
    if (!read_varint(reader, &key) || !(key >> 3) || (key & 7) > 5) return FALSE;
    *field = (guint)(key >> 3);
    *wire = (guint)(key & 7);
    return TRUE;
}

static gboolean skip_field(ProtoReader *reader, guint wire) {
    guint64 ignored = 0;
    const guint8 *bytes = NULL;
    size_t length = 0;
    switch (wire) {
    case 0:
        return read_varint(reader, &ignored);
    case 1:
        if (reader->length - reader->offset < 8) return FALSE;
        reader->offset += 8;
        return TRUE;
    case 2:
        return read_bytes(reader, &bytes, &length);
    case 5:
        if (reader->length - reader->offset < 4) return FALSE;
        reader->offset += 4;
        return TRUE;
    default:
        return FALSE;
    }
}

static char *read_string(ProtoReader *reader) {
    const guint8 *data = NULL;
    size_t length = 0;
    if (!read_bytes(reader, &data, &length) || length > CREDENTIAL_LIMIT ||
        memchr(data, '\0', length) || !g_utf8_validate((const char *)data, (gssize)length, NULL)) return NULL;
    return g_strndup((const char *)data, length);
}

static gboolean decode_timestamp(const guint8 *data, size_t length, gint64 *milliseconds) {
    ProtoReader reader = {data, length, 0};
    gint64 seconds = 0;
    gint32 nanos = 0;
    while (reader.offset < reader.length) {
        guint field = 0;
        guint wire = 0;
        guint64 value = 0;
        if (!next_field(&reader, &field, &wire)) return FALSE;
        if (field == 1 && wire == 0) {
            if (!read_varint(&reader, &value)) return FALSE;
            seconds = (gint64)value;
        } else if (field == 2 && wire == 0) {
            if (!read_varint(&reader, &value) || value > G_MAXINT32) return FALSE;
            nanos = (gint32)value;
        } else if (!skip_field(&reader, wire)) {
            return FALSE;
        }
    }
    if (seconds > G_MAXINT64 / 1000 || seconds < G_MININT64 / 1000 || nanos < 0 || nanos >= 1000000000) {
        return FALSE;
    }
    *milliseconds = seconds * 1000 + nanos / 1000000;
    return TRUE;
}

static gboolean decode_plan_info(const guint8 *data, size_t length, PlanStatus *status) {
    ProtoReader reader = {data, length, 0};
    while (reader.offset < reader.length) {
        guint field = 0;
        guint wire = 0;
        if (!next_field(&reader, &field, &wire)) return FALSE;
        if (field == 2 && wire == 2) {
            char *name = read_string(&reader);
            if (!name) return FALSE;
            g_free(status->plan_name);
            status->plan_name = name;
        } else if (!skip_field(&reader, wire)) {
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean decode_plan_status(const guint8 *data, size_t length, PlanStatus *status) {
    ProtoReader reader = {data, length, 0};
    while (reader.offset < reader.length) {
        guint field = 0;
        guint wire = 0;
        guint64 value = 0;
        const guint8 *nested = NULL;
        size_t nested_length = 0;
        if (!next_field(&reader, &field, &wire)) return FALSE;
        if ((field == 1 || field == 2 || field == 3) && wire == 2) {
            if (!read_bytes(&reader, &nested, &nested_length)) return FALSE;
            if (field == 1 && !decode_plan_info(nested, nested_length, status)) return FALSE;
            if (field == 2) {
                status->has_plan_start = decode_timestamp(nested, nested_length, &status->plan_start_ms);
                if (!status->has_plan_start) return FALSE;
            }
            if (field == 3) {
                status->has_plan_end = decode_timestamp(nested, nested_length, &status->plan_end_ms);
                if (!status->has_plan_end) return FALSE;
            }
        } else if ((field == 14 || field == 15 || field == 17 || field == 18) && wire == 0) {
            if (!read_varint(&reader, &value)) return FALSE;
            if (field == 14) {
                status->has_daily_remaining = TRUE;
                status->daily_remaining = value;
            } else if (field == 15) {
                status->has_weekly_remaining = TRUE;
                status->weekly_remaining = value;
            } else if (value <= G_MAXINT64 / 1000) {
                if (field == 17) {
                    status->has_daily_reset = TRUE;
                    status->daily_reset_ms = (gint64)value * 1000;
                } else {
                    status->has_weekly_reset = TRUE;
                    status->weekly_reset_ms = (gint64)value * 1000;
                }
            } else {
                return FALSE;
            }
        } else if (!skip_field(&reader, wire)) {
            return FALSE;
        }
    }
    return TRUE;
}

static char *reset_description(gint64 reset_ms, gint64 now_ms) {
    gint64 seconds = (reset_ms - now_ms) / 1000;
    if (seconds <= 0) return g_strdup("Expired");
    gint64 hours = seconds / 3600;
    gint64 minutes = seconds % 3600 / 60;
    if (hours >= 24) return g_strdup_printf("Resets in %" G_GINT64_FORMAT "d %" G_GINT64_FORMAT "h",
                                             hours / 24, hours % 24);
    if (hours > 0) return g_strdup_printf("Resets in %" G_GINT64_FORMAT "h %" G_GINT64_FORMAT "m",
                                           hours, minutes);
    return g_strdup_printf("Resets in %" G_GINT64_FORMAT "m", minutes);
}

static void add_window(CodexBarProvider *provider,
                       const char *id,
                       const char *title,
                       double remaining,
                       gboolean has_reset,
                       gint64 reset_ms,
                       gint64 now_ms) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = 100 - CLAMP(remaining, 0.0, 100.0);
    window->has_resets_at = has_reset;
    window->resets_at_ms = reset_ms;
    if (has_reset) window->reset_description = reset_description(reset_ms, now_ms);
    codexbar_provider_add_quota_window(provider, window);
}

static gboolean json_number(json_object *object, const char *key, double *result) {
    json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value) ||
        !(json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double))) return FALSE;
    double number = json_object_get_double(value);
    if (!isfinite(number)) return FALSE;
    *result = number;
    return TRUE;
}

static gboolean json_integer(json_object *object, const char *key, gint64 *result) {
    double number = 0;
    if (!json_number(object, key, &number) || number < (double)G_MININT64 || number > (double)G_MAXINT64 ||
        trunc(number) != number) return FALSE;
    *result = (gint64)number;
    return TRUE;
}

static char *cached_string(json_object *object, const char *key) {
    json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value) ||
        !json_object_is_type(value, json_type_string)) return NULL;
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!text || memchr(text, '\0', length) || !g_utf8_validate(text, (gssize)length, NULL)) return NULL;
    return g_strndup(text, length);
}

static json_object *parse_cached_json(const char *text, size_t length) {
    if (!text || !length || length > RESPONSE_LIMIT || length > G_MAXINT || memchr(text, '\0', length) ||
        !g_utf8_validate(text, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, text, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && g_ascii_isspace(text[consumed])) consumed++;
    gboolean valid = parse_error == json_tokener_success && root && consumed == length &&
                     json_object_is_type(root, json_type_object);
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static void add_count_window(CodexBarProvider *provider,
                             const char *id,
                             const char *title,
                             json_object *usage,
                             const char *total_key,
                             const char *used_key,
                             const char *remaining_key,
                             const char *unit) {
    gint64 total = 0;
    gint64 used = 0;
    gint64 remaining = 0;
    if (!json_integer(usage, total_key, &total) || total <= 0) return;
    if (!json_integer(usage, used_key, &used)) {
        if (!json_integer(usage, remaining_key, &remaining)) return;
        used = MAX((gint64)0, total - remaining);
    }
    used = CLAMP(used, (gint64)0, total);
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = (double)used / (double)total * 100.0;
    window->reset_description = g_strdup_printf("%" G_GINT64_FORMAT " / %" G_GINT64_FORMAT " %s",
                                                 used, total, unit);
    codexbar_provider_add_quota_window(provider, window);
}

CodexBarProvider *codexbar_windsurf_parse_cached_plan(const char *json,
                                                      size_t length,
                                                      gint64 now_ms,
                                                      GError **error) {
    json_object *root = parse_cached_json(json, length);
    if (!root) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Could not parse Windsurf cached plan data");
        return NULL;
    }
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("windsurf");
    provider->source = g_strdup("local");
    provider->plan = cached_string(root, "planName");
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = g_strdup(provider->plan);
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    gint64 end_ms = 0;
    if (json_integer(root, "endTimestamp", &end_ms) && end_ms > 0) {
        GDateTime *end = g_date_time_new_from_unix_local(end_ms / 1000);
        if (end) {
            char *date = g_date_time_format(end, "%b %-d, %Y");
            provider->identity->organization = g_strdup_printf("Expires %s", date);
            g_free(date);
            g_date_time_unref(end);
        }
        provider->has_subscription_expires_at = TRUE;
        provider->subscription_expires_at_ms = end_ms;
    }

    json_object *quota = NULL;
    gboolean daily = FALSE;
    gboolean weekly = FALSE;
    if (json_object_object_get_ex(root, "quotaUsage", &quota) && json_object_is_type(quota, json_type_object)) {
        double remaining = 0;
        gint64 reset = 0;
        if (json_number(quota, "dailyRemainingPercent", &remaining)) {
            gboolean has_reset = json_integer(quota, "dailyResetAtUnix", &reset) && reset > 0 &&
                                 reset <= G_MAXINT64 / 1000;
            add_window(provider, "windsurf.daily", "Daily", remaining,
                       has_reset, has_reset ? reset * 1000 : 0, now_ms);
            daily = TRUE;
        }
        if (json_number(quota, "weeklyRemainingPercent", &remaining)) {
            gboolean has_reset = json_integer(quota, "weeklyResetAtUnix", &reset) && reset > 0 &&
                                 reset <= G_MAXINT64 / 1000;
            add_window(provider, "windsurf.weekly", "Weekly", remaining,
                       has_reset, has_reset ? reset * 1000 : 0, now_ms);
            weekly = TRUE;
        }
    }
    json_object *usage = NULL;
    if (json_object_object_get_ex(root, "usage", &usage) && json_object_is_type(usage, json_type_object)) {
        if (!daily) add_count_window(provider, "windsurf.messages", "Messages", usage,
                                     "messages", "usedMessages", "remainingMessages", "messages");
        if (!weekly) add_count_window(provider, "windsurf.flow-actions", "Flow actions", usage,
                                      "flowActions", "usedFlowActions", "remainingFlowActions", "flow actions");
    }
    json_object_put(root);
    return provider;
}

static char *decode_cached_database_value(sqlite3_stmt *statement) {
    int type = sqlite3_column_type(statement, 0);
    if (type != SQLITE_TEXT && type != SQLITE_BLOB) return NULL;
    const guint8 *bytes = sqlite3_column_blob(statement, 0);
    int raw_length = sqlite3_column_bytes(statement, 0);
    if (!bytes || raw_length <= 0 || raw_length > (int)RESPONSE_LIMIT) return NULL;
    size_t length = (size_t)raw_length;
    while (length > 0 && (bytes[length - 1] < 32 || bytes[length - 1] == 127)) length--;
    if (length > 0 && !memchr(bytes, '\0', length) && g_utf8_validate((const char *)bytes, (gssize)length, NULL)) {
        return g_strndup((const char *)bytes, length);
    }
    gsize converted_length = 0;
    GError *conversion_error = NULL;
    char *converted = g_convert((const char *)bytes,
                                raw_length,
                                "UTF-8",
                                "UTF-16LE",
                                NULL,
                                &converted_length,
                                &conversion_error);
    g_clear_error(&conversion_error);
    if (!converted || converted_length > RESPONSE_LIMIT || memchr(converted, '\0', converted_length) ||
        !g_utf8_validate(converted, (gssize)converted_length, NULL)) {
        g_free(converted);
        return NULL;
    }
    while (converted_length > 0 &&
           ((guint8)converted[converted_length - 1] < 32 || (guint8)converted[converted_length - 1] == 127)) {
        converted[--converted_length] = '\0';
    }
    if (converted_length > 0) return converted;
    g_free(converted);
    return NULL;
}

CodexBarProvider *codexbar_windsurf_fetch_local(const char *database_path,
                                                gint64 now_ms,
                                                GError **error) {
    if (!database_path || !database_path[0]) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Windsurf database path is missing");
        return NULL;
    }
    sqlite3 *database = NULL;
    if (sqlite3_open_v2(database_path, &database, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        const char *message = database ? sqlite3_errmsg(database) : "unknown error";
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                    "Could not read Windsurf database '%s': %s", database_path, message);
        sqlite3_close(database);
        return NULL;
    }
    sqlite3_busy_timeout(database, 250);
    sqlite3_stmt *statement = NULL;
    const char *query =
        "SELECT value FROM ItemTable WHERE key = 'windsurf.settings.cachedPlanInfo' LIMIT 1;";
    int status = sqlite3_prepare_v2(database, query, -1, &statement, NULL);
    if (status == SQLITE_OK) status = sqlite3_step(statement);
    if (status != SQLITE_ROW) {
        const char *message = status == SQLITE_DONE ? "cached plan entry was not found" : sqlite3_errmsg(database);
        g_set_error(error, G_IO_ERROR, status == SQLITE_DONE ? G_IO_ERROR_NOT_FOUND : G_IO_ERROR_FAILED,
                    "Could not read Windsurf cached plan: %s", message);
        sqlite3_finalize(statement);
        sqlite3_close(database);
        return NULL;
    }
    char *json = decode_cached_database_value(statement);
    sqlite3_finalize(statement);
    sqlite3_close(database);
    if (!json) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Windsurf cached plan is not valid UTF-8 or UTF-16LE JSON");
        return NULL;
    }
    CodexBarProvider *provider = codexbar_windsurf_parse_cached_plan(json, strlen(json), now_ms, error);
    g_free(json);
    return provider;
}

CodexBarProvider *codexbar_windsurf_parse(const guint8 *data,
                                          size_t length,
                                          gint64 now_ms,
                                          GError **error) {
    if (!data || !length || length > RESPONSE_LIMIT) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Windsurf returned an empty or oversized protobuf response");
        return NULL;
    }
    ProtoReader reader = {data, length, 0};
    PlanStatus status = {0};
    gboolean has_status = FALSE;
    while (reader.offset < reader.length) {
        guint field = 0;
        guint wire = 0;
        if (!next_field(&reader, &field, &wire)) goto malformed;
        if (field == 1 && wire == 2) {
            const guint8 *nested = NULL;
            size_t nested_length = 0;
            g_free(status.plan_name);
            status = (PlanStatus){0};
            if (!read_bytes(&reader, &nested, &nested_length) ||
                !decode_plan_status(nested, nested_length, &status)) goto malformed;
            has_status = TRUE;
        } else if (!skip_field(&reader, wire)) {
            goto malformed;
        }
    }
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("windsurf");
    provider->source = g_strdup("web");
    provider->plan = g_strdup(status.plan_name);
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = g_strdup(status.plan_name);
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    if (has_status && status.has_plan_end) {
        GDateTime *end = g_date_time_new_from_unix_utc(status.plan_end_ms / 1000);
        char *date = g_date_time_format(end, "%Y-%m-%d");
        provider->identity->organization = g_strdup_printf("Expires %s", date);
        provider->has_subscription_expires_at = TRUE;
        provider->subscription_expires_at_ms = status.plan_end_ms;
        g_free(date);
        g_date_time_unref(end);
    }
    if (has_status && status.has_daily_remaining) {
        add_window(provider, "windsurf.daily", "Daily", status.daily_remaining,
                   status.has_daily_reset, status.daily_reset_ms, now_ms);
    }
    if (has_status && status.has_weekly_remaining) {
        add_window(provider, "windsurf.weekly", "Weekly", status.weekly_remaining,
                   status.has_weekly_reset, status.weekly_reset_ms, now_ms);
    }
    g_free(status.plan_name);
    return provider;

malformed:
    g_free(status.plan_name);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Could not parse Windsurf plan status protobuf");
    return NULL;
}

static void session_clear(Session *session) {
    g_free(session->session_token);
    g_free(session->auth1_token);
    g_free(session->account_id);
    g_free(session->primary_org_id);
    *session = (Session){0};
}

static char *clean_value(const char *raw) {
    if (!raw || strlen(raw) > CREDENTIAL_LIMIT || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *copy = g_strstrip(g_strdup(raw));
    size_t length = strlen(copy);
    if (length >= 2 && ((copy[0] == '"' && copy[length - 1] == '"') ||
                        (copy[0] == '\'' && copy[length - 1] == '\''))) {
        copy[length - 1] = '\0';
        memmove(copy, copy + 1, length - 1);
        g_strstrip(copy);
    }
    for (const unsigned char *cursor = (const unsigned char *)copy; *cursor; cursor++) {
        if (g_ascii_iscntrl(*cursor)) {
            g_free(copy);
            return NULL;
        }
    }
    if (copy[0]) return copy;
    g_free(copy);
    return NULL;
}

static json_object *parse_json(const char *text, size_t length) {
    if (!text || !length || length > CREDENTIAL_LIMIT || length > G_MAXINT ||
        memchr(text, '\0', length) || !g_utf8_validate(text, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, text, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && strchr(" \t\r\n", text[consumed])) consumed++;
    gboolean valid = parse_error == json_tokener_success && root && consumed == length &&
                     json_object_is_type(root, json_type_object);
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static char *json_alias(json_object *root, const char *const *keys) {
    for (size_t index = 0; keys[index]; index++) {
        json_object *value = NULL;
        if (!json_object_object_get_ex(root, keys[index], &value) ||
            !json_object_is_type(value, json_type_string)) continue;
        const char *text = json_object_get_string(value);
        size_t length = (size_t)json_object_get_string_len(value);
        if (memchr(text, '\0', length) || !g_utf8_validate(text, (gssize)length, NULL)) continue;
        char *copy = g_strndup(text, length);
        char *clean = clean_value(copy);
        g_free(copy);
        if (clean) return clean;
    }
    return NULL;
}

static void parse_object_session(json_object *root, Session *session) {
    const char *session_keys[] = {"devin_session_token", "devinSessionToken", "sessionToken", NULL};
    const char *auth_keys[] = {"devin_auth1_token", "devinAuth1Token", "auth1Token", NULL};
    const char *account_keys[] = {"devin_account_id", "devinAccountId", "accountID", "accountId", NULL};
    const char *org_keys[] = {"devin_primary_org_id", "devinPrimaryOrgId", "primaryOrgID", "primaryOrgId", NULL};
    session->session_token = json_alias(root, session_keys);
    session->auth1_token = json_alias(root, auth_keys);
    session->account_id = json_alias(root, account_keys);
    session->primary_org_id = json_alias(root, org_keys);
}

static void parse_key_value_session(const char *raw, Session *session) {
    json_object *values = json_object_new_object();
    char *copy = g_strstrip(g_strdup(raw));
    size_t copy_length = strlen(copy);
    if (copy_length >= 2 && copy[0] == '{' && copy[copy_length - 1] == '}') {
        copy[copy_length - 1] = '\0';
        memmove(copy, copy + 1, copy_length - 1);
    }
    char **segments = g_strsplit_set(copy, "\n,;", -1);
    for (size_t index = 0; segments[index]; index++) {
        char *segment = g_strstrip(segments[index]);
        while (*segment == '{' || *segment == '}') segment++;
        char *delimiter = strchr(segment, '=');
        if (!delimiter) delimiter = strchr(segment, ':');
        if (!delimiter) continue;
        *delimiter = '\0';
        char *key = g_strstrip(segment);
        char *value = clean_value(delimiter + 1);
        if (key[0] && value) json_object_object_add(values, key, json_object_new_string(value));
        g_free(value);
    }
    parse_object_session(values, session);
    json_object_put(values);
    g_strfreev(segments);
    g_free(copy);
}

static gboolean parse_session(const CodexBarProviderConfig *config, Session *session, GError **error) {
    json_object *raw = config ? config->raw : NULL;
    if (raw && json_object_is_type(raw, json_type_object)) parse_object_session(raw, session);
    json_object *bundle_value = NULL;
    char *bundle = NULL;
    if (raw && json_object_object_get_ex(raw, "session", &bundle_value) &&
        json_object_is_type(bundle_value, json_type_string)) {
        const char *text = json_object_get_string(bundle_value);
        size_t length = (size_t)json_object_get_string_len(bundle_value);
        if (!memchr(text, '\0', length) && g_utf8_validate(text, (gssize)length, NULL)) bundle = g_strndup(text, length);
    }
    if (!bundle && raw && json_object_object_get_ex(raw, "cookieHeader", &bundle_value) &&
        json_object_is_type(bundle_value, json_type_string)) {
        const char *text = json_object_get_string(bundle_value);
        size_t length = (size_t)json_object_get_string_len(bundle_value);
        if (!memchr(text, '\0', length) && g_utf8_validate(text, (gssize)length, NULL)) bundle = g_strndup(text, length);
    }
    if (!bundle) bundle = g_strdup(g_getenv("WINDSURF_SESSION"));
    if ((!session->session_token || !session->auth1_token || !session->account_id ||
         !session->primary_org_id) && bundle) {
        session_clear(session);
        json_object *object = parse_json(bundle, strlen(bundle));
        if (object) {
            parse_object_session(object, session);
            json_object_put(object);
        } else {
            parse_key_value_session(bundle, session);
        }
    }
    if (session->session_token && session->auth1_token && session->account_id && session->primary_org_id) {
        g_free(bundle);
        return TRUE;
    }
    g_free(bundle);
    session_clear(session);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "Windsurf requires a complete Devin session bundle");
    return FALSE;
}

static void append_varint(GByteArray *body, guint64 value) {
    while (value >= 0x80) {
        guint8 byte = (guint8)((value & 0x7f) | 0x80);
        g_byte_array_append(body, &byte, 1);
        value >>= 7;
    }
    guint8 byte = (guint8)value;
    g_byte_array_append(body, &byte, 1);
}

static GByteArray *request_body(const char *session_token) {
    GByteArray *body = g_byte_array_new();
    append_varint(body, 1 << 3 | 2);
    append_varint(body, strlen(session_token));
    g_byte_array_append(body, (const guint8 *)session_token, strlen(session_token));
    append_varint(body, 2 << 3);
    append_varint(body, 1);
    return body;
}

static gboolean same_origin(const CodexBarHttpResponse *response, const char *url) {
    if (!response || !response->effective_url) return TRUE;
    GUri *expected = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
    GUri *actual = g_uri_parse(response->effective_url, G_URI_FLAGS_NONE, NULL);
    const char *expected_scheme = expected ? g_uri_get_scheme(expected) : NULL;
    const char *actual_scheme = actual ? g_uri_get_scheme(actual) : NULL;
    const char *expected_host = expected ? g_uri_get_host(expected) : NULL;
    const char *actual_host = actual ? g_uri_get_host(actual) : NULL;
    gboolean same = expected_scheme && actual_scheme && expected_host && actual_host &&
                    g_ascii_strcasecmp(actual_scheme, "https") == 0 &&
                    g_ascii_strcasecmp(expected_scheme, actual_scheme) == 0 &&
                    g_ascii_strcasecmp(expected_host, actual_host) == 0 &&
                    g_uri_get_port(expected) == g_uri_get_port(actual);
    if (expected) g_uri_unref(expected);
    if (actual) g_uri_unref(actual);
    return same;
}

CodexBarProvider *codexbar_windsurf_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWindsurfTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    Session session = {0};
    if (!parse_session(config, &session, error)) return NULL;
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) {
        session_clear(&session);
        return NULL;
    }
    if (!transport) {
        session_clear(&session);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Windsurf transport is missing");
        return NULL;
    }
    const char *url =
        "https://windsurf.com/_backend/exa.seat_management_pb.SeatManagementService/GetPlanStatus";
    CodexBarHttpRequestHeader headers[] = {
        {"Content-Type", "application/proto"},
        {"Connect-Protocol-Version", "1"},
        {"Origin", "https://windsurf.com"},
        {"Referer", "https://windsurf.com/profile"},
        {"x-auth-token", session.session_token},
        {"x-devin-session-token", session.session_token},
        {"x-devin-auth1-token", session.auth1_token},
        {"x-devin-account-id", session.account_id},
        {"x-devin-primary-org-id", session.primary_org_id},
    };
    GByteArray *body = request_body(session.session_token);
    CodexBarHttpRequest request = {
        .url = url,
        .method = "POST",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .body = body->data,
        .body_length = body->len,
        .timeout_seconds = 15,
        .maximum_response_bytes = RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = transport(&request, error);
    g_byte_array_unref(body);
    session_clear(&session);
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        codexbar_http_response_free(response);
        if (error && *error) g_clear_error(error);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        return NULL;
    }
    if (!response) return NULL;
    if (!same_origin(response, url)) {
        codexbar_http_response_free(response);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Windsurf redirected outside its trusted origin");
        return NULL;
    }
    if (response->status != 200) {
        long status = response->status;
        codexbar_http_response_free(response);
        g_set_error(error, G_IO_ERROR,
                    status == 400 || status == 401 || status == 403
                        ? G_IO_ERROR_PERMISSION_DENIED
                        : G_IO_ERROR_FAILED,
                    "Windsurf API returned HTTP %ld", status);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_windsurf_parse(
        (const guint8 *)response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

CodexBarProvider *codexbar_windsurf_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                           GCancellable *cancellable,
                                                           GError **error) {
    return codexbar_windsurf_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

static char *windsurf_database_path(const CodexBarProviderConfig *config) {
    char *path = NULL;
    json_object *value = NULL;
    if (config && config->raw && json_object_object_get_ex(config->raw, "dbPath", &value) &&
        json_object_is_type(value, json_type_string)) {
        path = clean_value(json_object_get_string(value));
    }
    if (!path) path = clean_value(g_getenv("WINDSURF_STATE_DB"));
    if (!path) path = g_build_filename(
        g_get_user_config_dir(), "Windsurf", "User", "globalStorage", "state.vscdb", NULL);
    return path;
}

static CodexBarProvider *windsurf_fetch_local_config(const CodexBarProviderConfig *config,
                                                     gint64 now_ms,
                                                     GError **error) {
    char *path = windsurf_database_path(config);
    CodexBarProvider *provider = codexbar_windsurf_fetch_local(path, now_ms, error);
    g_free(path);
    return provider;
}

CodexBarProvider *codexbar_windsurf_fetch_for_source_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    CodexBarWindsurfTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    const char *mode = source && source[0] ? source : "auto";
    if (g_str_equal(mode, "web")) {
        return codexbar_windsurf_fetch_with_transport_and_cancellable(
            config, transport, cancellable, now_ms, error);
    }
    if (g_str_equal(mode, "cli")) return windsurf_fetch_local_config(config, now_ms, error);
    if (!g_str_equal(mode, "auto")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Windsurf source '%s' is unsupported", mode);
        return NULL;
    }
    GError *web_error = NULL;
    CodexBarProvider *provider = codexbar_windsurf_fetch_with_transport_and_cancellable(
        config, transport, cancellable, now_ms, &web_error);
    if (provider) return provider;
    if (web_error && g_error_matches(web_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        if (error) *error = web_error;
        else g_clear_error(&web_error);
        return NULL;
    }
    g_clear_error(&web_error);
    return windsurf_fetch_local_config(config, now_ms, error);
}

CodexBarProvider *codexbar_windsurf_fetch_for_source_with_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    GCancellable *cancellable,
    GError **error) {
    return codexbar_windsurf_fetch_for_source_with_transport_and_cancellable(
        config, source, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}
