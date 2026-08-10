#include "notion.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define NOTION_BASE_URL "https://app.notion.com"
#define NOTION_MAXIMUM_CREDENTIAL_BYTES (256U * 1024U)
#define NOTION_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)
#define NOTION_MONTHLY_SENTINEL_MINUTES 43200

typedef struct {
    const char *lower_name;
    const char *wire_name;
    const char *default_value;
} NotionHeaderDefinition;

static const NotionHeaderDefinition notion_header_definitions[] = {
    {"accept", "Accept", "*/*"},
    {"accept-language", "Accept-Language", "en-US,en;q=0.9"},
    {"notion-audit-log-platform", "notion-audit-log-platform", NULL},
    {"notion-client-version", "notion-client-version", NULL},
    {"referer", "Referer", "https://app.notion.com/"},
    {"sec-fetch-dest", "Sec-Fetch-Dest", "empty"},
    {"sec-fetch-mode", "Sec-Fetch-Mode", "cors"},
    {"sec-fetch-site", "Sec-Fetch-Site", "same-origin"},
    {"user-agent", "User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                                 "(KHTML, like Gecko) Chrome/143.0.0.0 Safari/537.36"},
    {"x-notion-active-user-header", "x-notion-active-user-header", NULL},
};

typedef struct {
    char *cookie;
    char *values[G_N_ELEMENTS(notion_header_definitions)];
} NotionRequestContext;

typedef struct {
    char *id;
    char *name;
    char *plan_type;
    char *subscription_tier;
} NotionWorkspace;

typedef struct {
    char *user_id;
    char *email;
    char *name;
    GPtrArray *workspaces;
} NotionAccount;

typedef struct {
    gboolean not_applicable;
    gboolean has_rolling;
    gboolean has_rolling_used;
    double rolling_used;
    gboolean has_rolling_limit;
    double rolling_limit;
    char *rolling_token;
    gboolean has_resets_in_seconds;
    double resets_in_seconds;
    gboolean has_billing;
    gboolean has_billing_used;
    double billing_used;
    gboolean has_billing_limit;
    double billing_limit;
    gboolean has_period_end_ms;
    double period_end_ms;
} NotionRateLimitStatus;

static void request_context_clear(NotionRequestContext *context) {
    if (!context) return;
    g_free(context->cookie);
    for (size_t index = 0; index < G_N_ELEMENTS(context->values); index++) g_free(context->values[index]);
    *context = (NotionRequestContext){0};
}

static void workspace_free(gpointer data) {
    NotionWorkspace *workspace = data;
    if (!workspace) return;
    g_free(workspace->id);
    g_free(workspace->name);
    g_free(workspace->plan_type);
    g_free(workspace->subscription_tier);
    g_free(workspace);
}

static void account_clear(NotionAccount *account) {
    if (!account) return;
    g_free(account->user_id);
    g_free(account->email);
    g_free(account->name);
    g_clear_pointer(&account->workspaces, g_ptr_array_unref);
    *account = (NotionAccount){0};
}

static void rate_limit_status_clear(NotionRateLimitStatus *status) {
    g_free(status->rolling_token);
    *status = (NotionRateLimitStatus){0};
}

static gboolean safe_value(const char *value) {
    if (!value || value[0] == '\0' || !g_utf8_validate(value, -1, NULL)) return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (*cursor < 32 || *cursor == 127) return FALSE;
    }
    return TRUE;
}

static gboolean valid_cookie_name(const char *name) {
    if (!name || name[0] == '\0') return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)name; *cursor; cursor++) {
        if (*cursor <= 32 || *cursor >= 127 || strchr("()<>@,;:\\\"/[]?={} ", *cursor)) return FALSE;
    }
    return TRUE;
}

static gboolean valid_cookie_value(const char *value) {
    if (!value || value[0] == '\0') return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (*cursor < 33 || *cursor == 127 || *cursor == ';') return FALSE;
    }
    return TRUE;
}

