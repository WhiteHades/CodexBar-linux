#include "web_providers.h"

#include <json-c/json.h>
#include <math.h>
#include <sqlite3.h>
#include <string.h>

#define WEB_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)
#define WEB_MAXIMUM_CREDENTIAL_BYTES 16384U
#define WEB_TIMEOUT_SECONDS 15
#define CURSOR_BASE_URL "https://cursor.com"
#define OPENCODE_SERVER_URL "https://opencode.ai/_server"
#define OPENCODE_WORKSPACES_ID "def39973159c7f0483d8793a822b8dbb10d067e12c65455fcb4608459ba0234f"
#define OPENCODE_SUBSCRIPTION_ID "7abeebee372f304e050aaaf92be863f4a86490e382f8c79db68fd94040d691b4"
#define DEVIN_BASE_URL "https://app.devin.ai/api"

static gboolean json_whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

static json_object *parse_json_document(const char *json, size_t length) {
    if (!json || length == 0 || length > G_MAXINT || length > WEB_MAXIMUM_RESPONSE_BYTES ||
        !g_utf8_validate(json, (gssize)length, NULL) || memchr(json, '\0', length)) {
        return NULL;
    }
    json_tokener *tokener = json_tokener_new();
    if (!tokener) return NULL;
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, json, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && json_whitespace(json[consumed])) consumed++;
    gboolean valid = parse_error == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static json_object *object_member(json_object *object, const char *key) {
    json_object *value = NULL;
    return object && json_object_is_type(object, json_type_object) &&
                   json_object_object_get_ex(object, key, &value)
               ? value
               : NULL;
}

static void strip_unicode_whitespace(char *value) {
    char *start = value;
    while (*start && g_unichar_isspace(g_utf8_get_char(start))) start = g_utf8_next_char(start);
    char *end = value + strlen(value);
    while (end > start) {
        char *previous = g_utf8_find_prev_char(start, end);
        if (!previous || !g_unichar_isspace(g_utf8_get_char(previous))) break;
        end = previous;
    }
    size_t length = (size_t)(end - start);
    memmove(value, start, length);
    value[length] = '\0';
}

static char *clean_header_credential(const char *raw) {
    if (!raw || !g_utf8_validate(raw, -1, NULL) || strlen(raw) > WEB_MAXIMUM_CREDENTIAL_BYTES) return NULL;
    char *value = g_strdup(raw);
    strip_unicode_whitespace(value);
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (*cursor < 32 || *cursor == 127) {
            g_free(value);
            return NULL;
        }
    }
    if (value[0] != '\0') return value;
    g_free(value);
    return NULL;
}

static char *raw_string(const CodexBarProviderConfig *config, const char *key) {
    json_object *value = object_member(config ? config->raw : NULL, key);
    if (!value || !json_object_is_type(value, json_type_string)) return NULL;
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!text || memchr(text, '\0', length) || !g_utf8_validate(text, (gssize)length, NULL)) return NULL;
    char *copy = g_strndup(text, length);
    strip_unicode_whitespace(copy);
    if (copy[0] != '\0') return copy;
    g_free(copy);
    return NULL;
}

static char *first_config_or_environment(const CodexBarProviderConfig *config,
                                         const char *config_key,
                                         const char *const *environment_keys) {
    char *value = raw_string(config, config_key);
    for (size_t index = 0; !value && environment_keys[index]; index++) {
        value = clean_header_credential(g_getenv(environment_keys[index]));
    }
    return value;
}

static gboolean valid_cookie_name(const char *name) {
    if (!name || name[0] == '\0') return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)name; *cursor; cursor++) {
        if (*cursor <= 32 || *cursor >= 127 || strchr("()<>@,;:\\\"/[]?={} ", *cursor)) return FALSE;
    }
    return TRUE;
}

static char *normalize_cookie_header(const char *raw, const char *const *allowed_names) {
    char *header = clean_header_credential(raw);
    if (!header) return NULL;
    if (g_ascii_strncasecmp(header, "cookie:", 7) == 0) {
        memmove(header, header + 7, strlen(header + 7) + 1);
        strip_unicode_whitespace(header);
    }
    GString *result = g_string_new(NULL);
    char **parts = g_strsplit(header, ";", -1);
    for (size_t index = 0; parts[index]; index++) {
        char *part = g_strstrip(parts[index]);
        char *separator = strchr(part, '=');
        if (!separator || separator == part) continue;
        *separator = '\0';
        char *name = g_strstrip(part);
        char *value = g_strstrip(separator + 1);
        if (!valid_cookie_name(name) || value[0] == '\0') continue;
        gboolean allowed = allowed_names == NULL;
        for (size_t allowed_index = 0; !allowed && allowed_names[allowed_index]; allowed_index++) {
            allowed = g_str_equal(name, allowed_names[allowed_index]);
        }
        if (!allowed) continue;
        gboolean safe = TRUE;
        for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
            if (*cursor < 33 || *cursor == 127 || *cursor == ';') safe = FALSE;
        }
        if (!safe) continue;
        if (result->len > 0) g_string_append(result, "; ");
        g_string_append_printf(result, "%s=%s", name, value);
    }
    g_strfreev(parts);
    g_free(header);
    if (result->len > 0) return g_string_free(result, FALSE);
    g_string_free(result, TRUE);
    return NULL;
}

static gboolean json_number(json_object *value, double *result) {
    if (!value || json_object_is_type(value, json_type_null) || json_object_is_type(value, json_type_boolean)) {
        return FALSE;
    }
    double number = 0;
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        number = json_object_get_double(value);
    } else if (json_object_is_type(value, json_type_string)) {
        const char *text = json_object_get_string(value);
        size_t length = (size_t)json_object_get_string_len(value);
        if (!text || memchr(text, '\0', length)) return FALSE;
        char *copy = g_strndup(text, length);
        g_strstrip(copy);
        char *end = NULL;
        number = g_ascii_strtod(copy, &end);
        gboolean valid = copy[0] != '\0' && end && *end == '\0' && isfinite(number);
        g_free(copy);
        if (!valid) return FALSE;
    } else {
        return FALSE;
    }
    if (!isfinite(number)) return FALSE;
    *result = number;
    return TRUE;
}

static gboolean object_number(json_object *object, const char *key, double *result) {
    return json_number(object_member(object, key), result);
}

static char *json_clean_string(json_object *object, const char *key) {
    json_object *value = object_member(object, key);
    if (!value || !json_object_is_type(value, json_type_string)) return NULL;
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!text || memchr(text, '\0', length) || !g_utf8_validate(text, (gssize)length, NULL)) return NULL;
    char *copy = g_strndup(text, length);
    strip_unicode_whitespace(copy);
    if (copy[0] != '\0') return copy;
    g_free(copy);
    return NULL;
}

static gboolean parse_timestamp_ms(json_object *value, gint64 *result) {
    double number = 0;
    if (json_number(value, &number)) {
        if (number <= 0 || number > (double)G_MAXINT64) return FALSE;
        if (number < 1000000000000.0) number *= 1000.0;
        if (number > (double)G_MAXINT64) return FALSE;
        *result = (gint64)llround(number);
        return TRUE;
    }
    if (!value || !json_object_is_type(value, json_type_string)) return FALSE;
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!text || memchr(text, '\0', length)) return FALSE;
    char *copy = g_strndup(text, length);
    GDateTime *date = g_date_time_new_from_iso8601(copy, NULL);
    g_free(copy);
    if (!date) return FALSE;
    *result = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static gboolean check_cancelled(GCancellable *cancellable, GError **error) {
    return cancellable && g_cancellable_set_error_if_cancelled(cancellable, error);
}

static CodexBarHttpResponse *send_request(const CodexBarHttpRequest *request,
                                          CodexBarWebProvidersTransport transport,
                                          GError **error) {
    if (check_cancelled(request->cancellable, error)) return NULL;
    CodexBarHttpResponse *response = transport(request, error);
    if (request->cancellable && g_cancellable_is_cancelled(request->cancellable)) {
        codexbar_http_response_free(response);
        if (error && *error) g_clear_error(error);
        g_cancellable_set_error_if_cancelled(request->cancellable, error);
        return NULL;
    }
    if (!response && error && !*error) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Provider network request failed");
    }
    return response;
}

