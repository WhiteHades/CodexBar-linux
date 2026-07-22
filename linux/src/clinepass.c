#include "clinepass.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define CLINEPASS_USAGE_URL "https://api.cline.bot/api/v1/users/me/plan/usage-limits"
#define CLINEPASS_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)

static char *clean_api_key(const char *raw) {
    if (!raw) return NULL;
    char *clean = g_strdup(raw);
    g_strstrip(clean);
    size_t length = strlen(clean);
    if (length >= 1 && ((clean[0] == '\'' && clean[length - 1] == '\'') ||
                        (clean[0] == '"' && clean[length - 1] == '"'))) {
        clean[length - 1] = '\0';
        memmove(clean, clean + 1, length - 1);
        g_strstrip(clean);
    }
    if (clean[0] != '\0') return clean;
    g_free(clean);
    return NULL;
}

static char *resolve_api_key(const CodexBarProviderConfig *config) {
    char *key = clean_api_key(config ? config->api_key : NULL);
    if (!key) key = clean_api_key(g_getenv("CLINE_API_KEY"));
    if (!key) key = clean_api_key(g_getenv("CLINEPASS_API_KEY"));
    return key;
}

static gboolean digits_at(const char *raw, size_t offset, size_t count) {
    for (size_t index = offset; index < offset + count; index++) {
        if (!g_ascii_isdigit(raw[index])) return FALSE;
    }
    return TRUE;
}

static gboolean internet_timestamp(const char *raw, size_t length) {
    if (length < 20 || !digits_at(raw, 0, 4) || raw[4] != '-' || !digits_at(raw, 5, 2) ||
        raw[7] != '-' || !digits_at(raw, 8, 2) || raw[10] != 'T' || !digits_at(raw, 11, 2) ||
        raw[13] != ':' || !digits_at(raw, 14, 2) || raw[16] != ':' || !digits_at(raw, 17, 2)) {
        return FALSE;
    }
    size_t offset = 19;
    if (raw[offset] == '.') {
        size_t fraction = ++offset;
        while (offset < length && g_ascii_isdigit(raw[offset])) offset++;
        if (offset == fraction) return FALSE;
    }
    if (offset < length && raw[offset] == 'Z') return offset + 1 == length;
    return offset + 6 == length && (raw[offset] == '+' || raw[offset] == '-') &&
           digits_at(raw, offset + 1, 2) && raw[offset + 3] == ':' && digits_at(raw, offset + 4, 2);
}

static gboolean parse_reset_timestamp(json_object *value, gint64 *milliseconds) {
    if (!value || json_object_is_type(value, json_type_null)) return TRUE;
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    const char *raw = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!internet_timestamp(raw, length)) return FALSE;
    GDateTime *time = g_date_time_new_from_iso8601(raw, NULL);
    if (!time) return FALSE;
    *milliseconds = g_date_time_to_unix(time) * 1000 + g_date_time_get_microsecond(time) / 1000;
    g_date_time_unref(time);
    return TRUE;
}

static int limit_slot(json_object *type) {
    const char *raw = json_object_get_string(type);
    size_t length = (size_t)json_object_get_string_len(type);
    if (length == strlen("five_hour") && memcmp(raw, "five_hour", length) == 0) return 0;
    if (length == strlen("weekly") && memcmp(raw, "weekly", length) == 0) return 1;
    if (length == strlen("monthly") && memcmp(raw, "monthly", length) == 0) return 2;
    return -1;
}

static CodexBarQuotaWindow *make_window(int slot, double used_percent, json_object *resets_at, GError **error) {
    static const char *ids[] = {"primary", "secondary", "tertiary"};
    static const char *titles[] = {"5-hour", "Weekly", "Monthly"};
    static const gint64 minutes[] = {300, 10080, 43200};
    gint64 reset_ms = 0;
    if (!parse_reset_timestamp(resets_at, &reset_ms)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "ClinePass usage response is malformed");
        return NULL;
    }
    CodexBarQuotaWindow *window = codexbar_quota_window_new(ids[slot], titles[slot]);
    window->usage_known = TRUE;
    window->used_percent = CLAMP(used_percent, 0.0, 100.0);
    window->has_window_minutes = TRUE;
    window->window_minutes = minutes[slot];
    if (resets_at && !json_object_is_type(resets_at, json_type_null)) {
        window->has_resets_at = TRUE;
        window->resets_at_ms = reset_ms;
    }
    return window;
}