static char *normalize_cookie(const char *raw) {
    if (!raw || strlen(raw) > NOTION_MAXIMUM_CREDENTIAL_BYTES || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *clean = g_strstrip(g_strdup(raw));
    if (g_ascii_strncasecmp(clean, "Cookie:", 7) == 0) {
        memmove(clean, clean + 7, strlen(clean + 7) + 1);
        g_strstrip(clean);
    }
    if (!strchr(clean, '=')) {
        gboolean valid = valid_cookie_value(clean);
        char *cookie = valid ? g_strdup_printf("token_v2=%s", clean) : NULL;
        g_free(clean);
        return cookie;
    }

    gboolean has_token = FALSE;
    GString *result = g_string_new(NULL);
    char **parts = g_strsplit(clean, ";", -1);
    for (guint index = 0; parts[index]; index++) {
        char *part = g_strstrip(parts[index]);
        char *separator = strchr(part, '=');
        if (!separator || separator == part) continue;
        *separator = '\0';
        char *name = g_strstrip(part);
        char *value = g_strstrip(separator + 1);
        if (!valid_cookie_name(name) || !valid_cookie_value(value)) continue;
        if (g_str_equal(name, "token_v2")) has_token = TRUE;
        if (result->len > 0) g_string_append(result, "; ");
        g_string_append_printf(result, "%s=%s", name, value);
    }
    g_strfreev(parts);
    g_free(clean);
    if (has_token) return g_string_free(result, FALSE);
    g_string_free(result, TRUE);
    return NULL;
}

static void capture_header(NotionRequestContext *context, const char *field) {
    const char *colon = field ? strchr(field, ':') : NULL;
    if (!colon) return;
    char *name = g_strndup(field, (gsize)(colon - field));
    g_strstrip(name);
    char *value = g_strstrip(g_strdup(colon + 1));
    if (!safe_value(value)) {
        g_free(name);
        g_free(value);
        return;
    }
    if (g_ascii_strcasecmp(name, "cookie") == 0) {
        char *cookie = normalize_cookie(value);
        if (cookie) {
            g_free(context->cookie);
            context->cookie = cookie;
        }
    } else {
        for (size_t index = 0; index < G_N_ELEMENTS(notion_header_definitions); index++) {
            if (g_ascii_strcasecmp(name, notion_header_definitions[index].lower_name) != 0) continue;
            g_free(context->values[index]);
            context->values[index] = g_strdup(value);
            break;
        }
    }
    g_free(name);
    g_free(value);
}

static void append_shell_escape(GString *word, char escaped) {
    switch (escaped) {
    case 'n':
        g_string_append_c(word, '\n');
        break;
    case 'r':
        g_string_append_c(word, '\r');
        break;
    case 't':
        g_string_append_c(word, '\t');
        break;
    default:
        g_string_append_c(word, escaped);
        break;
    }
}

static GPtrArray *shell_words(const char *raw) {
    if (!raw || strlen(raw) > NOTION_MAXIMUM_CREDENTIAL_BYTES || !g_utf8_validate(raw, -1, NULL)) return NULL;
    GPtrArray *words = g_ptr_array_new_with_free_func(g_free);
    const char *cursor = raw;
    while (*cursor) {
        while (g_ascii_isspace(*cursor)) cursor++;
        if (!*cursor) break;
        GString *word = g_string_new(NULL);
        while (*cursor && !g_ascii_isspace(*cursor)) {
            if (*cursor == '\\' && cursor[1] == '\n') {
                cursor += 2;
                continue;
            }
            if (*cursor == '\'' || (*cursor == '$' && cursor[1] == '\'')) {
                gboolean ansi = *cursor == '$';
                cursor += ansi ? 2 : 1;
                while (*cursor && *cursor != '\'') {
                    if (ansi && *cursor == '\\' && cursor[1]) {
                        cursor++;
                        append_shell_escape(word, *cursor++);
                    } else {
                        g_string_append_c(word, *cursor++);
                    }
                }
                if (*cursor != '\'') goto malformed;
                cursor++;
            } else if (*cursor == '"') {
                cursor++;
                while (*cursor && *cursor != '"') {
                    if (*cursor == '\\' && cursor[1]) cursor++;
                    g_string_append_c(word, *cursor++);
                }
                if (*cursor != '"') goto malformed;
                cursor++;
            } else if (*cursor == '\\' && cursor[1]) {
                cursor++;
                g_string_append_c(word, *cursor++);
            } else {
                g_string_append_c(word, *cursor++);
            }
            if (word->len > NOTION_MAXIMUM_CREDENTIAL_BYTES) goto malformed;
        }
        g_ptr_array_add(words, g_string_free(word, FALSE));
        if (words->len > 256) {
            g_ptr_array_free(words, TRUE);
            return NULL;
        }
        continue;

    malformed:
        g_string_free(word, TRUE);
        g_ptr_array_free(words, TRUE);
        return NULL;
    }
    return words;
}

static gboolean parse_curl_capture(const char *raw, NotionRequestContext *context) {
    GPtrArray *words = shell_words(raw);
    if (!words || words->len < 2) {
        if (words) g_ptr_array_free(words, TRUE);
        return FALSE;
    }
    const char *first = g_ptr_array_index(words, 0);
    const char *command = strrchr(first, '/');
    command = command ? command + 1 : first;
    if (!g_str_equal(command, "curl")) {
        g_ptr_array_free(words, TRUE);
        return FALSE;
    }
    for (guint index = 1; index < words->len; index++) {
        const char *argument = g_ptr_array_index(words, index);
        const char *field = NULL;
        if (g_str_equal(argument, "-H") || g_str_equal(argument, "--header")) {
            if (++index < words->len) field = g_ptr_array_index(words, index);
        } else if (g_str_has_prefix(argument, "--header=")) {
            field = argument + strlen("--header=");
        } else if (g_str_has_prefix(argument, "-H") && argument[2] != '\0') {
            field = argument + 2;
        } else if (g_str_equal(argument, "-b") || g_str_equal(argument, "--cookie")) {
            if (++index < words->len) {
                char *cookie = normalize_cookie(g_ptr_array_index(words, index));
                if (cookie) {
                    g_free(context->cookie);
                    context->cookie = cookie;
                }
            }
        } else if (g_str_has_prefix(argument, "--cookie=")) {
            char *cookie = normalize_cookie(argument + strlen("--cookie="));
            if (cookie) {
                g_free(context->cookie);
                context->cookie = cookie;
            }
        }
        if (field) capture_header(context, field);
    }
    g_ptr_array_free(words, TRUE);
    return context->cookie != NULL;
}

static const char *config_string(const CodexBarProviderConfig *config, const char *key) {
    json_object *value = NULL;
    if (!config || !config->raw || !json_object_is_type(config->raw, json_type_object) ||
        !json_object_object_get_ex(config->raw, key, &value) || !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    const char *string = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    return string && !memchr(string, '\0', length) ? string : NULL;
}

static gboolean resolve_context(const CodexBarProviderConfig *config,
                                NotionRequestContext *context,
                                GError **error) {
    const char *source = config_string(config, "cookieSource");
    if (!source || !g_str_equal(source, "manual")) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "Notion AI requires cookieSource manual.");
        return FALSE;
    }
    const char *raw = config_string(config, "cookieHeader");
    if (!raw || raw[0] == '\0') {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "Notion AI needs a manual token_v2 cookie in cookieHeader.");
        return FALSE;
    }
    if (strlen(raw) > NOTION_MAXIMUM_CREDENTIAL_BYTES || !g_utf8_validate(raw, -1, NULL)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Notion AI cookieHeader is invalid.");
        return FALSE;
    }
    char *clean = g_strstrip(g_strdup(raw));
    gboolean valid = parse_curl_capture(clean, context);
    if (!valid) context->cookie = normalize_cookie(clean);
    valid = context->cookie != NULL;
    g_free(clean);
    if (valid) return TRUE;
    request_context_clear(context);
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "Notion AI cookieHeader must be a token_v2 value, Cookie header, or browser cURL capture.");
    return FALSE;
}