static CodexBarProvider *new_web_provider(const char *provider_id, gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup(provider_id);
    provider->source = g_strdup("web");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));
    return provider;
}

static CodexBarQuotaWindow *new_window(const char *id,
                                       const char *title,
                                       double used_percent,
                                       gint64 window_minutes,
                                       gboolean has_window_minutes,
                                       gint64 resets_at_ms,
                                       gboolean has_resets_at) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = CLAMP(used_percent, 0, 100);
    window->has_window_minutes = has_window_minutes;
    window->window_minutes = window_minutes;
    window->has_resets_at = has_resets_at;
    window->resets_at_ms = resets_at_ms;
    return window;
}

/* Cursor */

static const char *const cursor_environment_keys[] = {"CURSOR_COOKIE_HEADER", "CODEXBAR_CURSOR_COOKIE_HEADER", NULL};

static char *cursor_raw_cookie(const CodexBarProviderConfig *config) {
    return first_config_or_environment(config, "cookieHeader", cursor_environment_keys);
}

static char *cursor_manual_cookie_from_raw(const char *raw) {
    char *cookie = normalize_cookie_header(raw, NULL);
    return cookie;
}

static char *default_cursor_database_path(void) {
    return g_build_filename(g_get_user_config_dir(), "Cursor", "User", "globalStorage", "state.vscdb", NULL);
}

static char *base64url_decode_string(const char *encoded) {
    if (!encoded || !g_utf8_validate(encoded, -1, NULL)) return NULL;
    for (const unsigned char *cursor = (const unsigned char *)encoded; *cursor; cursor++) {
        if (!(g_ascii_isalnum(*cursor) || *cursor == '-' || *cursor == '_' || *cursor == '=')) return NULL;
    }
    GString *base64 = g_string_new(encoded);
    for (size_t index = 0; index < base64->len; index++) {
        if (base64->str[index] == '-') base64->str[index] = '+';
        if (base64->str[index] == '_') base64->str[index] = '/';
    }
    while (base64->len % 4 != 0) g_string_append_c(base64, '=');
    gsize decoded_length = 0;
    guchar *decoded = g_base64_decode(base64->str, &decoded_length);
    g_string_free(base64, TRUE);
    if (!decoded || decoded_length == 0 || memchr(decoded, '\0', decoded_length) ||
        !g_utf8_validate((const char *)decoded, (gssize)decoded_length, NULL)) {
        g_free(decoded);
        return NULL;
    }
    char *result = g_strndup((const char *)decoded, decoded_length);
    g_free(decoded);
    return result;
}

char *codexbar_cursor_load_app_cookie(const char *database_path, gint64 now_ms, GError **error) {
    char *owned_path = database_path ? NULL : default_cursor_database_path();
    const char *path = database_path ? database_path : owned_path;
    sqlite3 *database = NULL;
    if (sqlite3_open_v2(path, &database, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        const char *message = database ? sqlite3_errmsg(database) : "unknown error";
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Could not read Cursor auth database: %s", message);
        sqlite3_close(database);
        g_free(owned_path);
        return NULL;
    }
    sqlite3_busy_timeout(database, 250);
    sqlite3_stmt *statement = NULL;
    const char *query = "SELECT value FROM ItemTable WHERE key = ? LIMIT 1;";
    if (sqlite3_prepare_v2(database, query, -1, &statement, NULL) != SQLITE_OK ||
        sqlite3_bind_text(statement, 1, "cursorAuth/accessToken", -1, SQLITE_STATIC) != SQLITE_OK ||
        sqlite3_step(statement) != SQLITE_ROW) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Cursor app auth token was not found");
        sqlite3_finalize(statement);
        sqlite3_close(database);
        g_free(owned_path);
        return NULL;
    }
    const void *bytes = sqlite3_column_blob(statement, 0);
    int byte_count = sqlite3_column_bytes(statement, 0);
    char *token = bytes && byte_count > 0 && !memchr(bytes, '\0', (size_t)byte_count)
                      ? g_strndup(bytes, (size_t)byte_count)
                      : NULL;
    sqlite3_finalize(statement);
    sqlite3_close(database);
    g_free(owned_path);
    if (!token || !g_utf8_validate(token, -1, NULL) || strlen(token) > WEB_MAXIMUM_CREDENTIAL_BYTES) {
        g_free(token);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Cursor app auth token is invalid");
        return NULL;
    }
    char **segments = g_strsplit(token, ".", 3);
    char *payload_text = segments[0] && segments[1] ? base64url_decode_string(segments[1]) : NULL;
    json_object *payload = payload_text ? parse_json_document(payload_text, strlen(payload_text)) : NULL;
    char *subject = payload ? json_clean_string(payload, "sub") : NULL;
    double expiration = 0;
    gboolean has_expiration = payload && object_number(payload, "exp", &expiration);
    char *separator = subject ? strrchr(subject, '|') : NULL;
    const char *user_id = separator ? separator + 1 : subject;
    gboolean valid_user = user_id && user_id[0] != '\0';
    for (const unsigned char *cursor = (const unsigned char *)user_id; valid_user && *cursor; cursor++) {
        if (!(g_ascii_isalnum(*cursor) || *cursor == '.' || *cursor == '_' || *cursor == '-')) valid_user = FALSE;
    }
    gboolean fresh = has_expiration && expiration * 1000.0 > (double)now_ms + 60000.0;
    char *cookie = valid_user && fresh ? g_strdup_printf("WorkosCursorSessionToken=%s%%3A%%3A%s", user_id, token) : NULL;
    g_free(subject);
    if (payload) json_object_put(payload);
    g_free(payload_text);
    g_strfreev(segments);
    g_free(token);
    if (!cookie) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Cursor app auth token is expired or invalid");
    }
    return cookie;
}

gboolean codexbar_cursor_has_auth(const CodexBarProviderConfig *config) {
    char *raw = cursor_raw_cookie(config);
    if (raw) {
        char *cookie = cursor_manual_cookie_from_raw(raw);
        g_free(raw);
        gboolean valid = cookie != NULL;
        g_free(cookie);
        return valid;
    }
    char *path = default_cursor_database_path();
    gboolean exists = g_file_test(path, G_FILE_TEST_IS_REGULAR);
    g_free(path);
    return exists;
}

static gboolean nested_number(json_object *root,
                              const char *first,
                              const char *second,
                              const char *third,
                              double *result) {
    json_object *value = object_member(object_member(object_member(root, first), second), third);
    return json_number(value, result);
}

static char *cursor_plan_name(const char *raw) {
    if (!raw) return NULL;
    char *lower = g_ascii_strdown(raw, -1);
    const char *suffix = raw;
    if (g_str_equal(lower, "enterprise")) suffix = "Enterprise";
    else if (g_str_equal(lower, "express")) suffix = "Start";
    else if (g_str_equal(lower, "free")) suffix = "Free";
    else if (g_str_equal(lower, "free_trial")) suffix = "Pro Trial";
    else if (g_str_equal(lower, "hobby")) suffix = "Hobby";
    else if (g_str_equal(lower, "pro")) suffix = "Pro";
    else if (g_str_equal(lower, "pro_plus")) suffix = "Pro+";
    else if (g_str_equal(lower, "pro_student")) suffix = "Pro";
    else if (g_str_equal(lower, "team")) suffix = "Team";
    else if (g_str_equal(lower, "ultra")) suffix = "Ultra";
    char *result = g_strdup_printf("Cursor %s", suffix);
    g_free(lower);
    return result;
}

