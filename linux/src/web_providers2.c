#include "web_providers2.h"
#include "process.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define WEB2_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)
#define WEB2_MAXIMUM_CREDENTIAL_BYTES 16384U
#define WEB2_TIMEOUT_SECONDS 15
#define MANUS_CREDITS_URL "https://api.manus.im/user.v1.UserService/GetAvailableCredits"
#define AMP_SETTINGS_URL "https://ampcode.com/settings"
#define AMP_USAGE_URL "https://ampcode.com/api/internal?userDisplayBalanceInfo"
#define T3CHAT_USAGE_BASE_URL "https://t3.chat/api/trpc/getCustomerData"

static gboolean json_whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

static json_object *parse_json_document(const char *json, size_t length) {
    if (!json || length == 0 || length > G_MAXINT || length > WEB2_MAXIMUM_RESPONSE_BYTES ||
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

static char *clean_header_value(const char *raw) {
    if (!raw || !g_utf8_validate(raw, -1, NULL) || strlen(raw) > WEB2_MAXIMUM_CREDENTIAL_BYTES) return NULL;
    char *value = g_strdup(raw);
    strip_unicode_whitespace(value);
    size_t length = strlen(value);
    if (length >= 2 && ((value[0] == '\'' && value[length - 1] == '\'') ||
                        (value[0] == '"' && value[length - 1] == '"'))) {
        value[length - 1] = '\0';
        memmove(value, value + 1, length - 1);
        strip_unicode_whitespace(value);
    }
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
    const char *raw = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!raw || length > WEB2_MAXIMUM_CREDENTIAL_BYTES || memchr(raw, '\0', length) ||
        !g_utf8_validate(raw, (gssize)length, NULL)) {
        return NULL;
    }
    char *copy = g_strndup(raw, length);
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
        value = clean_header_value(g_getenv(environment_keys[index]));
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

static char *normalize_cookie_header(const char *raw, const char *required_name) {
    char *header = clean_header_value(raw);
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
        if (required_name && g_ascii_strcasecmp(name, required_name) != 0) continue;
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

static gboolean json_number(json_object *value, gboolean allow_string, double *result) {
    if (!value || json_object_is_type(value, json_type_null) || json_object_is_type(value, json_type_boolean)) {
        return FALSE;
    }
    double number = 0;
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        number = json_object_get_double(value);
    } else if (allow_string && json_object_is_type(value, json_type_string)) {
        const char *raw = json_object_get_string(value);
        size_t length = (size_t)json_object_get_string_len(value);
        if (!raw || memchr(raw, '\0', length)) return FALSE;
        char *copy = g_strndup(raw, length);
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

static gboolean object_number(json_object *object, const char *key, gboolean allow_string, double *result) {
    return json_number(object_member(object, key), allow_string, result);
}

static char *json_clean_string(json_object *object, const char *key) {
    json_object *value = object_member(object, key);
    if (!value || !json_object_is_type(value, json_type_string)) return NULL;
    const char *raw = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!raw || memchr(raw, '\0', length) || !g_utf8_validate(raw, (gssize)length, NULL)) return NULL;
    char *copy = g_strndup(raw, length);
    strip_unicode_whitespace(copy);
    if (copy[0] != '\0') return copy;
    g_free(copy);
    return NULL;
}

static gboolean parse_timestamp_ms(json_object *value, gint64 *result) {
    double number = 0;
    if (json_number(value, FALSE, &number)) {
        if (number <= 0 || number > (double)G_MAXINT64) return FALSE;
        if (number < 100000000000.0) number *= 1000.0;
        if (number > (double)G_MAXINT64) return FALSE;
        *result = (gint64)llround(number);
        return TRUE;
    }
    if (!value || !json_object_is_type(value, json_type_string)) return FALSE;
    const char *raw = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!raw || memchr(raw, '\0', length)) return FALSE;
    char *copy = g_strndup(raw, length);
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
                                          CodexBarWebProviders2Transport transport,
                                          GError **error) {
    if (!transport) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Provider transport is missing");
        return NULL;
    }
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

static CodexBarProvider *new_provider(const char *provider_id, const char *source, gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup(provider_id);
    provider->source = g_strdup(source);
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
                                       gboolean has_resets_at,
                                       const char *detail) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = CLAMP(used_percent, 0, 100);
    window->has_window_minutes = has_window_minutes;
    window->window_minutes = window_minutes;
    window->has_resets_at = has_resets_at;
    window->resets_at_ms = resets_at_ms;
    window->detail = g_strdup(detail);
    return window;
}

static gboolean require_success(const CodexBarHttpResponse *response, const char *provider, GError **error) {
    if (response->status == 200) return TRUE;
    if (response->status == 401 || response->status == 403) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s credentials are invalid or expired", provider);
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s API returned HTTP %ld", provider, response->status);
    }
    return FALSE;
}

/* Manus */

static const char *const manus_token_environment_keys[] = {
    "MANUS_SESSION_TOKEN", "manus_session_token", "MANUS_SESSION_ID", "manus_session_id", NULL};
static const char *const manus_cookie_environment_keys[] = {"MANUS_COOKIE", "manus_cookie", NULL};

char *codexbar_manus_extract_session_token(const char *raw) {
    char *value = clean_header_value(raw);
    if (!value) return NULL;
    if (!strchr(value, '=') && !strchr(value, ';')) return value;
    char *cookie = normalize_cookie_header(value, "session_id");
    g_free(value);
    if (!cookie) return NULL;
    char *separator = strchr(cookie, '=');
    char *token = separator && separator[1] ? g_strdup(separator + 1) : NULL;
    g_free(cookie);
    return token;
}

static char *manus_token(const CodexBarProviderConfig *config) {
    char *raw = first_config_or_environment(config, "cookieHeader", manus_token_environment_keys);
    char *token = codexbar_manus_extract_session_token(raw);
    g_free(raw);
    if (token) return token;
    raw = first_config_or_environment(config, "cookieHeader", manus_cookie_environment_keys);
    token = codexbar_manus_extract_session_token(raw);
    g_free(raw);
    return token;
}

gboolean codexbar_manus_has_auth(const CodexBarProviderConfig *config) {
    char *token = manus_token(config);
    g_free(token);
    return token != NULL;
}