static json_object *parse_json(const char *data, size_t length) {
    if (!data || length > G_MAXINT || !g_utf8_validate(data, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, data, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && g_ascii_isspace((guchar)data[consumed])) consumed++;
    gboolean valid = parse_error == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static char *json_string(json_object *object, const char *key) {
    json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!text || memchr(text, '\0', length)) return NULL;
    return g_strdup(text);
}

static json_object *unwrap_record(json_object *raw) {
    if (!raw || !json_object_is_type(raw, json_type_object)) return NULL;
    json_object *value = NULL;
    if (!json_object_object_get_ex(raw, "value", &value) || !json_object_is_type(value, json_type_object)) return raw;
    json_object *inner = NULL;
    if (json_object_object_get_ex(value, "value", &inner) && json_object_is_type(inner, json_type_object)) {
        return inner;
    }
    return value;
}

static gboolean container_identifies_user(json_object *container, const char *key) {
    json_object *users = NULL;
    json_object *entry = NULL;
    if (!container || !json_object_is_type(container, json_type_object) ||
        !json_object_object_get_ex(container, "notion_user", &users) ||
        !json_object_is_type(users, json_type_object) || !json_object_object_get_ex(users, key, &entry)) {
        return FALSE;
    }
    json_object *record = unwrap_record(entry);
    char *id = json_string(record, "id");
    gboolean matches = id && g_str_equal(id, key);
    g_free(id);
    return matches;
}

static gint workspace_compare(gconstpointer left, gconstpointer right) {
    const NotionWorkspace *left_workspace = *(NotionWorkspace *const *)left;
    const NotionWorkspace *right_workspace = *(NotionWorkspace *const *)right;
    return g_strcmp0(left_workspace->id, right_workspace->id);
}

static gboolean parse_spaces(const char *data, size_t length, NotionAccount *account, GError **error) {
    json_object *root = parse_json(data, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse Notion getSpaces response.");
        return FALSE;
    }
    const char *resolved = NULL;
    guint identified = 0;
    json_object_object_foreach(root, key, container) {
        if (container_identifies_user(container, key)) {
            resolved = key;
            identified++;
        }
    }
    if (identified == 0 && json_object_object_length(root) == 1) {
        json_object_object_foreach(root, key, container) {
            (void)container;
            resolved = key;
        }
    }
    if (!resolved || identified > 1) {
        json_object_put(root);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "Notion getSpaces response did not identify a single user.");
        return FALSE;
    }

    json_object *selected_container = NULL;
    if (!json_object_object_get_ex(root, resolved, &selected_container) ||
        !json_object_is_type(selected_container, json_type_object)) {
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Notion getSpaces user record is invalid.");
        return FALSE;
    }
    account->user_id = g_strdup(resolved);
    account->workspaces = g_ptr_array_new_with_free_func(workspace_free);
    json_object *users = NULL;
    if (json_object_object_get_ex(selected_container, "notion_user", &users) &&
        json_object_is_type(users, json_type_object)) {
        json_object *entry = NULL;
        json_object *record = NULL;
        if (json_object_object_get_ex(users, resolved, &entry)) record = unwrap_record(entry);
        if (!record) {
            json_object_object_foreach(users, key, candidate) {
                (void)key;
                record = unwrap_record(candidate);
                if (record) break;
            }
        }
        account->email = json_string(record, "email");
        account->name = json_string(record, "name");
    }

    json_object *spaces = NULL;
    if (json_object_object_get_ex(selected_container, "space", &spaces) &&
        json_object_is_type(spaces, json_type_object)) {
        json_object_object_foreach(spaces, key, entry) {
            json_object *record = unwrap_record(entry);
            if (!record) continue;
            NotionWorkspace *workspace = g_new0(NotionWorkspace, 1);
            workspace->id = json_string(record, "id");
            if (!workspace->id) workspace->id = g_strdup(key);
            workspace->name = json_string(record, "name");
            workspace->plan_type = json_string(record, "plan_type");
            workspace->subscription_tier = json_string(record, "subscription_tier");
            g_ptr_array_add(account->workspaces, workspace);
        }
    }
    g_ptr_array_sort(account->workspaces, workspace_compare);
    json_object_put(root);
    return TRUE;
}