CodexBarProvider *codexbar_cursor_parse_usage(const char *summary_json,
                                              size_t summary_length,
                                              const char *user_json,
                                              size_t user_length,
                                              const char *request_json,
                                              size_t request_length,
                                              gint64 now_ms,
                                              GError **error) {
    json_object *summary = parse_json_document(summary_json, summary_length);
    if (!summary || !json_object_is_type(summary, json_type_object)) {
        if (summary) json_object_put(summary);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Cursor usage summary is invalid JSON");
        return NULL;
    }
    json_object *user = user_json && user_length ? parse_json_document(user_json, user_length) : NULL;
    json_object *request = request_json && request_length ? parse_json_document(request_json, request_length) : NULL;
    double plan_used = 0, plan_limit = 0, overall_used = 0, overall_limit = 0, pooled_used = 0, pooled_limit = 0;
    gboolean has_plan_used = nested_number(summary, "individualUsage", "plan", "used", &plan_used);
    gboolean has_plan_limit = nested_number(summary, "individualUsage", "plan", "limit", &plan_limit);
    gboolean has_overall_used = nested_number(summary, "individualUsage", "overall", "used", &overall_used);
    gboolean has_overall_limit = nested_number(summary, "individualUsage", "overall", "limit", &overall_limit);
    gboolean has_pooled_used = nested_number(summary, "teamUsage", "pooled", "used", &pooled_used);
    gboolean has_pooled_limit = nested_number(summary, "teamUsage", "pooled", "limit", &pooled_limit);
    double auto_percent = 0, api_percent = 0, total_percent = 0;
    gboolean has_auto = nested_number(summary, "individualUsage", "plan", "autoPercentUsed", &auto_percent);
    gboolean has_api = nested_number(summary, "individualUsage", "plan", "apiPercentUsed", &api_percent);
    gboolean has_total = nested_number(summary, "individualUsage", "plan", "totalPercentUsed", &total_percent);
    double headline = 0;
    if (has_total) headline = total_percent;
    else if (has_auto && has_api) headline = (auto_percent + api_percent) / 2.0;
    else if (has_api) headline = api_percent;
    else if (has_auto) headline = auto_percent;
    else if (has_plan_used && has_plan_limit && plan_limit > 0) headline = plan_used / plan_limit * 100.0;
    else if (has_overall_used && has_overall_limit && overall_limit > 0) headline = overall_used / overall_limit * 100.0;
    else if (has_pooled_used && has_pooled_limit && pooled_limit > 0) headline = pooled_used / pooled_limit * 100.0;

    gint64 billing_start = 0, billing_end = 0;
    gboolean has_billing_start = parse_timestamp_ms(object_member(summary, "billingCycleStart"), &billing_start);
    gboolean has_billing_end = parse_timestamp_ms(object_member(summary, "billingCycleEnd"), &billing_end);
    gint64 window_minutes = 0;
    gboolean has_window_minutes = has_billing_start && has_billing_end && billing_end > billing_start;
    if (has_window_minutes) window_minutes = (billing_end - billing_start + 30000) / 60000;

    double requests_used = 0, requests_limit = 0;
    json_object *gpt4 = object_member(request, "gpt-4");
    gboolean has_requests_used = object_number(gpt4, "numRequestsTotal", &requests_used) ||
                                 object_number(gpt4, "numRequests", &requests_used);
    gboolean has_requests_limit = object_number(gpt4, "maxRequestUsage", &requests_limit) && requests_limit > 0;
    gboolean legacy = has_requests_used && has_requests_limit;
    if (legacy) headline = requests_used / requests_limit * 100.0;

    CodexBarProvider *provider = new_web_provider("cursor", now_ms);
    provider->dashboard_url = g_strdup("https://cursor.com/dashboard?tab=usage");
    provider->explicit_quota_slots = TRUE;
    CodexBarQuotaWindow *primary = new_window("primary",
                                              legacy ? "Requests" : "Total",
                                              headline,
                                              window_minutes,
                                              has_window_minutes,
                                              billing_end,
                                              has_billing_end);
    if (legacy) primary->detail = g_strdup_printf("%.0f/%.0f requests", requests_used, requests_limit);
    codexbar_provider_add_quota_window(provider, primary);
    if (!legacy && has_auto) {
        codexbar_provider_add_quota_window(provider,
                                           new_window("secondary", "Auto", auto_percent, window_minutes,
                                                      has_window_minutes, billing_end, has_billing_end));
    }
    if (!legacy && has_api) {
        codexbar_provider_add_quota_window(provider,
                                           new_window("tertiary", "API", api_percent, window_minutes,
                                                      has_window_minutes, billing_end, has_billing_end));
    }

    double individual_on_demand_used = 0, individual_on_demand_limit = 0;
    double team_on_demand_used = 0, team_on_demand_limit = 0;
    gboolean has_individual_used = nested_number(summary, "individualUsage", "onDemand", "used",
                                                 &individual_on_demand_used);
    gboolean has_individual_limit = nested_number(summary, "individualUsage", "onDemand", "limit",
                                                  &individual_on_demand_limit);
    gboolean has_team_used = nested_number(summary, "teamUsage", "onDemand", "used", &team_on_demand_used);
    gboolean has_team_limit = nested_number(summary, "teamUsage", "onDemand", "limit", &team_on_demand_limit);
    double cost_used = has_individual_used ? individual_on_demand_used / 100.0 : 0;
    double cost_limit = has_individual_limit ? individual_on_demand_limit / 100.0 : 0;
    gboolean shared_budget = cost_limit <= 0 && has_team_limit && team_on_demand_limit > 0;
    if (shared_budget) {
        cost_used = has_team_used ? team_on_demand_used / 100.0 : 0;
        cost_limit = team_on_demand_limit / 100.0;
    }
    if (cost_used > 0 || cost_limit > 0) {
        provider->provider_cost = g_new0(CodexBarProviderCost, 1);
        provider->provider_cost->used = cost_used;
        provider->provider_cost->limit = cost_limit;
        provider->provider_cost->currency = g_strdup("USD");
        provider->provider_cost->period = g_strdup("Monthly");
        provider->provider_cost->has_resets_at = has_billing_end;
        provider->provider_cost->resets_at_ms = billing_end;
        provider->provider_cost->has_updated_at = TRUE;
        provider->provider_cost->updated_at_ms = now_ms;
        if (shared_budget && has_individual_used && individual_on_demand_used > 0) {
            provider->provider_cost->has_personal_used = TRUE;
            provider->provider_cost->personal_used = individual_on_demand_used / 100.0;
        }
    }
    char *membership = json_clean_string(summary, "membershipType");
    char *email = user ? json_clean_string(user, "email") : NULL;
    char *account_id = user ? json_clean_string(user, "sub") : NULL;
    provider->account = email;
    if (membership || account_id) {
        provider->identity = g_new0(CodexBarProviderIdentity, 1);
        provider->identity->account_id = account_id;
        provider->identity->login_method = cursor_plan_name(membership);
    } else {
        g_free(account_id);
    }
    provider->plan = cursor_plan_name(membership);
    g_free(membership);
    if (request) json_object_put(request);
    if (user) json_object_put(user);
    json_object_put(summary);
    return provider;
}

static CodexBarHttpResponse *web_get(const char *url,
                                     const CodexBarHttpRequestHeader *headers,
                                     size_t header_count,
                                     CodexBarWebProvidersTransport transport,
                                     GCancellable *cancellable,
                                     GError **error) {
    CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = header_count,
        .timeout_seconds = WEB_TIMEOUT_SECONDS,
        .maximum_response_bytes = WEB_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    return send_request(&request, transport, error);
}

static gboolean require_success(CodexBarHttpResponse *response, const char *provider, GError **error) {
    if (response->status == 200) return TRUE;
    if (response->status == 401 || response->status == 403) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s credentials are invalid or expired", provider);
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s API returned HTTP %ld", provider, response->status);
    }
    return FALSE;
}

static char *cursor_user_id_from_user(const CodexBarHttpResponse *response) {
    json_object *root = response ? parse_json_document(response->body, response->body_length) : NULL;
    char *user_id = root ? json_clean_string(root, "sub") : NULL;
    if (root) json_object_put(root);
    return user_id;
}