static gboolean manus_has_credit_key(json_object *object) {
    static const char *const keys[] = {
        "totalCredits", "freeCredits", "periodicCredits", "addonCredits", "refreshCredits",
        "maxRefreshCredits", "proMonthlyCredits", "eventCredits", NULL};
    for (size_t index = 0; keys[index]; index++) {
        if (object_member(object, keys[index])) return TRUE;
    }
    return FALSE;
}

static json_object *manus_credit_object(json_object *root) {
    static const char *const envelopes[] = {"data", "result", "response", "availableCredits", NULL};
    for (size_t index = 0; envelopes[index]; index++) {
        json_object *value = object_member(root, envelopes[index]);
        if (value && json_object_is_type(value, json_type_object)) return value;
    }
    return root;
}

static char *format_credit_detail(double total, double free_credits) {
    return g_strdup_printf("Total %.0f • Free %.0f", round(total), round(free_credits));
}

CodexBarProvider *codexbar_manus_parse_credits(const char *json,
                                               size_t length,
                                               gint64 now_ms,
                                               GError **error) {
    json_object *root = parse_json_document(json, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Manus credits response is invalid JSON");
        return NULL;
    }
    json_object *credits = manus_credit_object(root);
    if (!manus_has_credit_key(credits)) {
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Manus response is missing expected credits fields");
        return NULL;
    }
    double total = 0, free_credits = 0, periodic = 0, refresh = 0, max_refresh = 0, monthly = 0;
    object_number(credits, "totalCredits", TRUE, &total);
    object_number(credits, "freeCredits", TRUE, &free_credits);
    object_number(credits, "periodicCredits", TRUE, &periodic);
    object_number(credits, "refreshCredits", TRUE, &refresh);
    object_number(credits, "maxRefreshCredits", TRUE, &max_refresh);
    object_number(credits, "proMonthlyCredits", TRUE, &monthly);
    gint64 next_refresh_ms = 0;
    gboolean has_next_refresh = parse_timestamp_ms(object_member(credits, "nextRefreshTime"), &next_refresh_ms);
    char *interval = json_clean_string(credits, "refreshInterval");

    CodexBarProvider *provider = new_provider("manus", "web", now_ms);
    provider->dashboard_url = g_strdup("https://manus.im");
    provider->explicit_quota_slots = TRUE;
    if (monthly > 0) {
        char *detail = format_credit_detail(total, free_credits);
        codexbar_provider_add_quota_window(
            provider,
            new_window("primary", "Monthly credits", (monthly - periodic) / monthly * 100.0,
                       0, FALSE, 0, FALSE, detail));
        g_free(detail);
    }
    if (max_refresh > 0) {
        char *title = interval ? g_strdup_printf("%c%s: %.0f / %.0f",
                                                 g_ascii_toupper(interval[0]), interval + 1,
                                                 round(refresh), round(max_refresh))
                               : g_strdup_printf("%.0f / %.0f", round(refresh), round(max_refresh));
        codexbar_provider_add_quota_window(
            provider,
            new_window("secondary", "Daily refresh", (max_refresh - refresh) / max_refresh * 100.0,
                       0, FALSE, next_refresh_ms, has_next_refresh, title));
        g_free(title);
    }
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = g_strdup_printf("Balance: %.0f credits", round(total));
    g_free(interval);
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_manus_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders2Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *token = manus_token(config);
    if (!token) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No Manus session token is configured");
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", token);
    CodexBarHttpRequestHeader headers[] = {
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"Authorization", authorization},
        {"Origin", "https://manus.im"},
        {"Referer", "https://manus.im/"},
        {"Connect-Protocol-Version", "1"},
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
                       "Chrome/135.0.0.0 Safari/537.36"},
    };
    static const char body[] = "{}";
    CodexBarHttpRequest request = {
        .url = MANUS_CREDITS_URL,
        .method = "POST",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .body = body,
        .body_length = sizeof(body) - 1,
        .timeout_seconds = WEB2_TIMEOUT_SECONDS,
        .maximum_response_bytes = WEB2_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    g_free(authorization);
    g_free(token);
    if (!response || !require_success(response, "Manus", error)) {
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_manus_parse_credits(
        response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

CodexBarProvider *codexbar_manus_fetch_with_transport(const CodexBarProviderConfig *config,
                                                      CodexBarWebProviders2Transport transport,
                                                      gint64 now_ms,
                                                      GError **error) {
    return codexbar_manus_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_manus_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                        GCancellable *cancellable,
                                                        GError **error) {
    return codexbar_manus_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_manus_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_manus_fetch_with_cancellable(config, NULL, error);
}

/* Amp */

static const char *const amp_token_environment_keys[] = {"AMP_API_KEY", NULL};
static const char *const amp_cookie_environment_keys[] = {
    "AMP_COOKIE_HEADER", "CODEXBAR_AMP_COOKIE_HEADER", "AMP_COOKIE", NULL};

static char *amp_binary(void) {
    const char *override = g_getenv("AMP_CLI_PATH");
    if (override) {
        char *path = clean_header_value(override);
        if (path && g_path_is_absolute(path) && g_file_test(path, G_FILE_TEST_IS_EXECUTABLE)) return path;
        g_free(path);
        return NULL;
    }
    char *path = g_find_program_in_path("amp");
    if (path) return path;
    const char *relative_paths[] = {".local/bin/amp", ".amp/bin/amp"};
    for (size_t index = 0; index < G_N_ELEMENTS(relative_paths); index++) {
        path = g_build_filename(g_get_home_dir(), relative_paths[index], NULL);
        if (g_file_test(path, G_FILE_TEST_IS_EXECUTABLE)) return path;
        g_free(path);
    }
    const char *system_paths[] = {"/usr/local/bin/amp", "/opt/homebrew/bin/amp"};
    for (size_t index = 0; index < G_N_ELEMENTS(system_paths); index++) {
        if (g_file_test(system_paths[index], G_FILE_TEST_IS_EXECUTABLE)) return g_strdup(system_paths[index]);
    }
    return NULL;
}

static char *amp_api_token(const CodexBarProviderConfig *config) {
    char *value = config && config->api_key ? clean_header_value(config->api_key) : NULL;
    for (size_t index = 0; !value && amp_token_environment_keys[index]; index++) {
        value = clean_header_value(g_getenv(amp_token_environment_keys[index]));
    }
    return value;
}

static char *amp_cookie(const CodexBarProviderConfig *config) {
    char *raw = first_config_or_environment(config, "cookieHeader", amp_cookie_environment_keys);
    char *cookie = normalize_cookie_header(raw, "session");
    g_free(raw);
    return cookie;
}

gboolean codexbar_amp_has_auth(const CodexBarProviderConfig *config) {
    char *token = amp_api_token(config);
    char *cookie = amp_cookie(config);
    gboolean available = token || cookie;
    g_free(token);
    g_free(cookie);
    return available;
}

static char **regex_captures(const char *text, const char *pattern, guint capture_count) {
    GError *regex_error = NULL;
    GRegex *regex = g_regex_new(pattern, G_REGEX_MULTILINE | G_REGEX_CASELESS, 0, &regex_error);
    if (!regex) {
        g_clear_error(&regex_error);
        return NULL;
    }
    GMatchInfo *match = NULL;
    gboolean found = g_regex_match(regex, text, 0, &match);
    if (!found) {
        g_match_info_free(match);
        g_regex_unref(regex);
        return NULL;
    }
    char **captures = g_new0(char *, capture_count + 1);
    for (guint index = 0; index < capture_count; index++) {
        captures[index] = g_match_info_fetch(match, (gint)index + 1);
        if (captures[index]) g_strstrip(captures[index]);
    }
    g_match_info_free(match);
    g_regex_unref(regex);
    return captures;
}

static void free_captures(char **captures) {
    g_strfreev(captures);
}

static gboolean parse_display_number(const char *text, double *result) {
    if (!text || text[0] == '\0') return FALSE;
    char *copy = g_strdup(text);
    char *write = copy;
    for (char *read = copy; *read; read++) {
        if (*read != ',') *write++ = *read;
    }
    *write = '\0';
    char *end = NULL;
    double value = g_ascii_strtod(copy, &end);
    gboolean valid = copy[0] != '\0' && end && *end == '\0' && isfinite(value);
    g_free(copy);
    if (valid) *result = value;
    return valid;
}

static char *strip_ansi(const char *text) {
    GError *regex_error = NULL;
    GRegex *regex = g_regex_new("\\x1B(?:\\[[0-?]*[ -/]*[@-~]|\\][^\\x07]*(?:\\x07|\\x1B\\\\))",
                                G_REGEX_OPTIMIZE, 0, &regex_error);
    if (!regex) {
        g_clear_error(&regex_error);
        return g_strdup(text);
    }
    char *result = g_regex_replace(regex, text, -1, 0, "", 0, NULL);
    g_regex_unref(regex);
    return result;
}

static void amp_add_workspace_balances(CodexBarProvider *provider, const char *text) {
    GError *regex_error = NULL;
    GRegex *regex = g_regex_new(
        "^\\s*Workspace\\s+(.+?):\\s*\\$?([0-9][0-9,]*(?:\\.[0-9]+)?)\\s+remaining",
        G_REGEX_MULTILINE | G_REGEX_CASELESS, 0, &regex_error);
    if (!regex) {
        g_clear_error(&regex_error);
        return;
    }
    GMatchInfo *match = NULL;
    g_regex_match(regex, text, 0, &match);
    while (g_match_info_matches(match)) {
        char *name = g_match_info_fetch(match, 1);
        char *raw_remaining = g_match_info_fetch(match, 2);
        double remaining = 0;
        g_strstrip(name);
        if (name[0] != '\0' && parse_display_number(raw_remaining, &remaining)) {
            char *id = g_ascii_strdown(name, -1);
            for (char *cursor = id; *cursor; cursor++) {
                if (!g_ascii_isalnum(*cursor)) *cursor = '-';
            }
            CodexBarBalance *balance = codexbar_balance_new(id, name, remaining, "USD");
            codexbar_provider_add_balance(provider, balance);
            g_free(id);
        }
        g_free(raw_remaining);
        g_free(name);
        if (!g_match_info_next(match, NULL)) break;
    }
    g_match_info_free(match);
    g_regex_unref(regex);
}

static CodexBarProvider *amp_parse_display_text_with_source(const char *text,
                                                           size_t length,
                                                           const char *source,
                                                           gint64 now_ms,
                                                           GError **error) {
    if (!text || length == 0 || length > WEB2_MAXIMUM_RESPONSE_BYTES || memchr(text, '\0', length) ||
        !g_utf8_validate(text, (gssize)length, NULL)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Amp usage text is invalid");
        return NULL;
    }
    char *input = g_strndup(text, length);
    char *clean = strip_ansi(input);
    g_free(input);
    char **bold_parts = g_strsplit(clean, "**", -1);
    g_free(clean);
    clean = g_strjoinv("", bold_parts);
    g_strfreev(bold_parts);
    static const char absolute_pattern[] =
        "^\\s*Amp Free:\\s*\\$?([0-9][0-9,]*(?:\\.[0-9]+)?)\\s*/\\s*\\$?"
        "([0-9][0-9,]*(?:\\.[0-9]+)?)\\s+remaining(?:\\s*\\(replenishes\\s*\\+\\$?"
        "([0-9][0-9,]*(?:\\.[0-9]+)?)\\s*/\\s*hour\\))?";
    static const char percent_pattern[] =
        "^\\s*Amp Free:\\s*([0-9][0-9,]*(?:\\.[0-9]+)?)\\s*%\\s+remaining(?:\\s+today)?"
        "(?:\\s*(\\(resets\\s+daily\\)))?";
    static const char legacy_subscription_pattern[] =
        "^\\s*Subscription\\s+(.+?):\\s*([0-9][0-9,]*(?:\\.[0-9]+)?)\\s*%\\s+other\\s+usage"
        "\\s+and\\s+([0-9][0-9,]*(?:\\.[0-9]+)?)\\s*%\\s+orb\\s+usage\\s+remaining\\s*-"
        "\\s*resets\\s+upon\\s+renewal\\s+in\\s+([0-9][0-9,]*)\\s+(days?|months?)"
        "(?:\\s*-\\s*https?://\\S+)?\\s*$";
    static const char current_subscription_pattern[] =
        "^\\s*Amp\\s+(.+?)\\s+Subscription:\\s*([0-9][0-9,]*(?:\\.[0-9]+)?)\\s*%\\s+other\\s+usage"
        "\\s+and\\s+([0-9][0-9,]*(?:\\.[0-9]+)?)\\s*%\\s+orb\\s+usage\\s+remaining\\s*-"
        "\\s*resets\\s+upon\\s+renewal\\s+in\\s+([0-9][0-9,]*)\\s+(days?|months?)"
        "(?:\\s*-\\s*https?://\\S+)?\\s*$";
    char **absolute = regex_captures(clean, absolute_pattern, 3);
    char **percent = absolute ? NULL : regex_captures(clean, percent_pattern, 2);
    char **subscription = regex_captures(clean, legacy_subscription_pattern, 5);
    if (!subscription) subscription = regex_captures(clean, current_subscription_pattern, 5);
    char **identity = regex_captures(clean,
        "^\\s*Signed in as\\s+([^\\s(]+)(?:\\s+\\(([^\\r\\n)]+)\\))?\\s*$", 2);
    char **individual = regex_captures(clean,
        "^\\s*Individual credits:\\s*\\$?([0-9][0-9,]*(?:\\.[0-9]+)?)\\s+remaining", 1);

    gboolean has_free = FALSE;
    double free_remaining = 0, free_quota = 0, hourly = 0;
    const char *free_detail = NULL;
    if (absolute && parse_display_number(absolute[0], &free_remaining) &&
        parse_display_number(absolute[1], &free_quota)) {
        has_free = TRUE;
        parse_display_number(absolute[2], &hourly);
    } else if (percent && parse_display_number(percent[0], &free_remaining)) {
        has_free = TRUE;
        free_remaining = CLAMP(free_remaining, 0, 100);
        free_quota = 100;
        free_detail = percent[1] && percent[1][0] != '\0' ? "resets daily" : NULL;
    }
    gboolean has_subscription = FALSE;
    double other_remaining = 0, orb_remaining = 0, renewal_value = 0;
    if (subscription && parse_display_number(subscription[1], &other_remaining) &&
        parse_display_number(subscription[2], &orb_remaining) &&
        parse_display_number(subscription[3], &renewal_value) && subscription[0][0] != '\0' &&
        subscription[4] && subscription[4][0] != '\0') {
        has_subscription = TRUE;
    }
    double individual_remaining = 0;
    gboolean has_individual = individual && parse_display_number(individual[0], &individual_remaining);
    if (!has_free && !has_subscription && !has_individual && !strstr(clean, "Workspace ")) {
        free_captures(individual);
        free_captures(identity);
        free_captures(subscription);
        free_captures(percent);
        free_captures(absolute);
        g_free(clean);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Amp usage data is missing");
        return NULL;
    }

    CodexBarProvider *provider = new_provider("amp", source, now_ms);
    provider->dashboard_url = g_strdup("https://ampcode.com/settings/usage");
    provider->explicit_quota_slots = TRUE;
    if (has_subscription) {
        gboolean uses_months = g_ascii_strncasecmp(subscription[4], "month", 5) == 0;
        gint64 resets_at_ms = 0;
        if (uses_months) {
            GDateTime *now = g_date_time_new_from_unix_utc(now_ms / 1000);
            GDateTime *reset = now ? g_date_time_add_months(now, (gint)llround(renewal_value)) : NULL;
            if (reset) resets_at_ms = g_date_time_to_unix(reset) * 1000;
            if (reset) g_date_time_unref(reset);
            if (now) g_date_time_unref(now);
        } else {
            resets_at_ms = now_ms + (gint64)llround(renewal_value * 86400000.0);
        }
        const char *unit = uses_months ? "month" : "day";
        char *detail = renewal_value == 1
                           ? g_strdup_printf("renews in 1 %s", unit)
                           : g_strdup_printf("renews in %.0f %ss", renewal_value, unit);
        CodexBarQuotaWindow *primary = new_window(
            "primary", "Other usage", 100 - CLAMP(other_remaining, 0, 100),
            30 * 24 * 60, TRUE, resets_at_ms, resets_at_ms > 0, NULL);
        CodexBarQuotaWindow *secondary = new_window(
            "secondary", "Orb usage", 100 - CLAMP(orb_remaining, 0, 100),
            30 * 24 * 60, TRUE, resets_at_ms, resets_at_ms > 0, NULL);
        primary->reset_description = g_strdup(detail);
        secondary->reset_description = g_strdup(detail);
        codexbar_provider_add_quota_window(provider, primary);
        codexbar_provider_add_quota_window(provider, secondary);
        provider->plan = g_strdup(subscription[0]);
        g_free(detail);
    } else if (has_free) {
        double used = MAX(0, free_quota - free_remaining);
        gint64 window_minutes = hourly > 0 ? (gint64)llround(MAX(1, free_quota / hourly) * 60.0) : 24 * 60;
        gboolean has_reset = hourly > 0;
        gint64 resets_at_ms = has_reset ? now_ms + (gint64)llround(used / hourly * 3600000.0) : 0;
        CodexBarQuotaWindow *window = new_window(
            "primary", "Amp Free", free_quota > 0 ? used / free_quota * 100.0 : 0,
            window_minutes, TRUE, resets_at_ms, has_reset, NULL);
        window->reset_description = g_strdup(free_detail);
        codexbar_provider_add_quota_window(provider, window);
    }
    if (has_individual) {
        codexbar_provider_add_balance(
            provider, codexbar_balance_new("individual", "Individual credits", individual_remaining, "USD"));
    }
    amp_add_workspace_balances(provider, clean);
    if (identity && identity[0] && identity[0][0] != '\0') provider->account = g_strdup(identity[0]);
    if (identity || has_subscription || has_free) {
        provider->identity = g_new0(CodexBarProviderIdentity, 1);
        if (identity && identity[1] && identity[1][0] != '\0') {
            provider->identity->organization = g_strdup(identity[1]);
        }
        provider->identity->login_method = g_strdup(
            has_subscription ? subscription[0] : (has_free ? "Amp Free" : "Amp"));
    }

    free_captures(individual);
    free_captures(identity);
    free_captures(subscription);
    free_captures(percent);
    free_captures(absolute);
    g_free(clean);
    return provider;
}

CodexBarProvider *codexbar_amp_parse_display_text(const char *text,
                                                  size_t length,
                                                  gint64 now_ms,
                                                  GError **error) {
    return amp_parse_display_text_with_source(text, length, "api", now_ms, error);
}

CodexBarProvider *codexbar_amp_parse_usage_api(const char *json,
                                               size_t length,
                                               gint64 now_ms,
                                               GError **error) {
    json_object *root = parse_json_document(json, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Amp usage API response is invalid JSON");
        return NULL;
    }
    json_object *ok = object_member(root, "ok");
    if (!ok || !json_object_is_type(ok, json_type_boolean)) {
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Amp usage API response is missing ok");
        return NULL;
    }
    if (!json_object_get_boolean(ok)) {
        json_object *api_error = object_member(root, "error");
        char *code = json_clean_string(api_error, "code");
        char *message = json_clean_string(api_error, "message");
        if (code && g_str_equal(code, "auth-required")) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                "Amp access token is invalid or expired");
        } else {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Amp usage API returned an error: %s",
                        message ? message : "unknown error");
        }
        g_free(message);
        g_free(code);
        json_object_put(root);
        return NULL;
    }
    char *display = json_clean_string(object_member(root, "result"), "displayText");
    if (!display) {
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Amp usage API response is missing displayText");
        return NULL;
    }
    CodexBarProvider *provider = amp_parse_display_text_with_source(
        display, strlen(display), "api", now_ms, error);
    g_free(display);
    json_object_put(root);
    return provider;
}