static char *normalize_workspace_id(const char *raw) {
    if (!raw) return NULL;
    char *trimmed = g_ascii_strdown(raw, -1);
    g_strstrip(trimmed);
    if (trimmed[0] == '\0') {
        g_free(trimmed);
        return NULL;
    }
    GString *compact = g_string_sized_new(32);
    gboolean hex = TRUE;
    for (const char *cursor = trimmed; *cursor; cursor++) {
        if (*cursor == '-') continue;
        if (!g_ascii_isxdigit(*cursor)) hex = FALSE;
        g_string_append_c(compact, *cursor);
    }
    if (hex && compact->len == 32) {
        char *normalized = g_strdup_printf("%.8s-%.4s-%.4s-%.4s-%.12s",
                                           compact->str,
                                           compact->str + 8,
                                           compact->str + 12,
                                           compact->str + 16,
                                           compact->str + 20);
        g_string_free(compact, TRUE);
        g_free(trimmed);
        return normalized;
    }
    g_string_free(compact, TRUE);
    return trimmed;
}

static gboolean workspace_may_have_allowance(const NotionWorkspace *workspace) {
    return workspace->subscription_tier &&
           (g_ascii_strcasecmp(workspace->subscription_tier, "business") == 0 ||
            g_ascii_strcasecmp(workspace->subscription_tier, "enterprise") == 0);
}