CodexBarProvider *codexbar_cursor_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProvidersTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *raw_cookie = cursor_raw_cookie(config);
    char *cookie = cursor_manual_cookie_from_raw(raw_cookie);
    if (raw_cookie && !cookie) {
        g_free(raw_cookie);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Cursor manual cookie header is invalid");
        return NULL;
    }
    g_free(raw_cookie);
    if (!cookie) {
        GError *auth_error = NULL;
        cookie = codexbar_cursor_load_app_cookie(NULL, now_ms, &auth_error);
        if (!cookie) {
            g_propagate_prefixed_error(error, auth_error, "Cursor authentication failed: ");
            return NULL;
        }
    }
    CodexBarHttpRequestHeader headers[] = {
        {"Accept", "application/json"},
        {"Cookie", cookie},
    };
    CodexBarHttpResponse *summary = web_get(CURSOR_BASE_URL "/api/usage-summary",
                                            headers,
                                            G_N_ELEMENTS(headers),
                                            transport,
                                            cancellable,
                                            error);
    if (!summary || !require_success(summary, "Cursor", error)) {
        codexbar_http_response_free(summary);
        g_free(cookie);
        return NULL;
    }
    CodexBarHttpResponse *user = web_get(CURSOR_BASE_URL "/api/auth/me",
                                         headers,
                                         G_N_ELEMENTS(headers),
                                         transport,
                                         cancellable,
                                         NULL);
    char *user_id = user && user->status == 200 ? cursor_user_id_from_user(user) : NULL;
    CodexBarHttpResponse *legacy = NULL;
    if (user_id) {
        char *escaped = g_uri_escape_string(user_id, NULL, TRUE);
        char *url = g_strdup_printf(CURSOR_BASE_URL "/api/usage?user=%s", escaped);
        legacy = web_get(url, headers, G_N_ELEMENTS(headers), transport, cancellable, NULL);
        g_free(url);
        g_free(escaped);
        if (legacy && legacy->status != 200) g_clear_pointer(&legacy, codexbar_http_response_free);
    }
    CodexBarProvider *provider = codexbar_cursor_parse_usage(summary->body,
                                                             summary->body_length,
                                                             user && user->status == 200 ? user->body : NULL,
                                                             user && user->status == 200 ? user->body_length : 0,
                                                             legacy ? legacy->body : NULL,
                                                             legacy ? legacy->body_length : 0,
                                                             now_ms,
                                                             error);
    codexbar_http_response_free(legacy);
    codexbar_http_response_free(user);
    codexbar_http_response_free(summary);
    g_free(user_id);
    g_free(cookie);
    return provider;
}

CodexBarProvider *codexbar_cursor_fetch_with_transport(const CodexBarProviderConfig *config,
                                                       CodexBarWebProvidersTransport transport,
                                                       gint64 now_ms,
                                                       GError **error) {
    return codexbar_cursor_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_cursor_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error) {
    return codexbar_cursor_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_cursor_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_cursor_fetch_with_cancellable(config, NULL, error);
}

/* OpenCode */

static const char *const opencode_allowed_cookies[] = {"auth", "__Host-auth", NULL};
static const char *const opencode_environment_keys[] = {
    "OPENCODE_COOKIE_HEADER", "CODEXBAR_OPENCODE_COOKIE_HEADER", NULL,
};

static char *opencode_cookie(const CodexBarProviderConfig *config) {
    char *raw = first_config_or_environment(config, "cookieHeader", opencode_environment_keys);
    char *cookie = normalize_cookie_header(raw, opencode_allowed_cookies);
    g_free(raw);
    return cookie;
}

gboolean codexbar_opencode_has_auth(const CodexBarProviderConfig *config) {
    char *cookie = opencode_cookie(config);
    gboolean present = cookie != NULL;
    g_free(cookie);
    return present;
}

static const char *const opencode_percent_keys[] = {
    "usagePercent", "usedPercent", "percentUsed", "percent", "usage_percent", "used_percent",
    "utilization", "utilizationPercent", "utilization_percent", "usage", NULL,
};
static const char *const opencode_reset_seconds_keys[] = {
    "resetInSec", "resetInSeconds", "resetSeconds", "reset_sec", "reset_in_sec", "resetsInSec",
    "resetsInSeconds", "resetIn", "resetSec", NULL,
};
static const char *const opencode_reset_at_keys[] = {
    "resetAt", "resetsAt", "reset_at", "resets_at", "nextReset", "next_reset", "renewAt", "renew_at", NULL,
};

typedef struct {
    double percent;
    gint64 reset_seconds;
    char *path;
} OpenCodeCandidate;

static void opencode_candidate_free(gpointer data) {
    OpenCodeCandidate *candidate = data;
    g_free(candidate->path);
    g_free(candidate);
}

static gboolean first_number(json_object *object, const char *const *keys, double *result) {
    for (size_t index = 0; keys[index]; index++) {
        if (object_number(object, keys[index], result)) return TRUE;
    }
    return FALSE;
}

static gboolean first_timestamp(json_object *object, const char *const *keys, gint64 *result) {
    for (size_t index = 0; keys[index]; index++) {
        if (parse_timestamp_ms(object_member(object, keys[index]), result)) return TRUE;
    }
    return FALSE;
}

static OpenCodeCandidate *opencode_parse_window(json_object *object, const char *path, gint64 now_ms) {
    if (!object || !json_object_is_type(object, json_type_object)) return NULL;
    double percent = 0;
    gboolean direct_percent = first_number(object, opencode_percent_keys, &percent);
    if (!direct_percent) {
        double used = 0, limit = 0;
        static const char *const used_keys[] = {"used", "usage", "consumed", "count", "usedTokens", NULL};
        static const char *const limit_keys[] = {"limit", "total", "quota", "max", "cap", "tokenLimit", NULL};
        if (!first_number(object, used_keys, &used) || !first_number(object, limit_keys, &limit) || limit <= 0) {
            return NULL;
        }
        percent = used / limit * 100.0;
    } else if (percent >= 0 && percent <= 1.0) {
        percent *= 100.0;
    }
    gint64 reset_seconds = 0;
    double raw_seconds = 0;
    if (first_number(object, opencode_reset_seconds_keys, &raw_seconds) && raw_seconds >= 0 &&
        raw_seconds <= (double)G_MAXINT64) {
        reset_seconds = (gint64)raw_seconds;
    } else {
        gint64 reset_ms = 0;
        if (first_timestamp(object, opencode_reset_at_keys, &reset_ms)) {
            reset_seconds = MAX((gint64)0, (reset_ms - now_ms) / 1000);
        }
    }
    OpenCodeCandidate *candidate = g_new0(OpenCodeCandidate, 1);
    candidate->percent = CLAMP(percent, 0, 100);
    candidate->reset_seconds = reset_seconds;
    candidate->path = g_ascii_strdown(path, -1);
    return candidate;
}

static void opencode_collect_candidates(json_object *value,
                                        const char *path,
                                        guint depth,
                                        gint64 now_ms,
                                        GPtrArray *candidates) {
    if (!value || depth > 8) return;
    if (json_object_is_type(value, json_type_object)) {
        OpenCodeCandidate *candidate = opencode_parse_window(value, path, now_ms);
        if (candidate) g_ptr_array_add(candidates, candidate);
        json_object_object_foreach(value, key, child) {
            char *child_path = path[0] ? g_strdup_printf("%s.%s", path, key) : g_strdup(key);
            opencode_collect_candidates(child, child_path, depth + 1, now_ms, candidates);
            g_free(child_path);
        }
    } else if (json_object_is_type(value, json_type_array)) {
        size_t count = json_object_array_length(value);
        for (size_t index = 0; index < count; index++) {
            char *child_path = g_strdup_printf("%s[%zu]", path, index);
            opencode_collect_candidates(json_object_array_get_idx(value, index),
                                        child_path,
                                        depth + 1,
                                        now_ms,
                                        candidates);
            g_free(child_path);
        }
    }
}

static gboolean path_is_rolling(const char *path) {
    return strstr(path, "rolling") || strstr(path, "hour") || strstr(path, "5h") || strstr(path, "5-hour");
}