static gboolean amp_html_number(const char *object, const char *key, double *result) {
    char *escaped = g_regex_escape_string(key, -1);
    char *pattern = g_strdup_printf("(?:[\\\"']?%s[\\\"']?)\\s*:\\s*([0-9]+(?:\\.[0-9]+)?)", escaped);
    char **captures = regex_captures(object, pattern, 1);
    gboolean parsed = captures && parse_display_number(captures[0], result);
    free_captures(captures);
    g_free(pattern);
    g_free(escaped);
    return parsed;
}

static char *amp_extract_usage_object(const char *html) {
    static const char *const tokens[] = {"freeTierUsage", "getFreeTierUsage", NULL};
    const char *brace = NULL;
    for (size_t index = 0; tokens[index] && !brace; index++) {
        const char *token = strstr(html, tokens[index]);
        if (token) brace = strchr(token + strlen(tokens[index]), '{');
    }
    if (!brace) return NULL;
    guint depth = 0;
    gboolean in_string = FALSE;
    gboolean escaped = FALSE;
    char quote = '\0';
    for (const char *cursor = brace; *cursor; cursor++) {
        if (in_string) {
            if (escaped) escaped = FALSE;
            else if (*cursor == '\\') escaped = TRUE;
            else if (*cursor == quote) in_string = FALSE;
            continue;
        }
        if (*cursor == '"' || *cursor == '\'') {
            in_string = TRUE;
            quote = *cursor;
        } else if (*cursor == '{') {
            depth++;
        } else if (*cursor == '}') {
            if (depth == 0) return NULL;
            depth--;
            if (depth == 0) return g_strndup(brace, (size_t)(cursor - brace + 1));
        }
    }
    return NULL;
}