static NotionWorkspace *select_workspace(const NotionAccount *account, const char *configured_id) {
    char *preferred = normalize_workspace_id(configured_id);
    for (guint index = 0; preferred && index < account->workspaces->len; index++) {
        NotionWorkspace *workspace = g_ptr_array_index(account->workspaces, index);
        char *candidate = normalize_workspace_id(workspace->id);
        gboolean match = candidate && g_str_equal(candidate, preferred);
        g_free(candidate);
        if (match) {
            g_free(preferred);
            return workspace;
        }
    }
    g_free(preferred);
    for (guint index = 0; index < account->workspaces->len; index++) {
        NotionWorkspace *workspace = g_ptr_array_index(account->workspaces, index);
        if (workspace_may_have_allowance(workspace)) return workspace;
    }
    return account->workspaces->len > 0 ? g_ptr_array_index(account->workspaces, 0) : NULL;
}

static gboolean optional_number(
    json_object *object, const char *key, gboolean *present, double *result) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) {
        *present = FALSE;
        return TRUE;
    }
    if (!json_object_is_type(value, json_type_int) && !json_object_is_type(value, json_type_double)) return FALSE;
    *result = json_object_get_double(value);
    *present = isfinite(*result);
    return *present;
}

static gboolean optional_object(json_object *object, const char *key, gboolean *present, json_object **result) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) {
        *present = FALSE;
        *result = NULL;
        return TRUE;
    }
    if (!json_object_is_type(value, json_type_object)) return FALSE;
    *present = TRUE;
    *result = value;
    return TRUE;
}

static gboolean parse_rate_limit(const char *data,
                                 size_t length,
                                 NotionRateLimitStatus *status,
                                 GError **error) {
    json_object *root = parse_json(data, length);
    if (!root || !json_object_is_type(root, json_type_object)) goto invalid;
    char *state = json_string(root, "status");
    status->not_applicable = state && g_ascii_strcasecmp(state, "not_applicable") == 0;
    g_free(state);
    if (status->not_applicable) {
        json_object_put(root);
        return TRUE;
    }

    json_object *rolling = NULL;
    json_object *billing = NULL;
    if (!optional_object(root, "window", &status->has_rolling, &rolling) ||
        !optional_object(root, "billingPeriodWindow", &status->has_billing, &billing) ||
        (!status->has_rolling && !status->has_billing)) {
        goto invalid;
    }
    if (status->has_rolling) {
        if (!optional_number(rolling, "used", &status->has_rolling_used, &status->rolling_used) ||
            !optional_number(rolling, "limit", &status->has_rolling_limit, &status->rolling_limit)) {
            goto invalid;
        }
        status->rolling_token = json_string(rolling, "window");
    }
    if (!optional_number(
            root, "resetsInSeconds", &status->has_resets_in_seconds, &status->resets_in_seconds)) {
        goto invalid;
    }
    if (status->has_billing &&
        (!optional_number(billing, "used", &status->has_billing_used, &status->billing_used) ||
         !optional_number(billing, "limit", &status->has_billing_limit, &status->billing_limit) ||
         !optional_number(billing, "periodEndMs", &status->has_period_end_ms, &status->period_end_ms))) {
        goto invalid;
    }
    json_object_put(root);
    return TRUE;

invalid:
    if (root) json_object_put(root);
    rate_limit_status_clear(status);
    g_set_error_literal(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_DATA,
                        "Could not parse Notion getCreditRateLimitStatus response.");
    return FALSE;
}

static gboolean safe_milliseconds(double value, gint64 *result) {
    if (!isfinite(value) || value <= 0 || value < (double)G_MININT64 || value >= (double)G_MAXINT64) return FALSE;
    *result = (gint64)value;
    return TRUE;
}

static gboolean rolling_reset(gint64 now_ms, double seconds, gint64 *result) {
    if (!isfinite(seconds) || seconds < 0 || seconds > (double)G_MAXINT64 / 1000.0) return FALSE;
    double reset = (double)now_ms + seconds * 1000.0;
    if (reset < (double)G_MININT64 || reset >= (double)G_MAXINT64) return FALSE;
    *result = (gint64)reset;
    return TRUE;
}