static gboolean path_is_weekly(const char *path) {
    return strstr(path, "weekly") || strstr(path, "week");
}

static OpenCodeCandidate *opencode_pick(GPtrArray *candidates,
                                        gboolean rolling,
                                        OpenCodeCandidate *excluded) {
    OpenCodeCandidate *best_preferred = NULL;
    OpenCodeCandidate *best_fallback = NULL;
    for (guint index = 0; index < candidates->len; index++) {
        OpenCodeCandidate *candidate = g_ptr_array_index(candidates, index);
        if (candidate == excluded) continue;
        gboolean preferred = rolling ? path_is_rolling(candidate->path) : path_is_weekly(candidate->path);
        OpenCodeCandidate **best = preferred ? &best_preferred : &best_fallback;
        if (!*best || (rolling ? candidate->reset_seconds < (*best)->reset_seconds
                              : candidate->reset_seconds > (*best)->reset_seconds) ||
            (candidate->reset_seconds == (*best)->reset_seconds && candidate->percent > (*best)->percent)) {
            *best = candidate;
        }
    }
    return best_preferred ? best_preferred : best_fallback;
}

static gboolean opencode_extract_legacy(const char *text,
                                        const char *window_name,
                                        double *percent,
                                        gint64 *reset_seconds) {
    char *percent_pattern = g_strdup_printf(
        "%s[^}]*usagePercent[[:space:]]*:[[:space:]]*([0-9]+(?:\\.[0-9]+)?)", window_name);
    char *reset_pattern = g_strdup_printf(
        "%s[^}]*resetInSec[[:space:]]*:[[:space:]]*([0-9]+)", window_name);
    GRegex *percent_regex = g_regex_new(percent_pattern, G_REGEX_CASELESS, 0, NULL);
    GRegex *reset_regex = g_regex_new(reset_pattern, G_REGEX_CASELESS, 0, NULL);
    g_free(percent_pattern);
    g_free(reset_pattern);
    if (!percent_regex || !reset_regex) {
        if (percent_regex) g_regex_unref(percent_regex);
        if (reset_regex) g_regex_unref(reset_regex);
        return FALSE;
    }
    GMatchInfo *percent_match = NULL;
    GMatchInfo *reset_match = NULL;
    gboolean found = g_regex_match(percent_regex, text, 0, &percent_match) &&
                     g_regex_match(reset_regex, text, 0, &reset_match);
    char *percent_text = found ? g_match_info_fetch(percent_match, 1) : NULL;
    char *reset_text = found ? g_match_info_fetch(reset_match, 1) : NULL;
    if (found) {
        *percent = g_ascii_strtod(percent_text, NULL);
        *reset_seconds = g_ascii_strtoll(reset_text, NULL, 10);
    }
    g_free(percent_text);
    g_free(reset_text);
    if (percent_match) g_match_info_free(percent_match);
    if (reset_match) g_match_info_free(reset_match);
    g_regex_unref(percent_regex);
    g_regex_unref(reset_regex);
    return found;
}

static gboolean find_timestamp_recursive(json_object *value,
                                         const char *const *keys,
                                         gint64 *result,
                                         guint depth) {
    if (!value || depth > 8) return FALSE;
    gboolean found = FALSE;
    if (json_object_is_type(value, json_type_object)) {
        found = first_timestamp(value, keys, result);
        json_object_object_foreach(value, key, child) {
            (void)key;
            gint64 child_result = 0;
            if (find_timestamp_recursive(child, keys, &child_result, depth + 1)) {
                *result = child_result;
                found = TRUE;
            }
        }
    } else if (json_object_is_type(value, json_type_array)) {
        for (size_t index = 0; index < json_object_array_length(value); index++) {
            gint64 child_result = 0;
            if (find_timestamp_recursive(
                    json_object_array_get_idx(value, index), keys, &child_result, depth + 1)) {
                *result = child_result;
                found = TRUE;
            }
        }
    }
    return found;
}

CodexBarProvider *codexbar_opencode_parse_usage(const char *json,
                                                size_t length,
                                                gint64 now_ms,
                                                GError **error) {
    json_object *root = parse_json_document(json, length);
    double rolling_percent = 0, weekly_percent = 0;
    gint64 rolling_reset_seconds = 0, weekly_reset_seconds = 0;
    gint64 renews_at_ms = 0;
    gboolean has_renewal = FALSE;
    GPtrArray *candidates = g_ptr_array_new_with_free_func(opencode_candidate_free);
    if (root) {
        opencode_collect_candidates(root, "", 0, now_ms, candidates);
        OpenCodeCandidate *rolling = opencode_pick(candidates, TRUE, NULL);
        OpenCodeCandidate *weekly = opencode_pick(candidates, FALSE, rolling);
        if (rolling && weekly) {
            rolling_percent = rolling->percent;
            rolling_reset_seconds = rolling->reset_seconds;
            weekly_percent = weekly->percent;
            weekly_reset_seconds = weekly->reset_seconds;
        }
        static const char *const renewal_keys[] = {"renewAt", "renew_at", NULL};
        has_renewal = find_timestamp_recursive(root, renewal_keys, &renews_at_ms, 0);
    }
    gboolean found = candidates->len >= 2;
    if (!found && json && g_utf8_validate(json, (gssize)length, NULL) && !memchr(json, '\0', length)) {
        char *text = g_strndup(json, length);
        found = opencode_extract_legacy(text, "rollingUsage", &rolling_percent, &rolling_reset_seconds) &&
                opencode_extract_legacy(text, "weeklyUsage", &weekly_percent, &weekly_reset_seconds);
        g_free(text);
    }
    g_ptr_array_unref(candidates);
    if (root) json_object_put(root);
    if (!found) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenCode response is missing usage fields");
        return NULL;
    }
    CodexBarProvider *provider = new_web_provider("opencode", now_ms);
    provider->dashboard_url = g_strdup("https://opencode.ai");
    provider->explicit_quota_slots = TRUE;
    codexbar_provider_add_quota_window(
        provider,
        new_window("primary", "5-hour", rolling_percent, 300, TRUE,
                   now_ms + rolling_reset_seconds * 1000, TRUE));
    codexbar_provider_add_quota_window(
        provider,
        new_window("secondary", "Weekly", weekly_percent, 10080, TRUE,
                   now_ms + weekly_reset_seconds * 1000, TRUE));
    if (has_renewal) {
        provider->has_subscription_renews_at = TRUE;
        provider->subscription_renews_at_ms = renews_at_ms;
    }
    return provider;
}

static gboolean opencode_signed_out(const char *text) {
    if (!text) return FALSE;
    char *lower = g_ascii_strdown(text, -1);
    gboolean result = strstr(lower, "login") || strstr(lower, "sign in") || strstr(lower, "auth/authorize") ||
                      strstr(lower, "not associated with an account") || strstr(lower, "actor of type \"public\"");
    g_free(lower);
    return result;
}

static gboolean valid_workspace_id(const char *value) {
    if (!value || !g_str_has_prefix(value, "wrk_") || strlen(value) <= 4) return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)value + 4; *cursor; cursor++) {
        if (!g_ascii_isalnum(*cursor)) return FALSE;
    }
    return TRUE;
}

static char *workspace_from_config(const CodexBarProviderConfig *config) {
    const char *raw = config ? config->workspace_id : NULL;
    if (!raw || !raw[0]) raw = g_getenv("CODEXBAR_OPENCODE_WORKSPACE_ID");
    if (!raw || !g_utf8_validate(raw, -1, NULL)) return NULL;
    const char *match = strstr(raw, "wrk_");
    if (!match) return NULL;
    const char *end = match + 4;
    while (g_ascii_isalnum(*end)) end++;
    char *candidate = g_strndup(match, (size_t)(end - match));
    if (valid_workspace_id(candidate)) return candidate;
    g_free(candidate);
    return NULL;
}