CodexBarProvider *codexbar_amp_parse_settings_html(const char *html,
                                                   size_t length,
                                                   gint64 now_ms,
                                                   GError **error) {
    if (!html || length == 0 || length > WEB2_MAXIMUM_RESPONSE_BYTES || memchr(html, '\0', length) ||
        !g_utf8_validate(html, (gssize)length, NULL)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Amp settings response is invalid");
        return NULL;
    }
    char *document = g_strndup(html, length);
    char *object = amp_extract_usage_object(document);
    if (!object) {
        char *lower = g_ascii_strdown(document, -1);
        gboolean signed_out = strstr(lower, "sign in") || strstr(lower, "log in") || strstr(lower, "/login");
        g_free(lower);
        g_free(document);
        g_set_error_literal(error, G_IO_ERROR,
                            signed_out ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_INVALID_DATA,
                            signed_out ? "Amp session cookie is invalid or expired" : "Amp Free usage data is missing");
        return NULL;
    }
    double quota = 0, used = 0, hourly = 0, window_hours = 0;
    gboolean valid = amp_html_number(object, "quota", &quota) && amp_html_number(object, "used", &used) &&
                     amp_html_number(object, "hourlyReplenishment", &hourly);
    gboolean has_window = amp_html_number(object, "windowHours", &window_hours);
    g_free(object);
    g_free(document);
    if (!valid) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Amp Free usage data is malformed");
        return NULL;
    }
    CodexBarProvider *provider = new_provider("amp", "web", now_ms);
    provider->dashboard_url = g_strdup("https://ampcode.com/settings/usage");
    provider->explicit_quota_slots = TRUE;
    gint64 window_minutes = has_window && window_hours > 0 ? (gint64)llround(window_hours * 60.0) : 0;
    gboolean has_reset = quota > 0 && hourly > 0;
    gint64 resets_at_ms = has_reset ? now_ms + (gint64)llround(MAX(0, used) / hourly * 3600000.0) : 0;
    codexbar_provider_add_quota_window(
        provider,
        new_window("primary", "Amp Free", quota > 0 ? MAX(0, used) / quota * 100.0 : 0,
                   window_minutes, has_window && window_hours > 0, resets_at_ms, has_reset, NULL));
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = g_strdup("Amp Free");
    return provider;
}