static gboolean parse_window_minutes(const char *raw, gint64 *result) {
    if (!raw) return FALSE;
    char *clean = g_ascii_strdown(raw, -1);
    g_strstrip(clean);
    size_t length = strlen(clean);
    if (length < 2) {
        g_free(clean);
        return FALSE;
    }
    char unit = clean[length - 1];
    clean[length - 1] = '\0';
    char *end = NULL;
    guint64 value = g_ascii_strtoull(clean, &end, 10);
    gboolean valid = clean[0] != '\0' && end && *end == '\0' && value > 0;
    guint64 multiplier = unit == 'm' ? 1 : unit == 'h' ? 60 : unit == 'd' ? 1440 : unit == 'w' ? 10080 : 0;
    valid = valid && multiplier > 0 && value <= G_MAXINT64 / multiplier;
    guint64 minutes = valid ? value * multiplier : 0;
    g_free(clean);
    if (!valid || minutes == NOTION_MONTHLY_SENTINEL_MINUTES) return FALSE;
    *result = (gint64)minutes;
    return TRUE;
}

static gboolean calendar_month_minutes(gint64 period_end_ms, gint64 *result) {
    GDateTime *end = g_date_time_new_from_unix_utc(period_end_ms / 1000);
    if (!end) return FALSE;
    GDateTime *start = g_date_time_add_months(end, -1);
    GTimeSpan span = start ? g_date_time_difference(end, start) : 0;
    if (start) g_date_time_unref(start);
    g_date_time_unref(end);
    if (span <= 0 || span % G_TIME_SPAN_MINUTE != 0) return FALSE;
    *result = span / G_TIME_SPAN_MINUTE;
    return *result > 0;
}

static char *display_tier(const char *raw) {
    if (!raw) return NULL;
    char *tier = g_strstrip(g_strdup(raw));
    if (tier[0] == '\0') {
        g_free(tier);
        return NULL;
    }
    tier[0] = g_ascii_toupper(tier[0]);
    return tier;
}

static CodexBarProvider *make_provider(const NotionAccount *account,
                                       const NotionWorkspace *workspace,
                                       const NotionRateLimitStatus *status,
                                       gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("notion");
    provider->source = g_strdup("web");
    provider->account = g_strdup(account->email);
    provider->plan = display_tier(workspace->subscription_tier);
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->organization = g_strdup(workspace->name);
    provider->identity->account_id = g_strdup(account->user_id);
    provider->identity->login_method = display_tier(workspace->subscription_tier);
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));

    if (status->has_rolling_used && status->has_rolling_limit && status->rolling_limit > 0) {
        CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "Rolling");
        window->usage_known = TRUE;
        window->used_percent = MAX(0.0, status->rolling_used / status->rolling_limit * 100.0);
        window->has_window_minutes = parse_window_minutes(status->rolling_token, &window->window_minutes);
        window->has_resets_at = status->has_resets_in_seconds &&
                                rolling_reset(now_ms, status->resets_in_seconds, &window->resets_at_ms);
        codexbar_provider_add_quota_window(provider, window);
    }
    if (status->has_billing_used && status->has_billing_limit && status->billing_limit > 0) {
        CodexBarQuotaWindow *window = codexbar_quota_window_new("secondary", "Monthly");
        window->usage_known = TRUE;
        window->used_percent = MAX(0.0, status->billing_used / status->billing_limit * 100.0);
        window->has_resets_at = status->has_period_end_ms &&
                                safe_milliseconds(status->period_end_ms, &window->resets_at_ms);
        window->has_window_minutes = window->has_resets_at &&
                                     calendar_month_minutes(window->resets_at_ms, &window->window_minutes);
        codexbar_provider_add_quota_window(provider, window);
    }
    return provider;
}

static gboolean cancelled(GCancellable *cancellable, GError **error) {
    return cancellable && g_cancellable_set_error_if_cancelled(cancellable, error);
}