static char *opencode_first_workspace(const char *text, size_t length) {
    if (!text || !g_utf8_validate(text, (gssize)length, NULL) || memchr(text, '\0', length)) return NULL;
    char *copy = g_strndup(text, length);
    const char *match = strstr(copy, "wrk_");
    while (match) {
        const char *end = match + 4;
        while (g_ascii_isalnum(*end)) end++;
        char *candidate = g_strndup(match, (size_t)(end - match));
        if (valid_workspace_id(candidate)) {
            g_free(copy);
            return candidate;
        }
        g_free(candidate);
        match = strstr(match + 4, "wrk_");
    }
    g_free(copy);
    return NULL;
}

static CodexBarHttpResponse *opencode_server_request(const char *server_id,
                                                     const char *workspace,
                                                     gboolean post,
                                                     const char *cookie,
                                                     CodexBarWebProvidersTransport transport,
                                                     GCancellable *cancellable,
                                                     GError **error) {
    char *args = workspace ? g_strdup_printf("[\"%s\"]", workspace) : NULL;
    char *escaped_id = g_uri_escape_string(server_id, NULL, TRUE);
    char *escaped_args = args ? g_uri_escape_string(args, NULL, TRUE) : NULL;
    char *url = post ? g_strdup(OPENCODE_SERVER_URL)
                     : escaped_args ? g_strdup_printf(OPENCODE_SERVER_URL "?id=%s&args=%s", escaped_id, escaped_args)
                                    : g_strdup_printf(OPENCODE_SERVER_URL "?id=%s", escaped_id);
    char *instance = g_uuid_string_random();
    char *instance_header = g_strdup_printf("server-fn:%s", instance);
    char *referer = workspace ? g_strdup_printf("https://opencode.ai/workspace/%s/billing", workspace)
                              : g_strdup("https://opencode.ai");
    CodexBarHttpRequestHeader headers[] = {
        {"Cookie", cookie},
        {"X-Server-Id", server_id},
        {"X-Server-Instance", instance_header},
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/143.0.0.0 Safari/537.36"},
        {"Origin", "https://opencode.ai"},
        {"Referer", referer},
        {"Accept", "text/javascript, application/json;q=0.9, */*;q=0.8"},
        {"Content-Type", "application/json"},
    };
    CodexBarHttpRequest request = {
        .url = url,
        .method = post ? "POST" : "GET",
        .headers = headers,
        .header_count = post ? G_N_ELEMENTS(headers) : G_N_ELEMENTS(headers) - 1,
        .body = post ? (args ? args : "[]") : NULL,
        .body_length = post ? strlen(args ? args : "[]") : 0,
        .timeout_seconds = WEB_TIMEOUT_SECONDS,
        .maximum_response_bytes = WEB_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    g_free(referer);
    g_free(instance_header);
    g_free(instance);
    g_free(url);
    g_free(escaped_args);
    g_free(escaped_id);
    g_free(args);
    return response;
}

static gboolean opencode_response_ok(CodexBarHttpResponse *response, GError **error) {
    if (response->status == 200) return TRUE;
    if (response->status == 401 || response->status == 403 || opencode_signed_out(response->body)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "OpenCode session cookie is invalid or expired");
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OpenCode API returned HTTP %ld", response->status);
    }
    return FALSE;
}

static gboolean opencode_require_session(CodexBarHttpResponse *response, GError **error) {
    if (!opencode_response_ok(response, error)) return FALSE;
    if (!opencode_signed_out(response->body)) return TRUE;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                        "OpenCode session cookie is invalid or expired");
    return FALSE;
}

static gboolean response_is_null(const CodexBarHttpResponse *response) {
    if (!response || !response->body || !g_utf8_validate(response->body, (gssize)response->body_length, NULL) ||
        memchr(response->body, '\0', response->body_length)) {
        return FALSE;
    }
    char *copy = g_strndup(response->body, response->body_length);
    g_strstrip(copy);
    gboolean is_null = copy[0] == '\0' || g_ascii_strcasecmp(copy, "null") == 0;
    g_free(copy);
    return is_null;
}

CodexBarProvider *codexbar_opencode_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProvidersTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *cookie = opencode_cookie(config);
    if (!cookie) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "OpenCode requires a manual auth or __Host-auth cookie on Linux");
        return NULL;
    }
    char *workspace = workspace_from_config(config);
    if (!workspace) {
        for (int post = 0; post <= 1 && !workspace; post++) {
            CodexBarHttpResponse *response = opencode_server_request(
                OPENCODE_WORKSPACES_ID, NULL, post != 0, cookie, transport, cancellable, error);
            if (!response) goto fail;
            if (!opencode_require_session(response, error)) {
                codexbar_http_response_free(response);
                goto fail;
            }
            workspace = opencode_first_workspace(response->body, response->body_length);
            codexbar_http_response_free(response);
        }
        if (!workspace) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenCode workspace id is missing");
            goto fail;
        }
    }
    CodexBarProvider *provider = NULL;
    for (int post = 0; post <= 1 && !provider; post++) {
        CodexBarHttpResponse *response = opencode_server_request(
            OPENCODE_SUBSCRIPTION_ID, workspace, post != 0, cookie, transport, cancellable, error);
        if (!response) goto fail;
        if (!opencode_require_session(response, error)) {
            codexbar_http_response_free(response);
            goto fail;
        }
        if (response_is_null(response)) {
            codexbar_http_response_free(response);
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "OpenCode workspace %s has no subscription quota data", workspace);
            goto fail;
        }
        GError *parse_error = NULL;
        provider = codexbar_opencode_parse_usage(response->body, response->body_length, now_ms, &parse_error);
        codexbar_http_response_free(response);
        if (provider) break;
        if (post) g_propagate_error(error, parse_error);
        else g_clear_error(&parse_error);
    }
    g_free(workspace);
    g_free(cookie);
    return provider;

fail:
    g_free(workspace);
    g_free(cookie);
    return NULL;
}

CodexBarProvider *codexbar_opencode_fetch_with_transport(const CodexBarProviderConfig *config,
                                                         CodexBarWebProvidersTransport transport,
                                                         gint64 now_ms,
                                                         GError **error) {
    return codexbar_opencode_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_opencode_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                           GCancellable *cancellable,
                                                           GError **error) {
    return codexbar_opencode_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_opencode_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_opencode_fetch_with_cancellable(config, NULL, error);
}

/* Devin */

static const char *const devin_token_environment_keys[] = {"DEVIN_BEARER_TOKEN", "DEVIN_AUTHORIZATION", NULL};

static char *devin_token(const CodexBarProviderConfig *config) {
    char *token = first_config_or_environment(config, "cookieHeader", devin_token_environment_keys);
    if (!token) token = raw_string(config, "bearerToken");
    if (!token && config) token = clean_header_credential(config->api_key);
    if (!token) return NULL;
    if (g_ascii_strncasecmp(token, "authorization:", 14) == 0) {
        memmove(token, token + 14, strlen(token + 14) + 1);
        strip_unicode_whitespace(token);
    }
    if (g_ascii_strncasecmp(token, "bearer ", 7) == 0) {
        memmove(token, token + 7, strlen(token + 7) + 1);
        strip_unicode_whitespace(token);
    }
    for (const unsigned char *cursor = (const unsigned char *)token; *cursor; cursor++) {
        if (*cursor < 33 || *cursor == 127) {
            g_free(token);
            return NULL;
        }
    }
    if (token[0] != '\0') return token;
    g_free(token);
    return NULL;
}

gboolean codexbar_devin_has_auth(const CodexBarProviderConfig *config) {
    char *token = devin_token(config);
    gboolean present = token != NULL;
    g_free(token);
    return present;
}

static gboolean valid_devin_org_part(const char *value) {
    if (!value || value[0] == '\0') return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (!(g_ascii_isalnum(*cursor) || *cursor == '.' || *cursor == '_' || *cursor == '-')) return FALSE;
    }
    return TRUE;
}