static CodexBarProvider *amp_fetch_api(const char *token,
                                      CodexBarWebProviders2Transport transport,
                                      GCancellable *cancellable,
                                      gint64 now_ms,
                                      GError **error) {
    char *authorization = g_strdup_printf("Bearer %s", token);
    CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization}, {"Accept", "application/json"}, {"Content-Type", "application/json"}};
    static const char body[] = "{\"method\":\"userDisplayBalanceInfo\",\"params\":{}}";
    CodexBarHttpRequest request = {
        .url = AMP_USAGE_URL,
        .method = "POST",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .body = body,
        .body_length = sizeof(body) - 1,
        .timeout_seconds = WEB2_TIMEOUT_SECONDS,
        .maximum_response_bytes = WEB2_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    g_free(authorization);
    if (!response || !require_success(response, "Amp", error)) {
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_amp_parse_usage_api(
        response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

static CodexBarProvider *amp_fetch_web(const char *cookie,
                                      CodexBarWebProviders2Transport transport,
                                      GCancellable *cancellable,
                                      gint64 now_ms,
                                      GError **error) {
    CodexBarHttpRequestHeader headers[] = {
        {"Cookie", cookie},
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
                       "Chrome/143.0.0.0 Safari/537.36"},
        {"Accept-Language", "en-US,en;q=0.9"},
        {"Origin", "https://ampcode.com"},
        {"Referer", AMP_SETTINGS_URL},
    };
    CodexBarHttpRequest request = {
        .url = AMP_SETTINGS_URL,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = WEB2_TIMEOUT_SECONDS,
        .maximum_response_bytes = WEB2_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    if (!response || !require_success(response, "Amp", error)) {
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_amp_parse_settings_html(
        response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

static CodexBarProvider *amp_fetch_cli(GCancellable *cancellable, gint64 now_ms, GError **error) {
    char *binary = amp_binary();
    if (!binary) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Amp CLI was not found");
        return NULL;
    }
    char **environment = g_get_environ();
    environment = g_environ_setenv(environment, "NO_COLOR", "1", TRUE);
    const char *arguments[] = {binary, "usage", NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .environment = (const char *const *)environment,
        .timeout_milliseconds = 15000,
        .termination_grace_milliseconds = 300,
        .maximum_output_bytes = WEB2_MAXIMUM_RESPONSE_BYTES,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, cancellable, error);
    g_strfreev(environment);
    g_free(binary);
    if (!result) return NULL;
    if (!codexbar_process_result_succeeded(result)) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Amp CLI exited with status %d",
                    result->exit_status);
        codexbar_process_result_free(result);
        return NULL;
    }
    const char *output = result->standard_output;
    size_t length = result->standard_output_length;
    char *trimmed = g_strndup(output, length);
    strip_unicode_whitespace(trimmed);
    if (trimmed[0] == '\0') {
        g_free(trimmed);
        output = result->standard_error;
        length = result->standard_error_length;
    } else {
        g_free(trimmed);
    }
    CodexBarProvider *provider = amp_parse_display_text_with_source(output, length, "cli", now_ms, error);
    codexbar_process_result_free(result);
    return provider;
}

CodexBarProvider *codexbar_amp_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders2Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    const char *source = config ? config->source : NULL;
    gboolean wants_cli = !source || g_str_equal(source, "auto") || g_str_equal(source, "cli");
    gboolean wants_api = !source || g_str_equal(source, "auto") || g_str_equal(source, "api");
    gboolean wants_web = !source || g_str_equal(source, "auto") || g_str_equal(source, "web");
    if (wants_cli) {
        CodexBarProvider *cli_provider = amp_fetch_cli(cancellable, now_ms, error);
        if (cli_provider || (source && g_str_equal(source, "cli")) ||
            (cancellable && g_cancellable_is_cancelled(cancellable))) {
            return cli_provider;
        }
        if (error && *error) g_clear_error(error);
    }
    char *token = wants_api ? amp_api_token(config) : NULL;
    char *cookie = wants_web ? amp_cookie(config) : NULL;
    if (!token && !cookie) {
        g_free(cookie);
        g_free(token);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "No Amp access token or session cookie is configured");
        return NULL;
    }
    CodexBarProvider *provider = NULL;
    if (token) provider = amp_fetch_api(token, transport, cancellable, now_ms, error);
    if (!provider && cookie && (!cancellable || !g_cancellable_is_cancelled(cancellable)) &&
        (!source || g_str_equal(source, "auto"))) {
        if (error && *error) g_clear_error(error);
        provider = amp_fetch_web(cookie, transport, cancellable, now_ms, error);
    } else if (!provider && cookie && !token) {
        provider = amp_fetch_web(cookie, transport, cancellable, now_ms, error);
    }
    g_free(cookie);
    g_free(token);
    return provider;
}

CodexBarProvider *codexbar_amp_fetch_with_transport(const CodexBarProviderConfig *config,
                                                    CodexBarWebProviders2Transport transport,
                                                    gint64 now_ms,
                                                    GError **error) {
    return codexbar_amp_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_amp_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                      GCancellable *cancellable,
                                                      GError **error) {
    return codexbar_amp_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_amp_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_amp_fetch_with_cancellable(config, NULL, error);
}

/* T3 Chat */

static const char *const t3chat_cookie_environment_keys[] = {
    "T3CHAT_COOKIE_HEADER", "T3_CHAT_COOKIE_HEADER", "CODEXBAR_T3CHAT_COOKIE_HEADER", NULL};

typedef struct {
    const char *lower_name;
    const char *wire_name;
    const char *default_value;
} T3ChatHeaderDefinition;

static const T3ChatHeaderDefinition t3chat_header_definitions[] = {
    {"accept", "Accept", "*/*"},
    {"accept-language", "Accept-Language", "en-US,en;q=0.9"},
    {"cache-control", "Cache-Control", "no-cache"},
    {"pragma", "Pragma", "no-cache"},
    {"priority", "Priority", "u=4"},
    {"referer", "Referer", "https://t3.chat/settings/customization"},
    {"sec-fetch-dest", "Sec-Fetch-Dest", "empty"},
    {"sec-fetch-mode", "Sec-Fetch-Mode", "cors"},
    {"sec-fetch-site", "Sec-Fetch-Site", "same-origin"},
    {"trpc-accept", "trpc-accept", "application/jsonl"},
    {"user-agent", "User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 "
                                  "(KHTML, like Gecko) Chrome/143.0.0.0 Safari/537.36"},
    {"x-client-context", "x-client-context", NULL},
    {"x-deployment-id", "X-Deployment-Id", NULL},
    {"x-trpc-batch", "x-trpc-batch", "true"},
    {"x-trpc-source", "x-trpc-source", "web-client"},
};

typedef struct {
    char *cookie;
    char *values[G_N_ELEMENTS(t3chat_header_definitions)];
} T3ChatRequestContext;

static void t3chat_request_context_clear(T3ChatRequestContext *context) {
    g_free(context->cookie);
    for (size_t index = 0; index < G_N_ELEMENTS(context->values); index++) g_free(context->values[index]);
    *context = (T3ChatRequestContext){0};
}

static gboolean append_shell_escape(GString *word, char escaped) {
    switch (escaped) {
    case 'n':
        g_string_append_c(word, '\n');
        return TRUE;
    case 'r':
        g_string_append_c(word, '\r');
        return TRUE;
    case 't':
        g_string_append_c(word, '\t');
        return TRUE;
    case '\\':
    case '\'':
    case '"':
        g_string_append_c(word, escaped);
        return TRUE;
    default:
        g_string_append_c(word, escaped);
        return TRUE;
    }
}

static GPtrArray *shell_words(const char *raw) {
    if (!raw || strlen(raw) > WEB2_MAXIMUM_CREDENTIAL_BYTES || !g_utf8_validate(raw, -1, NULL)) return NULL;
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
                if (*cursor != '\'') {
                    g_string_free(word, TRUE);
                    g_ptr_array_free(words, TRUE);
                    return NULL;
                }
                cursor++;
            } else if (*cursor == '"') {
                cursor++;
                while (*cursor && *cursor != '"') {
                    if (*cursor == '\\' && cursor[1]) {
                        cursor++;
                        append_shell_escape(word, *cursor++);
                    } else {
                        g_string_append_c(word, *cursor++);
                    }
                }
                if (*cursor != '"') {
                    g_string_free(word, TRUE);
                    g_ptr_array_free(words, TRUE);
                    return NULL;
                }
                cursor++;
            } else if (*cursor == '\\' && cursor[1]) {
                cursor++;
                g_string_append_c(word, *cursor++);
            } else {
                g_string_append_c(word, *cursor++);
            }
            if (word->len > WEB2_MAXIMUM_CREDENTIAL_BYTES) {
                g_string_free(word, TRUE);
                g_ptr_array_free(words, TRUE);
                return NULL;
            }
        }
        g_ptr_array_add(words, g_string_free(word, FALSE));
        if (words->len > 256) {
            g_ptr_array_free(words, TRUE);
            return NULL;
        }
    }
    return words;
}