static CodexBarHttpResponse *post(const char *endpoint,
                                 const char *body,
                                 const NotionRequestContext *context,
                                 CodexBarNotionTransport transport,
                                 GCancellable *cancellable,
                                 GError **error) {
    char *url = g_strdup_printf(NOTION_BASE_URL "/api/v3/%s", endpoint);
    CodexBarHttpRequestHeader headers[13] = {{"Content-Type", "application/json"}};
    size_t header_count = 1;
    for (size_t index = 0; index < G_N_ELEMENTS(notion_header_definitions); index++) {
        const char *value = context->values[index] ? context->values[index]
                                                   : notion_header_definitions[index].default_value;
        if (!value) continue;
        headers[header_count++] = (CodexBarHttpRequestHeader){notion_header_definitions[index].wire_name, value};
    }
    headers[header_count++] = (CodexBarHttpRequestHeader){"Origin", NOTION_BASE_URL};
    headers[header_count++] = (CodexBarHttpRequestHeader){"Cookie", context->cookie};
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "POST",
        .headers = headers,
        .header_count = header_count,
        .body = body,
        .body_length = strlen(body),
        .timeout_seconds = 15,
        .maximum_response_bytes = NOTION_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = transport(&request, error);
    g_free(url);
    return response;
}

static gboolean response_succeeded(CodexBarHttpResponse *response,
                                   const char *endpoint,
                                   GError **error) {
    if (response->status == 200) return TRUE;
    if (response->status == 401) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PERMISSION_DENIED,
                            "Notion AI session cookie is invalid or expired.");
    } else {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Notion API error: HTTP %ld from %s.",
                    response->status,
                    endpoint);
    }
    return FALSE;
}

CodexBarProvider *codexbar_notion_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                        CodexBarNotionTransport transport,
                                                                        GCancellable *cancellable,
                                                                        gint64 now_ms,
                                                                        GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    if (cancelled(cancellable, error)) return NULL;
    NotionRequestContext context = {0};
    if (!resolve_context(config, &context, error)) return NULL;

    CodexBarHttpResponse *response = post("getSpaces", "{}", &context, transport, cancellable, error);
    if (!response || cancelled(cancellable, error)) {
        codexbar_http_response_free(response);
        request_context_clear(&context);
        return NULL;
    }
    if (!response_succeeded(response, "getSpaces", error)) {
        codexbar_http_response_free(response);
        request_context_clear(&context);
        return NULL;
    }
    NotionAccount account = {0};
    gboolean parsed = parse_spaces(response->body, response->body_length, &account, error);
    codexbar_http_response_free(response);
    if (!parsed) {
        request_context_clear(&context);
        account_clear(&account);
        return NULL;
    }
    NotionWorkspace *workspace = select_workspace(&account, config ? config->workspace_id : NULL);
    if (!workspace) {
        request_context_clear(&context);
        account_clear(&account);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No Notion workspace found for this account.");
        return NULL;
    }
    if (cancelled(cancellable, error)) {
        request_context_clear(&context);
        account_clear(&account);
        return NULL;
    }

    json_object *body_object = json_object_new_object();
    json_object_object_add(body_object, "spaceId", json_object_new_string(workspace->id));
    char *body = g_strdup(json_object_to_json_string_ext(body_object, JSON_C_TO_STRING_PLAIN));
    json_object_put(body_object);
    response = post("getCreditRateLimitStatus", body, &context, transport, cancellable, error);
    g_free(body);
    request_context_clear(&context);
    if (!response || cancelled(cancellable, error)) {
        codexbar_http_response_free(response);
        account_clear(&account);
        return NULL;
    }
    if (!response_succeeded(response, "getCreditRateLimitStatus", error)) {
        codexbar_http_response_free(response);
        account_clear(&account);
        return NULL;
    }
    NotionRateLimitStatus status = {0};
    parsed = parse_rate_limit(response->body, response->body_length, &status, error);
    codexbar_http_response_free(response);
    if (!parsed) {
        account_clear(&account);
        return NULL;
    }
    if (status.not_applicable) {
        const char *name = workspace->name ? workspace->name : workspace->id;
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_SUPPORTED,
                    "Notion AI usage allowance is not tracked for \"%s\". Allowances apply to Business and Enterprise workspaces.",
                    name);
        rate_limit_status_clear(&status);
        account_clear(&account);
        return NULL;
    }
    CodexBarProvider *provider = make_provider(&account, workspace, &status, now_ms);
    rate_limit_status_clear(&status);
    account_clear(&account);
    return provider;
}

CodexBarProvider *codexbar_notion_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error) {
    return codexbar_notion_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_notion_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_notion_fetch_with_cancellable(config, NULL, error);
}