char *codexbar_devin_normalize_organization(const char *raw) {
    if (!raw || !g_utf8_validate(raw, -1, NULL) || strlen(raw) > 2048) return NULL;
    char *value = g_strdup(raw);
    strip_unicode_whitespace(value);
    if (g_str_has_prefix(value, "https://") || g_str_has_prefix(value, "http://")) {
        GError *uri_error = NULL;
        GUri *uri = g_uri_parse(value, G_URI_FLAGS_NONE, &uri_error);
        g_clear_error(&uri_error);
        const char *host = uri ? g_uri_get_host(uri) : NULL;
        const char *path = uri ? g_uri_get_path(uri) : NULL;
        gboolean allowed_host = host && (g_ascii_strcasecmp(host, "devin.ai") == 0 ||
                                         g_ascii_strcasecmp(host, "app.devin.ai") == 0 ||
                                         g_str_has_suffix(host, ".devin.ai"));
        char *normalized_path = NULL;
        if (allowed_host && path) {
            char **parts = g_strsplit(path, "/", -1);
            for (size_t index = 0; parts[index]; index++) {
                if ((g_str_equal(parts[index], "org") || g_str_equal(parts[index], "organizations")) &&
                    parts[index + 1] && valid_devin_org_part(parts[index + 1])) {
                    normalized_path = g_strdup_printf("%s/%s", parts[index], parts[index + 1]);
                    break;
                }
            }
            g_strfreev(parts);
        }
        if (uri) g_uri_unref(uri);
        g_free(value);
        value = normalized_path;
        if (!value) return NULL;
    }
    g_strstrip(value);
    while (value[0] == '/') memmove(value, value + 1, strlen(value));
    while (value[0] && value[strlen(value) - 1] == '/') value[strlen(value) - 1] = '\0';
    const char *part = value;
    const char *prefix = NULL;
    if (g_str_has_prefix(value, "org/")) {
        prefix = "org";
        part = value + 4;
    } else if (g_str_has_prefix(value, "organizations/")) {
        prefix = "organizations";
        part = value + strlen("organizations/");
    } else if (g_str_has_prefix(value, "org-") || g_str_has_prefix(value, "org_")) {
        prefix = "organizations";
    } else {
        prefix = "org";
    }
    if (!valid_devin_org_part(part)) {
        g_free(value);
        return NULL;
    }
    char *normalized = g_strdup_printf("%s/%s", prefix, part);
    g_free(value);
    return normalized;
}

static char *devin_organization(const CodexBarProviderConfig *config) {
    char *raw = clean_header_credential(config ? config->workspace_id : NULL);
    if (!raw) raw = raw_string(config, "organization");
    if (!raw) raw = raw_string(config, "organizationID");
    if (!raw) raw = clean_header_credential(g_getenv("DEVIN_ORGANIZATION"));
    if (!raw) raw = clean_header_credential(g_getenv("DEVIN_ORG"));
    char *normalized = codexbar_devin_normalize_organization(raw);
    g_free(raw);
    return normalized;
}

static gboolean devin_is_daily_key(const char *key) {
    char *lower = g_ascii_strdown(key, -1);
    gboolean result = !strstr(lower, "hide") && (strstr(lower, "daily") || strstr(lower, "day"));
    g_free(lower);
    return result;
}

static gboolean devin_is_weekly_key(const char *key) {
    char *lower = g_ascii_strdown(key, -1);
    gboolean result = !strstr(lower, "hide") && (strstr(lower, "weekly") || strstr(lower, "week"));
    g_free(lower);
    return result;
}

typedef struct {
    gboolean present;
    double percent;
    gboolean has_reset;
    gint64 reset_ms;
} DevinWindow;

static gboolean devin_percent(json_object *value, double *result) {
    double number = 0;
    if (json_number(value, &number)) {
        *result = number <= 1.0 ? number * 100.0 : number;
        return TRUE;
    }
    if (!value || !json_object_is_type(value, json_type_object)) return FALSE;
    static const char *const direct_keys[] = {
        "used_percent", "usedPercent", "usage_percent", "usagePercent", "percent_used", "percentUsed", "percent",
        NULL,
    };
    if (first_number(value, direct_keys, &number)) {
        *result = number <= 1.0 ? number * 100.0 : number;
        return TRUE;
    }
    static const char *const remaining_keys[] = {
        "remaining_percent", "remainingPercent", "percent_remaining", "percentRemaining", NULL,
    };
    if (first_number(value, remaining_keys, &number)) {
        number = number <= 1.0 ? number * 100.0 : number;
        *result = 100.0 - number;
        return TRUE;
    }
    static const char *const used_keys[] = {"used", "usage", "used_count", "usedCount", "consumed", NULL};
    static const char *const limit_keys[] = {"limit", "quota", "total", "max", "available", NULL};
    double used = 0, limit = 0;
    if (first_number(value, used_keys, &used) && first_number(value, limit_keys, &limit) && limit > 0) {
        *result = used / limit * 100.0;
        return TRUE;
    }
    static const char *const left_keys[] = {"remaining", "left", "available", NULL};
    double remaining = 0;
    if (first_number(value, left_keys, &remaining) && first_number(value, limit_keys, &limit) && limit > 0) {
        *result = (limit - remaining) / limit * 100.0;
        return TRUE;
    }
    return FALSE;
}

static gboolean devin_find_reset(json_object *object, gint64 *reset_ms) {
    if (!object || !json_object_is_type(object, json_type_object)) return FALSE;
    json_object_object_foreach(object, key, value) {
        char *lower = g_ascii_strdown(key, -1);
        gboolean reset_key = strstr(lower, "reset") != NULL;
        g_free(lower);
        if (reset_key && parse_timestamp_ms(value, reset_ms)) return TRUE;
    }
    return FALSE;
}

static gboolean devin_window_from_value(json_object *value, DevinWindow *window, guint depth) {
    if (depth > 8) return FALSE;
    double percent = 0;
    if (devin_percent(value, &percent)) {
        window->present = TRUE;
        window->percent = CLAMP(percent, 0, 100);
        window->has_reset = devin_find_reset(value, &window->reset_ms);
        return TRUE;
    }
    if (value && json_object_is_type(value, json_type_object)) {
        json_object_object_foreach(value, key, child) {
            (void)key;
            if (devin_window_from_value(child, window, depth + 1)) return TRUE;
        }
    }
    return FALSE;
}

static gboolean devin_find_window(json_object *value,
                                  gboolean (*key_matches)(const char *),
                                  DevinWindow *window,
                                  guint depth) {
    if (!value || depth > 8) return FALSE;
    if (json_object_is_type(value, json_type_object)) {
        {
            json_object_object_foreach(value, key, child) {
                if (key_matches(key) && devin_window_from_value(child, window, 0)) return TRUE;
            }
        }
        {
            json_object_object_foreach(value, key, child) {
                (void)key;
                if (devin_find_window(child, key_matches, window, depth + 1)) return TRUE;
            }
        }
    } else if (json_object_is_type(value, json_type_array)) {
        for (size_t index = 0; index < json_object_array_length(value); index++) {
            if (devin_find_window(json_object_array_get_idx(value, index), key_matches, window, depth + 1)) return TRUE;
        }
    }
    return FALSE;
}

static char *title_case_plan(const char *raw) {
    if (!raw) return NULL;
    char **parts = g_strsplit_set(raw, "_-", -1);
    GString *result = g_string_new(NULL);
    for (size_t index = 0; parts[index]; index++) {
        if (parts[index][0] == '\0') continue;
        char *lower = g_utf8_strdown(parts[index], -1);
        char *first = g_utf8_strup(lower, 1);
        if (result->len) g_string_append_c(result, ' ');
        g_string_append(result, first);
        g_string_append(result, g_utf8_next_char(lower));
        g_free(first);
        g_free(lower);
    }
    g_strfreev(parts);
    if (result->len) return g_string_free(result, FALSE);
    g_string_free(result, TRUE);
    return NULL;
}