static gboolean valid_forwarded_header_value(const char *value) {
    if (!value || value[0] == '\0' || strlen(value) > 8192 || !g_utf8_validate(value, -1, NULL)) return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (*cursor < 32 || *cursor == 127) return FALSE;
    }
    return TRUE;
}

static void t3chat_capture_header(T3ChatRequestContext *context, const char *field) {
    if (!field) return;
    char *copy = g_strdup(field);
    char *colon = strchr(copy, ':');
    if (!colon) {
        g_free(copy);
        return;
    }
    *colon = '\0';
    char *name = g_strstrip(copy);
    char *value = g_strstrip(colon + 1);
    if (!valid_forwarded_header_value(value)) {
        g_free(copy);
        return;
    }
    if (g_ascii_strcasecmp(name, "cookie") == 0) {
        char *cookie = normalize_cookie_header(value, NULL);
        if (cookie) {
            g_free(context->cookie);
            context->cookie = cookie;
        }
        g_free(copy);
        return;
    }
    for (size_t index = 0; index < G_N_ELEMENTS(t3chat_header_definitions); index++) {
        if (g_ascii_strcasecmp(name, t3chat_header_definitions[index].lower_name) == 0) {
            g_free(context->values[index]);
            context->values[index] = g_strdup(value);
            break;
        }
    }
    g_free(copy);
}