static json_object *parse_json_document(const char *json, size_t length) {
    if (!json || length > G_MAXINT || !g_utf8_validate(json, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, json, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && g_ascii_isspace(json[consumed])) consumed++;
    gboolean valid = parse_error == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

CodexBarProvider *codexbar_clinepass_parse_usage_bytes(
    const char *json, size_t length, gint64 now_ms, GError **error) {
    json_object *root = parse_json_document(json, length);
    json_object *success = NULL;
    json_object *data = NULL;
    json_object *limits = NULL;
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "success", &success) || !json_object_is_type(success, json_type_boolean) ||
        !json_object_get_boolean(success) || !json_object_object_get_ex(root, "data", &data) ||
        !json_object_is_type(data, json_type_object) || !json_object_object_get_ex(data, "limits", &limits) ||
        !json_object_is_type(limits, json_type_array)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "ClinePass usage response is malformed");
        return NULL;
    }

    CodexBarQuotaWindow *windows[3] = {0};
    size_t count = json_object_array_length(limits);
    for (size_t index = 0; index < count; index++) {
        json_object *limit = json_object_array_get_idx(limits, index);
        json_object *type = NULL;
        json_object *percent = NULL;
        json_object *resets_at = NULL;
        gboolean has_reset = limit && json_object_object_get_ex(limit, "resetsAt", &resets_at);
        if (!limit || !json_object_is_type(limit, json_type_object) ||
            !json_object_object_get_ex(limit, "type", &type) || !json_object_is_type(type, json_type_string) ||
            !json_object_object_get_ex(limit, "percentUsed", &percent) ||
            (!json_object_is_type(percent, json_type_int) && !json_object_is_type(percent, json_type_double)) ||
            !isfinite(json_object_get_double(percent)) ||
            (has_reset && !json_object_is_type(resets_at, json_type_null) &&
             !json_object_is_type(resets_at, json_type_string))) {
            goto malformed;
        }
        int slot = limit_slot(type);
        if (slot < 0) continue;
        CodexBarQuotaWindow *window = make_window(slot, json_object_get_double(percent), resets_at, error);
        if (!window) goto failed;
        codexbar_quota_window_free(windows[slot]);
        windows[slot] = window;
    }

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("clinepass");
    provider->source = g_strdup("api");
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = g_strdup("API key");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    for (guint slot = 0; slot < G_N_ELEMENTS(windows); slot++) {
        if (windows[slot]) codexbar_provider_add_quota_window(provider, windows[slot]);
    }
    json_object_put(root);
    return provider;

malformed:
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "ClinePass usage response is malformed");
failed:
    for (guint slot = 0; slot < G_N_ELEMENTS(windows); slot++) codexbar_quota_window_free(windows[slot]);
    json_object_put(root);
    return NULL;
}

CodexBarProvider *codexbar_clinepass_parse_usage(const char *json, gint64 now_ms, GError **error) {
    return codexbar_clinepass_parse_usage_bytes(json, json ? strlen(json) : 0, now_ms, error);
}

CodexBarProvider *codexbar_clinepass_fetch_with_transport(const CodexBarProviderConfig *config,
                                                          CodexBarClinePassTransport transport,
                                                          gint64 now_ms,
                                                          GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    char *api_key = resolve_api_key(config);
    if (!api_key) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "Missing ClinePass API key. Set CLINE_API_KEY or CLINEPASS_API_KEY.");
        return NULL;
    }

    char *authorization = g_strdup_printf("Bearer %s", api_key);
    g_free(api_key);
    const CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
    };
    const CodexBarHttpRequest request = {
        .url = CLINEPASS_USAGE_URL,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 15,
        .maximum_response_bytes = CLINEPASS_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
    };
    CodexBarHttpResponse *response = transport(&request, error);
    g_free(authorization);
    if (!response) {
        if (!error || !*error) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ClinePass network request failed");
        }
        return NULL;
    }
    if (response->status == 401 || response->status == 403) {
        codexbar_http_response_free(response);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "ClinePass API key was rejected.");
        return NULL;
    }
    if (response->status != 200) {
        long status = response->status;
        codexbar_http_response_free(response);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ClinePass API error: HTTP %ld", status);
        return NULL;
    }
    CodexBarProvider *provider =
        codexbar_clinepass_parse_usage_bytes(response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

CodexBarProvider *codexbar_clinepass_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_clinepass_fetch_with_transport(config, codexbar_http_send, g_get_real_time() / 1000, error);
}