static char *devin_find_plan(json_object *value, guint depth) {
    if (!value || depth > 8) return NULL;
    if (json_object_is_type(value, json_type_object)) {
        static const char *const keys[] = {
            "plan_name", "planName", "plan", "tier", "subscription_tier", "subscriptionTier", NULL,
        };
        for (size_t index = 0; keys[index]; index++) {
            char *raw = json_clean_string(value, keys[index]);
            if (raw) {
                char *plan = title_case_plan(raw);
                g_free(raw);
                return plan;
            }
        }
        json_object_object_foreach(value, key, child) {
            (void)key;
            char *plan = devin_find_plan(child, depth + 1);
            if (plan) return plan;
        }
    } else if (json_object_is_type(value, json_type_array)) {
        for (size_t index = 0; index < json_object_array_length(value); index++) {
            char *plan = devin_find_plan(json_object_array_get_idx(value, index), depth + 1);
            if (plan) return plan;
        }
    }
    return NULL;
}

CodexBarProvider *codexbar_devin_parse_usage(const char *json,
                                             size_t length,
                                             const char *organization,
                                             gint64 now_ms,
                                             GError **error) {
    json_object *root = parse_json_document(json, length);
    if (!root) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Devin response is invalid JSON");
        return NULL;
    }
    DevinWindow daily = {0}, weekly = {0};
    double percent = 0;
    if (object_number(root, "daily_percentage", &percent)) {
        daily.present = TRUE;
        daily.percent = CLAMP(percent < 1.0 ? percent * 100.0 : percent, 0, 100);
        daily.has_reset = parse_timestamp_ms(object_member(root, "daily_reset_at"), &daily.reset_ms);
    }
    if (object_number(root, "weekly_percentage", &percent)) {
        weekly.present = TRUE;
        weekly.percent = CLAMP(percent < 1.0 ? percent * 100.0 : percent, 0, 100);
        weekly.has_reset = parse_timestamp_ms(object_member(root, "weekly_reset_at"), &weekly.reset_ms);
    }
    if (!daily.present) devin_find_window(root, devin_is_daily_key, &daily, 0);
    if (!weekly.present) devin_find_window(root, devin_is_weekly_key, &weekly, 0);
    if (!daily.present && !weekly.present) {
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Devin response is missing quota windows");
        return NULL;
    }
    CodexBarProvider *provider = new_web_provider("devin", now_ms);
    provider->dashboard_url = g_strdup("https://app.devin.ai/settings/usage");
    provider->explicit_quota_slots = TRUE;
    if (daily.present) {
        CodexBarQuotaWindow *window = new_window("primary", "Daily", daily.percent, 1440, TRUE,
                                                daily.reset_ms, daily.has_reset);
        window->reset_description = g_strdup("Daily");
        codexbar_provider_add_quota_window(provider, window);
    }
    if (weekly.present) {
        CodexBarQuotaWindow *window = new_window("secondary", "Weekly", weekly.percent, 10080, TRUE,
                                                weekly.reset_ms, weekly.has_reset);
        window->reset_description = g_strdup("Weekly");
        codexbar_provider_add_quota_window(provider, window);
    }
    char *plan = devin_find_plan(root, 0);
    char *normalized = codexbar_devin_normalize_organization(organization);
    char *display_org = normalized ? strchr(normalized, '/') : NULL;
    provider->plan = g_strdup(plan);
    if (plan || display_org) {
        provider->identity = g_new0(CodexBarProviderIdentity, 1);
        provider->identity->login_method = plan;
        provider->identity->organization = display_org ? g_strdup(display_org + 1) : NULL;
    } else {
        g_free(plan);
    }
    double balance = 0;
    gboolean has_balance = object_number(root, "overage_balance", &balance);
    if (!has_balance && object_number(root, "overage_balance_cents", &balance)) {
        balance /= 100.0;
        has_balance = TRUE;
    }
    if (has_balance && balance >= 0 && isfinite(balance)) {
        provider->provider_cost = g_new0(CodexBarProviderCost, 1);
        provider->provider_cost->used = balance;
        provider->provider_cost->currency = g_strdup("USD");
        provider->provider_cost->period = g_strdup("Extra usage balance");
        provider->provider_cost->has_updated_at = TRUE;
        provider->provider_cost->updated_at_ms = now_ms;
    }
    g_free(normalized);
    json_object_put(root);
    return provider;
}

static char *devin_internal_org(const char *normalized) {
    return normalized && g_str_has_prefix(normalized, "organizations/")
               ? g_strdup(normalized + strlen("organizations/"))
               : NULL;
}

static void add_unique_string(GPtrArray *array, const char *value) {
    for (guint index = 0; index < array->len; index++) {
        if (g_str_equal(g_ptr_array_index(array, index), value)) return;
    }
    g_ptr_array_add(array, g_strdup(value));
}

CodexBarProvider *codexbar_devin_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProvidersTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *token = devin_token(config);
    if (!token) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Devin requires a manual Bearer token on Linux");
        return NULL;
    }
    char *organization = devin_organization(config);
    if (!organization) {
        g_free(token);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Devin organization is missing or invalid");
        return NULL;
    }
    char *internal_org = devin_internal_org(organization);
    GPtrArray *paths = g_ptr_array_new_with_free_func(g_free);
    if (internal_org) {
        char *path = g_strdup_printf("%s/billing/quota/usage", internal_org);
        add_unique_string(paths, path);
        g_free(path);
    }
    char *path = g_strdup_printf("%s/billing/quota/usage", organization);
    add_unique_string(paths, path);
    g_free(path);
    if (g_str_has_prefix(organization, "org/")) {
        path = g_strdup_printf("%s/billing/quota/usage", organization + 4);
        add_unique_string(paths, path);
        g_free(path);
    }
    if (internal_org) {
        path = g_strdup_printf("organizations/%s/billing/quota/usage", internal_org);
        add_unique_string(paths, path);
        g_free(path);
    }
    char *authorization = g_strdup_printf("Bearer %s", token);
    CodexBarHttpRequestHeader headers[] = {
        {"Accept", "application/json"},
        {"Accept-Language", "en-US,en;q=0.9"},
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/143.0.0.0 Safari/537.36"},
        {"Authorization", authorization},
        {"x-cog-org-id", internal_org},
    };
    CodexBarProvider *provider = NULL;
    GError *last_error = NULL;
    for (guint index = 0; index < paths->len && !provider; index++) {
        char *url = g_strdup_printf(DEVIN_BASE_URL "/%s", (char *)g_ptr_array_index(paths, index));
        CodexBarHttpResponse *response = web_get(url,
                                                 headers,
                                                 internal_org ? G_N_ELEMENTS(headers) : G_N_ELEMENTS(headers) - 1,
                                                 transport,
                                                 cancellable,
                                                 &last_error);
        g_free(url);
        if (!response) break;
        if (response->status == 401 || response->status == 403) {
            codexbar_http_response_free(response);
            g_clear_error(&last_error);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                "Devin credentials are invalid or expired");
            goto cleanup;
        }
        if (response->status != 200) {
            g_clear_error(&last_error);
            g_set_error(&last_error, G_IO_ERROR, G_IO_ERROR_FAILED,
                        "Devin API returned HTTP %ld", response->status);
            codexbar_http_response_free(response);
            continue;
        }
        provider = codexbar_devin_parse_usage(response->body,
                                              response->body_length,
                                              organization,
                                              now_ms,
                                              &last_error);
        codexbar_http_response_free(response);
        if (!provider) break;
    }
    if (!provider) {
        if (last_error) g_propagate_error(error, last_error);
        else g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "No Devin quota endpoint succeeded");
    }

cleanup:
    g_free(authorization);
    g_ptr_array_unref(paths);
    g_free(internal_org);
    g_free(organization);
    g_free(token);
    return provider;
}

CodexBarProvider *codexbar_devin_fetch_with_transport(const CodexBarProviderConfig *config,
                                                      CodexBarWebProvidersTransport transport,
                                                      gint64 now_ms,
                                                      GError **error) {
    return codexbar_devin_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_devin_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                        GCancellable *cancellable,
                                                        GError **error) {
    return codexbar_devin_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_devin_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_devin_fetch_with_cancellable(config, NULL, error);
}