static gboolean t3chat_request_context_from_raw(const char *raw, T3ChatRequestContext *context) {
    *context = (T3ChatRequestContext){0};
    if (!raw || strlen(raw) > WEB2_MAXIMUM_CREDENTIAL_BYTES || !g_utf8_validate(raw, -1, NULL)) return FALSE;
    char *clean = g_strdup(raw);
    g_strstrip(clean);
    if (clean[0] == '\0') {
        g_free(clean);
        return FALSE;
    }
    GPtrArray *words = shell_words(clean);
    gboolean saw_header_option = FALSE;
    if (words) {
        for (guint index = 0; index < words->len; index++) {
            const char *word = g_ptr_array_index(words, index);
            const char *field = NULL;
            if (g_str_equal(word, "-H") || g_str_equal(word, "--header")) {
                saw_header_option = TRUE;
                if (index + 1 < words->len) field = g_ptr_array_index(words, ++index);
            } else if (g_str_has_prefix(word, "--header=")) {
                saw_header_option = TRUE;
                field = word + strlen("--header=");
            }
            if (field) t3chat_capture_header(context, field);
        }
        g_ptr_array_free(words, TRUE);
    }
    if (!saw_header_option) context->cookie = normalize_cookie_header(clean, NULL);
    g_free(clean);
    if (context->cookie) return TRUE;
    t3chat_request_context_clear(context);
    return FALSE;
}

static char *t3chat_raw_auth(const CodexBarProviderConfig *config) {
    return first_config_or_environment(config, "cookieHeader", t3chat_cookie_environment_keys);
}

gboolean codexbar_t3chat_has_auth(const CodexBarProviderConfig *config) {
    char *raw = t3chat_raw_auth(config);
    T3ChatRequestContext context = {0};
    gboolean available = t3chat_request_context_from_raw(raw, &context);
    t3chat_request_context_clear(&context);
    g_free(raw);
    return available;
}

static gboolean t3chat_is_customer_data(json_object *object) {
    if (!object || !json_object_is_type(object, json_type_object)) return FALSE;
    if (object_member(object, "usageFourHourPercentage") || object_member(object, "usageMonthPercentage")) {
        return TRUE;
    }
    return object_member(object, "subscription") && object_member(object, "usageBand");
}

static json_object *t3chat_find_customer_data(json_object *value, guint depth, guint *visited) {
    if (!value || depth > 64 || ++*visited > 16384) return NULL;
    if (t3chat_is_customer_data(value)) return value;
    if (json_object_is_type(value, json_type_object)) {
        json_object_object_foreach(value, key, member) {
            (void)key;
            json_object *found = t3chat_find_customer_data(member, depth + 1, visited);
            if (found) return found;
        }
    } else if (json_object_is_type(value, json_type_array)) {
        size_t length = json_object_array_length(value);
        for (size_t index = 0; index < length; index++) {
            json_object *found = t3chat_find_customer_data(
                json_object_array_get_idx(value, index), depth + 1, visited);
            if (found) return found;
        }
    }
    return NULL;
}

static char *t3chat_plan_name(json_object *customer) {
    json_object *subscription = object_member(customer, "subscription");
    char *raw = json_clean_string(subscription, "productName");
    if (!raw) raw = json_clean_string(customer, "subTier");
    if (!raw) return NULL;
    char **parts = g_strsplit(raw, "-", -1);
    GString *title = g_string_new(NULL);
    for (size_t index = 0; parts[index]; index++) {
        char *part = g_strstrip(parts[index]);
        if (part[0] == '\0') continue;
        if (title->len > 0) g_string_append_c(title, ' ');
        gunichar first = g_utf8_get_char(part);
        char first_buffer[7] = {0};
        gint first_length = g_unichar_to_utf8(g_unichar_toupper(first), first_buffer);
        g_string_append_len(title, first_buffer, first_length);
        g_string_append(title, g_utf8_next_char(part));
    }
    g_strfreev(parts);
    g_free(raw);
    if (title->len > 0) return g_string_free(title, FALSE);
    g_string_free(title, TRUE);
    return NULL;
}

static CodexBarProvider *t3chat_provider_from_customer(json_object *customer, gint64 now_ms) {
    double base_percent = 0, overage_percent = 0;
    object_number(customer, "usageFourHourPercentage", FALSE, &base_percent);
    if (!object_number(customer, "usageMonthPercentage", FALSE, &overage_percent)) {
        object_number(customer, "usagePeriodPercentage", FALSE, &overage_percent);
    }
    gint64 base_reset_ms = 0, overage_reset_ms = 0;
    gboolean has_base_reset = parse_timestamp_ms(object_member(customer, "usageFourHourNextResetAt"), &base_reset_ms);
    if (!has_base_reset) {
        has_base_reset = parse_timestamp_ms(object_member(customer, "usageWindowNextResetAt"), &base_reset_ms);
    }
    json_object *subscription = object_member(customer, "subscription");
    gboolean has_overage_reset = parse_timestamp_ms(object_member(subscription, "currentPeriodEnd"),
                                                     &overage_reset_ms);
    char *usage_band = json_clean_string(customer, "usageBand");
    char *base_detail = usage_band ? g_strdup_printf("Base - %s", usage_band) : g_strdup("Base");
    char *plan = t3chat_plan_name(customer);

    CodexBarProvider *provider = new_provider("t3chat", "web", now_ms);
    provider->dashboard_url = g_strdup("https://t3.chat/settings/customization");
    provider->explicit_quota_slots = TRUE;
    codexbar_provider_add_quota_window(
        provider,
        new_window("primary", "Base", base_percent, 240, TRUE,
                   base_reset_ms, has_base_reset, base_detail));
    codexbar_provider_add_quota_window(
        provider,
        new_window("secondary", "Overage", overage_percent, 0, FALSE,
                   overage_reset_ms, has_overage_reset, "Overage"));
    provider->plan = g_strdup(plan);
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = plan;
    g_free(base_detail);
    g_free(usage_band);
    return provider;
}

CodexBarProvider *codexbar_t3chat_parse_json_lines(const char *text,
                                                   size_t length,
                                                   gint64 now_ms,
                                                   GError **error) {
    if (!text || length == 0 || length > WEB2_MAXIMUM_RESPONSE_BYTES || memchr(text, '\0', length) ||
        !g_utf8_validate(text, (gssize)length, NULL)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "T3 Chat response is not valid UTF-8");
        return NULL;
    }
    char *document = g_strndup(text, length);
    char **lines = g_strsplit(document, "\n", -1);
    CodexBarProvider *provider = NULL;
    for (size_t index = 0; lines[index] && !provider; index++) {
        char *line = g_strstrip(lines[index]);
        if (line[0] == '\0') continue;
        json_object *root = parse_json_document(line, strlen(line));
        guint visited = 0;
        json_object *customer = root ? t3chat_find_customer_data(root, 0, &visited) : NULL;
        if (customer) provider = t3chat_provider_from_customer(customer, now_ms);
        if (root) json_object_put(root);
    }
    g_strfreev(lines);
    g_free(document);
    if (!provider) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "T3 Chat response is missing customer data");
    }
    return provider;
}

CodexBarProvider *codexbar_t3chat_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders2Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *raw = t3chat_raw_auth(config);
    T3ChatRequestContext context = {0};
    if (!t3chat_request_context_from_raw(raw, &context)) {
        g_free(raw);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "No valid T3 Chat session cookie is configured");
        return NULL;
    }
    g_free(raw);
    static const char input[] =
        "{\"0\":{\"json\":{\"sessionId\":null},\"meta\":{\"values\":{\"sessionId\":[\"undefined\"]}}}}";
    char *escaped_input = g_uri_escape_string(input, NULL, FALSE);
    char *url = g_strdup_printf(T3CHAT_USAGE_BASE_URL "?batch=1&input=%s", escaped_input);
    g_free(escaped_input);

    CodexBarHttpRequestHeader headers[G_N_ELEMENTS(t3chat_header_definitions) + 2];
    size_t header_count = 0;
    for (size_t index = 0; index < G_N_ELEMENTS(t3chat_header_definitions); index++) {
        const char *value = context.values[index]
                                ? context.values[index]
                                : t3chat_header_definitions[index].default_value;
        if (value) {
            headers[header_count++] = (CodexBarHttpRequestHeader){t3chat_header_definitions[index].wire_name, value};
        }
    }
    headers[header_count++] = (CodexBarHttpRequestHeader){"Origin", "https://t3.chat"};
    headers[header_count++] = (CodexBarHttpRequestHeader){"Cookie", context.cookie};
    CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = header_count,
        .timeout_seconds = WEB2_TIMEOUT_SECONDS,
        .maximum_response_bytes = WEB2_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    g_free(url);
    t3chat_request_context_clear(&context);
    if (!response) return NULL;
    if (response->status != 200) {
        const char *mitigated = codexbar_http_response_header_first(response, "x-vercel-mitigated");
        if (response->status == 429 && mitigated && g_ascii_strcasecmp(mitigated, "challenge") == 0) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                "T3 Chat returned a Vercel challenge; configure the full browser cURL request");
        } else {
            require_success(response, "T3 Chat", error);
        }
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_t3chat_parse_json_lines(
        response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

CodexBarProvider *codexbar_t3chat_fetch_with_transport(const CodexBarProviderConfig *config,
                                                       CodexBarWebProviders2Transport transport,
                                                       gint64 now_ms,
                                                       GError **error) {
    return codexbar_t3chat_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_t3chat_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error) {
    return codexbar_t3chat_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_t3chat_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_t3chat_fetch_with_cancellable(config, NULL, error);
}
