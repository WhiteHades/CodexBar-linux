#include "api_providers5.h"
#include "process.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>
#include <time.h>

#define PROVIDER_RESPONSE_LIMIT (1024U * 1024U)
#define BEDROCK_RESPONSE_LIMIT (4U * 1024U * 1024U)
#define BEDROCK_MAXIMUM_PAGES 20U

static gboolean json_whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

static json_object *parse_json_document(const char *json, size_t length) {
    if (!json || length > G_MAXINT || !g_utf8_validate(json, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
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

static char *clean_secret(const char *raw) {
    if (!raw || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *value = g_strdup(raw);
    strip_unicode_whitespace(value);
    size_t length = strlen(value);
    if (length >= 2 && ((value[0] == '\'' && value[length - 1] == '\'') ||
                        (value[0] == '"' && value[length - 1] == '"'))) {
        value[length - 1] = '\0';
        memmove(value, value + 1, length - 1);
        strip_unicode_whitespace(value);
    }
    for (const char *cursor = value; *cursor; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        if (g_unichar_isspace(character) || g_unichar_iscntrl(character)) {
            g_free(value);
            return NULL;
        }
    }
    if (*value) return value;
    g_free(value);
    return NULL;
}

static char *clean_text(const char *raw) {
    if (!raw || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *value = g_strdup(raw);
    strip_unicode_whitespace(value);
    if (*value) return value;
    g_free(value);
    return NULL;
}

static char *resolve_value(const char *configured, const char *environment_key, gboolean secret) {
    char *value = secret ? clean_secret(configured) : clean_text(configured);
    if (!value) value = secret ? clean_secret(g_getenv(environment_key)) : clean_text(g_getenv(environment_key));
    return value;
}

static gboolean check_cancelled(GCancellable *cancellable, GError **error) {
    return cancellable && g_cancellable_set_error_if_cancelled(cancellable, error);
}

static CodexBarHttpResponse *send_request(const CodexBarHttpRequest *request,
                                          CodexBarApiProviders5Transport transport,
                                          GError **error) {
    if (check_cancelled(request->cancellable, error)) return NULL;
    CodexBarHttpResponse *response = transport(request, error);
    if (request->cancellable && g_cancellable_is_cancelled(request->cancellable)) {
        codexbar_http_response_free(response);
        if (error && *error) g_clear_error(error);
        g_cancellable_set_error_if_cancelled(request->cancellable, error);
        return NULL;
    }
    if (response && request->maximum_response_bytes > 0 &&
        response->body_length > request->maximum_response_bytes) {
        codexbar_http_response_free(response);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "Provider response exceeded the size limit");
        return NULL;
    }
    if (!response && error && !*error) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Provider network request failed");
    }
    return response;
}

static gboolean json_number(json_object *value, double *result) {
    if (!value || (!json_object_is_type(value, json_type_int) &&
                   !json_object_is_type(value, json_type_double))) {
        return FALSE;
    }
    double number = json_object_get_double(value);
    if (!isfinite(number)) return FALSE;
    *result = number;
    return TRUE;
}

static gboolean object_number(json_object *object, const char *key, double *result) {
    json_object *value = NULL;
    return object && json_object_object_get_ex(object, key, &value) && json_number(value, result);
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
    char *copy = g_strndup(text, length);
    strip_unicode_whitespace(copy);
    if (*copy) return copy;
    g_free(copy);
    return NULL;
}

static json_object *object_member(json_object *object, const char *key) {
    json_object *value = NULL;
    return object && json_object_object_get_ex(object, key, &value) &&
                   json_object_is_type(value, json_type_object)
               ? value
               : NULL;
}

static gboolean parse_timestamp_string(const char *raw, gint64 *timestamp_ms) {
    if (!raw) return FALSE;
    GDateTime *time = g_date_time_new_from_iso8601(raw, NULL);
    if (!time) return FALSE;
    *timestamp_ms = g_date_time_to_unix(time) * 1000 + g_date_time_get_microsecond(time) / 1000;
    g_date_time_unref(time);
    return TRUE;
}

static CodexBarProvider *new_provider(const char *id, gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup(id);
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));
    return provider;
}

static char *normalize_base(const char *raw,
                            CodexBarHttpProtocolPolicy policy,
                            gboolean allow_query,
                            GError **error) {
    char *normalized = codexbar_http_normalize_endpoint(raw, policy, error);
    if (!normalized) return NULL;
    GError *uri_error = NULL;
    GUri *uri = g_uri_parse(normalized, G_URI_FLAGS_NONE, &uri_error);
    if (!uri || (!allow_query && (g_uri_get_query(uri) || g_uri_get_fragment(uri)))) {
        if (uri) g_uri_unref(uri);
        g_clear_error(&uri_error);
        g_free(normalized);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Provider endpoint is invalid");
        return NULL;
    }
    g_uri_unref(uri);
    return normalized;
}

static char *append_path(const char *base, const char *path) {
    size_t length = strlen(base);
    while (length > 0 && base[length - 1] == '/') length--;
    return g_strdup_printf("%.*s/%s", (int)length, base, path);
}

/* LiteLLM */

typedef struct {
    char *user_id;
    char *team_id;
    char *key_name;
    double spend;
    gboolean has_expiry;
    gint64 expiry_ms;
} LiteKeyInfo;

static void lite_key_info_clear(LiteKeyInfo *info) {
    g_free(info->user_id);
    g_free(info->team_id);
    g_free(info->key_name);
}

static gboolean lite_parse_key_info(json_object *root, LiteKeyInfo *result, GError **error) {
    json_object *info = object_member(root, "info");
    if (!info) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "LiteLLM response is missing info");
        return FALSE;
    }
    result->user_id = json_string(info, "user_id");
    result->team_id = json_string(info, "team_id");
    result->key_name = json_string(info, "key_name");
    object_number(info, "spend", &result->spend);
    char *expires = json_string(info, "expires");
    result->has_expiry = parse_timestamp_string(expires, &result->expiry_ms);
    g_free(expires);
    if (!result->user_id && !result->team_id) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "LiteLLM key info did not include a user_id or team_id");
        return FALSE;
    }
    return TRUE;
}

static void lite_add_window(CodexBarProvider *provider,
                            const char *id,
                            const char *title,
                            double spend,
                            double budget,
                            const char *label,
                            const char *reset,
                            gint64 now_ms) {
    if (!(budget > 0)) return;
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = CLAMP(spend / budget * 100, 0, 100);
    window->reset_description = g_strdup_printf("%s$%.2f / $%.2f", label ? label : "", spend, budget);
    window->has_resets_at = parse_timestamp_string(reset, &window->resets_at_ms);
    codexbar_provider_add_quota_window(provider, window);
    if (g_str_equal(id, "primary")) {
        provider->provider_cost = g_new0(CodexBarProviderCost, 1);
        provider->provider_cost->used = spend;
        provider->provider_cost->limit = budget;
        provider->provider_cost->currency = g_strdup("USD");
        provider->provider_cost->period = g_strdup("Personal budget");
        provider->provider_cost->has_updated_at = TRUE;
        provider->provider_cost->updated_at_ms = now_ms;
        if (window->has_resets_at) {
            provider->provider_cost->has_resets_at = TRUE;
            provider->provider_cost->resets_at_ms = window->resets_at_ms;
        }
    }
}

static json_object *lite_preferred_team(json_object *teams, const char *team_id) {
    if (!teams || !team_id || !json_object_is_type(teams, json_type_array)) return NULL;
    size_t count = json_object_array_length(teams);
    for (size_t index = 0; index < count; index++) {
        json_object *team = json_object_array_get_idx(teams, index);
        char *candidate = json_string(team, "team_id");
        gboolean matches = candidate && g_str_equal(candidate, team_id);
        g_free(candidate);
        if (matches) return team;
    }
    return NULL;
}

static void lite_set_cost_if_missing(CodexBarProvider *provider,
                                     double spend,
                                     double budget,
                                     gboolean has_budget,
                                     const char *period,
                                     const char *reset,
                                     gint64 now_ms) {
    if (provider->provider_cost || (!(spend > 0) && !(has_budget && budget > 0))) return;
    provider->provider_cost = g_new0(CodexBarProviderCost, 1);
    provider->provider_cost->used = spend;
    provider->provider_cost->limit = has_budget ? MAX(0, budget) : 0;
    provider->provider_cost->currency = g_strdup("USD");
    provider->provider_cost->period = g_strdup(period);
    provider->provider_cost->has_updated_at = TRUE;
    provider->provider_cost->updated_at_ms = now_ms;
    provider->provider_cost->has_resets_at =
        parse_timestamp_string(reset, &provider->provider_cost->resets_at_ms);
}

CodexBarProvider *codexbar_litellm_parse_usage_bytes(const char *key_info_json,
                                                     size_t key_info_length,
                                                     const char *account_info_json,
                                                     size_t account_info_length,
                                                     gint64 now_ms,
                                                     GError **error) {
    json_object *key_root = parse_json_document(key_info_json, key_info_length);
    json_object *account_root = parse_json_document(account_info_json, account_info_length);
    if (!key_root || !account_root || !json_object_is_type(key_root, json_type_object) ||
        !json_object_is_type(account_root, json_type_object)) {
        if (key_root) json_object_put(key_root);
        if (account_root) json_object_put(account_root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "LiteLLM response is malformed");
        return NULL;
    }
    LiteKeyInfo key = {0};
    if (!lite_parse_key_info(key_root, &key, error)) {
        lite_key_info_clear(&key);
        json_object_put(key_root);
        json_object_put(account_root);
        return NULL;
    }
    CodexBarProvider *provider = new_provider("litellm", now_ms);
    provider->explicit_quota_slots = TRUE;
    if (key.has_expiry) {
        provider->has_subscription_expires_at = TRUE;
        provider->subscription_expires_at_ms = key.expiry_ms;
    }
    json_object *extension = json_object_new_object();
    json_object_object_add(extension, "keyName", key.key_name ? json_object_new_string(key.key_name) : NULL);
    json_object_object_add(extension, "keySpendUSD", json_object_new_double(key.spend));

    if (key.user_id) {
        json_object *user = object_member(account_root, "user_info");
        if (!user) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "LiteLLM response is missing user_info");
            goto lite_fail;
        }
        char *response_id = json_string(user, "user_id");
        if (!response_id) response_id = json_string(account_root, "user_id");
        if (response_id && !g_str_equal(response_id, key.user_id)) {
            g_free(response_id);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "LiteLLM user_id did not match key info");
            goto lite_fail;
        }
        g_free(response_id);
        char *email = json_string(user, "user_email");
        if (!email) email = json_string(user, "user_alias");
        if (!email) email = json_string(object_member(user, "metadata"), "preferred_username");
        provider->account = g_strdup(email);
        provider->identity->account_id = email;
        provider->identity->login_method = g_strdup("api");
        double spend = 0, budget = 0;
        gboolean has_budget = object_number(user, "max_budget", &budget);
        object_number(user, "spend", &spend);
        char *reset = json_string(user, "budget_reset_at");
        if (has_budget && budget > 0) {
            lite_add_window(provider, "primary", "Personal budget", spend, budget, "", reset, now_ms);
        }
        lite_set_cost_if_missing(provider, spend, budget, has_budget, has_budget ? "Personal budget" : "Personal spend", reset, now_ms);
        g_free(reset);

        json_object *teams = NULL;
        json_object_object_get_ex(account_root, "teams", &teams);
        json_object *team = lite_preferred_team(teams, key.team_id);
        if (team) {
            char *alias = json_string(team, "team_alias");
            provider->identity->organization = g_strdup(alias);
            double team_spend = 0, team_budget = 0;
            gboolean has_team_budget = object_number(team, "max_budget", &team_budget);
            object_number(team, "spend", &team_spend);
            char *team_reset = json_string(team, "budget_reset_at");
            if (has_team_budget && team_budget > 0) {
                char *label = alias ? g_strdup_printf("Team %s: ", alias) : g_strdup("Team: ");
                lite_add_window(provider,
                                "secondary",
                                "Team budget",
                                team_spend,
                                team_budget,
                                label,
                                team_reset,
                                now_ms);
                g_free(label);
            }
            g_free(alias);
            g_free(team_reset);
        }
    } else {
        json_object *team = object_member(account_root, "team_info");
        if (!team) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "LiteLLM response is missing team_info");
            goto lite_fail;
        }
        char *response_id = json_string(team, "team_id");
        if (!response_id) response_id = json_string(account_root, "team_id");
        if (response_id && !g_str_equal(response_id, key.team_id)) {
            g_free(response_id);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "LiteLLM team_id did not match key info");
            goto lite_fail;
        }
        g_free(response_id);
        char *alias = json_string(team, "team_alias");
        provider->identity->organization = g_strdup(alias);
        provider->identity->login_method = g_strdup("api");
        double spend = 0, budget = 0;
        gboolean has_budget = object_number(team, "max_budget", &budget);
        object_number(team, "spend", &spend);
        char *reset = json_string(team, "budget_reset_at");
        if (has_budget && budget > 0) {
            lite_add_window(provider, "secondary", "Team budget", spend, budget, "Team: ", reset, now_ms);
        }
        lite_set_cost_if_missing(provider, spend, budget, has_budget, has_budget ? "Team budget" : "Team spend", reset, now_ms);
        g_free(alias);
        g_free(reset);
    }
    json_object_object_add(provider->usage_extensions, "liteLLMUsage", extension);
    lite_key_info_clear(&key);
    json_object_put(key_root);
    json_object_put(account_root);
    return provider;

lite_fail:
    json_object_put(extension);
    lite_key_info_clear(&key);
    json_object_put(key_root);
    json_object_put(account_root);
    codexbar_provider_free(provider);
    return NULL;
}

CodexBarProvider *codexbar_litellm_parse_usage(const char *key_info_json,
                                               const char *account_info_json,
                                               gint64 now_ms,
                                               GError **error) {
    return codexbar_litellm_parse_usage_bytes(key_info_json,
                                              key_info_json ? strlen(key_info_json) : 0,
                                              account_info_json,
                                              account_info_json ? strlen(account_info_json) : 0,
                                              now_ms,
                                              error);
}

gboolean codexbar_litellm_has_credentials(const CodexBarProviderConfig *config) {
    char *key = resolve_value(config ? config->api_key : NULL, "LITELLM_API_KEY", TRUE);
    char *base = resolve_value(config ? config->enterprise_host : NULL, "LITELLM_BASE_URL", FALSE);
    gboolean available = key && base;
    g_free(key);
    g_free(base);
    return available;
}

static char *litellm_management_base(const char *base) {
    size_t length = strlen(base);
    while (length > 0 && base[length - 1] == '/') length--;
    if (length >= 3 && g_ascii_strncasecmp(base + length - 3, "/v1", 3) == 0) length -= 3;
    return g_strndup(base, length);
}

CodexBarProvider *codexbar_litellm_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders5Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *key = resolve_value(config ? config->api_key : NULL, "LITELLM_API_KEY", TRUE);
    char *raw_base = resolve_value(config ? config->enterprise_host : NULL, "LITELLM_BASE_URL", FALSE);
    if (!key || !raw_base) {
        g_free(key);
        g_free(raw_base);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "LiteLLM API key and base URL are required");
        return NULL;
    }
    char *base = normalize_base(raw_base, CODEXBAR_HTTP_ALLOW_PRIVATE_HTTP, FALSE, error);
    g_free(raw_base);
    if (!base) {
        g_free(key);
        return NULL;
    }
    char *management = litellm_management_base(base);
    char *key_url = append_path(management, "key/info");
    char *authorization = g_strdup_printf("Bearer %s", key);
    CodexBarHttpRequestHeader headers[] = {{"Authorization", authorization}, {"Accept", "application/json"}};
    CodexBarHttpRequest request = {
        .url = key_url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 15,
        .maximum_response_bytes = PROVIDER_RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_ALLOW_PRIVATE_HTTP,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *key_response = send_request(&request, transport, error);
    if (!key_response) goto lite_fetch_fail;
    if (key_response->status < 200 || key_response->status >= 300) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "LiteLLM API returned HTTP %ld", key_response->status);
        codexbar_http_response_free(key_response);
        goto lite_fetch_fail;
    }
    json_object *key_root = parse_json_document(key_response->body, key_response->body_length);
    LiteKeyInfo info = {0};
    if (!key_root || !lite_parse_key_info(key_root, &info, error)) {
        if (!error || !*error) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "LiteLLM key response is malformed");
        }
        if (key_root) json_object_put(key_root);
        codexbar_http_response_free(key_response);
        lite_key_info_clear(&info);
        goto lite_fetch_fail;
    }
    json_object_put(key_root);
    char *escaped = g_uri_escape_string(info.user_id ? info.user_id : info.team_id, NULL, FALSE);
    char *path = append_path(management, info.user_id ? "user/info" : "team/info");
    char *detail_url = g_strdup_printf("%s?%s=%s", path, info.user_id ? "user_id" : "team_id", escaped);
    g_free(escaped);
    g_free(path);
    lite_key_info_clear(&info);
    request.url = detail_url;
    CodexBarHttpResponse *detail_response = send_request(&request, transport, error);
    if (!detail_response) {
        codexbar_http_response_free(key_response);
        g_free(detail_url);
        goto lite_fetch_fail;
    }
    if (detail_response->status < 200 || detail_response->status >= 300) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "LiteLLM API returned HTTP %ld", detail_response->status);
        codexbar_http_response_free(key_response);
        codexbar_http_response_free(detail_response);
        g_free(detail_url);
        goto lite_fetch_fail;
    }
    CodexBarProvider *provider = codexbar_litellm_parse_usage_bytes(key_response->body,
                                                                    key_response->body_length,
                                                                    detail_response->body,
                                                                    detail_response->body_length,
                                                                    now_ms,
                                                                    error);
    codexbar_http_response_free(key_response);
    codexbar_http_response_free(detail_response);
    g_free(detail_url);
    g_free(key_url);
    g_free(management);
    g_free(base);
    g_free(authorization);
    g_free(key);
    return provider;

lite_fetch_fail:
    g_free(key_url);
    g_free(management);
    g_free(base);
    g_free(authorization);
    g_free(key);
    return NULL;
}

CodexBarProvider *codexbar_litellm_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarApiProviders5Transport transport,
                                                        gint64 now_ms,
                                                        GError **error) {
    return codexbar_litellm_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_litellm_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error) {
    return codexbar_litellm_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_litellm_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_litellm_fetch_with_cancellable(config, NULL, error);
}

/* sub2api */

static gboolean object_boolean_default(json_object *object, const char *key, gboolean fallback) {
    json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value)) return fallback;
    return json_object_is_type(value, json_type_boolean) ? json_object_get_boolean(value) : fallback;
}

static gboolean sub_add_subscription_window(CodexBarProvider *provider,
                                            json_object *subscription,
                                            const char *usage_key,
                                            const char *limit_key,
                                            const char *id,
                                            const char *title,
                                            gint64 minutes) {
    double used = 0, limit = 0;
    object_number(subscription, usage_key, &used);
    if (!object_number(subscription, limit_key, &limit) || !(limit > 0)) return FALSE;
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = CLAMP(used / limit * 100, 0, 100);
    window->has_window_minutes = TRUE;
    window->window_minutes = minutes;
    window->reset_description = g_strdup_printf("$%.2f / $%.2f", used, limit);
    codexbar_provider_add_quota_window(provider, window);
    return TRUE;
}

static json_object *sub_usage_totals(json_object *usage, const char *key) {
    json_object *totals = object_member(usage, key);
    if (!totals) return NULL;
    double requests = 0, tokens = 0, cost = 0;
    object_number(totals, "requests", &requests);
    object_number(totals, "total_tokens", &tokens);
    object_number(totals, "actual_cost", &cost);
    json_object *copy = json_object_new_object();
    json_object_object_add(copy, "requests", json_object_new_int64((gint64)requests));
    json_object_object_add(copy, "totalTokens", json_object_new_int64((gint64)tokens));
    json_object_object_add(copy, "actualCostUSD", json_object_new_double(cost));
    return copy;
}

CodexBarProvider *codexbar_sub2api_parse_usage_bytes(const char *json,
                                                     size_t length,
                                                     gint64 now_ms,
                                                     GError **error) {
    json_object *root = parse_json_document(json, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "sub2api response is malformed");
        return NULL;
    }
    if (!object_boolean_default(root, "isValid", TRUE)) {
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "sub2api rejected the API key");
        return NULL;
    }
    CodexBarProvider *provider = new_provider("sub2api", now_ms);
    provider->explicit_quota_slots = TRUE;
    char *plan = json_string(root, "planName");
    provider->plan = g_strdup(plan);
    provider->identity->organization = g_strdup(plan);
    provider->identity->login_method = plan;
    char *unit = json_string(root, "unit");
    json_object *quota = object_member(root, "quota");
    if (!unit && quota) unit = json_string(quota, "unit");
    if (!unit) unit = g_strdup("USD");

    json_object *subscription = object_member(root, "subscription");
    gboolean has_subscription = subscription != NULL;
    if (subscription) {
        sub_add_subscription_window(
            provider, subscription, "daily_usage_usd", "daily_limit_usd", "primary", "Daily limit", 1440);
        sub_add_subscription_window(provider,
                                    subscription,
                                    "weekly_usage_usd",
                                    "weekly_limit_usd",
                                    "secondary",
                                    "Weekly limit",
                                    10080);
        sub_add_subscription_window(provider,
                                    subscription,
                                    "monthly_usage_usd",
                                    "monthly_limit_usd",
                                    "tertiary",
                                    "Monthly limit",
                                    43200);
        char *expiry = json_string(subscription, "expires_at");
        provider->has_subscription_expires_at = parse_timestamp_string(expiry, &provider->subscription_expires_at_ms);
        g_free(expiry);
    } else if (quota) {
        double limit = 0, used = 0, remaining = 0;
        if (object_number(quota, "limit", &limit) && object_number(quota, "used", &used) &&
            object_number(quota, "remaining", &remaining)) {
            char *quota_unit = json_string(quota, "unit");
            if (!quota_unit) quota_unit = g_strdup(unit);
            CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "Key quota");
            window->usage_known = TRUE;
            window->used_percent = limit > 0 ? CLAMP(used / limit * 100, 0, 100) : 0;
            window->reset_description =
                g_strdup_printf("%.2f / %.2f %s", used, limit, quota_unit);
            codexbar_provider_add_quota_window(provider, window);
            CodexBarBalance *balance = codexbar_balance_new("quota", "Quota remaining", remaining, quota_unit);
            balance->has_used = TRUE;
            balance->used = used;
            balance->has_limit = TRUE;
            balance->limit = limit;
            codexbar_provider_add_balance(provider, balance);
            g_free(quota_unit);
        }
    }

    json_object *rate_limits = NULL;
    if (json_object_object_get_ex(root, "rate_limits", &rate_limits) &&
        json_object_is_type(rate_limits, json_type_array)) {
        size_t count = MIN(json_object_array_length(rate_limits), 64);
        for (size_t index = 0; index < count; index++) {
            json_object *entry = json_object_array_get_idx(rate_limits, index);
            char *window_name = json_string(entry, "window");
            double limit = 0, used = 0;
            if (!window_name || !object_number(entry, "limit", &limit) ||
                !object_number(entry, "used", &used)) {
                g_free(window_name);
                continue;
            }
            const char *known_title = g_ascii_strcasecmp(window_name, "5h") == 0
                                          ? "5 hour limit"
                                      : g_ascii_strcasecmp(window_name, "1d") == 0
                                          ? "Daily limit"
                                      : g_ascii_strcasecmp(window_name, "7d") == 0 ? "7 day limit" : NULL;
            char *title = known_title ? g_strdup(known_title) : g_strdup_printf("%s limit", window_name);
            CodexBarQuotaWindow *window = codexbar_quota_window_new(window_name, title);
            window->usage_known = TRUE;
            window->used_percent = limit > 0 ? CLAMP(used / limit * 100, 0, 100) : 0;
            if (g_ascii_strcasecmp(window_name, "5h") == 0) {
                window->has_window_minutes = TRUE;
                window->window_minutes = 300;
            } else if (g_ascii_strcasecmp(window_name, "1d") == 0) {
                window->has_window_minutes = TRUE;
                window->window_minutes = 1440;
            } else if (g_ascii_strcasecmp(window_name, "7d") == 0) {
                window->has_window_minutes = TRUE;
                window->window_minutes = 10080;
            }
            char *reset = json_string(entry, "reset_at");
            window->has_resets_at = parse_timestamp_string(reset, &window->resets_at_ms);
            window->reset_description = g_strdup_printf("%.2f / %.2f", used, limit);
            codexbar_provider_add_quota_window(provider, window);
            g_free(reset);
            g_free(title);
            g_free(window_name);
        }
    }

    double wallet = 0;
    gboolean has_wallet = object_number(root, "balance", &wallet);
    if (has_wallet) {
        CodexBarBalance *balance = codexbar_balance_new("wallet", "Wallet", wallet, unit);
        codexbar_provider_add_balance(provider, balance);
    }
    if (!provider->has_subscription_expires_at) {
        char *expiry = json_string(root, "expires_at");
        provider->has_subscription_expires_at = parse_timestamp_string(expiry, &provider->subscription_expires_at_ms);
        g_free(expiry);
    }

    json_object *details = json_object_new_object();
    const char *kind = has_subscription ? "subscription" : quota ? "keyQuota" : has_wallet ? "wallet" : "unknown";
    json_object_object_add(details, "kind", json_object_new_string(kind));
    json_object_object_add(details, "unit", json_object_new_string(unit));
    if (object_number(root, "balance", &wallet)) {
        json_object_object_add(details, "balance", json_object_new_double(wallet));
    }
    json_object *usage = object_member(root, "usage");
    json_object *today = sub_usage_totals(usage, "today");
    json_object *total = sub_usage_totals(usage, "total");
    if (today) json_object_object_add(details, "today", today);
    if (total) json_object_object_add(details, "total", total);
    json_object_object_add(provider->usage_extensions, "sub2APIUsage", details);
    g_free(unit);
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_sub2api_parse_usage(const char *json, gint64 now_ms, GError **error) {
    return codexbar_sub2api_parse_usage_bytes(json, json ? strlen(json) : 0, now_ms, error);
}

gboolean codexbar_sub2api_has_credentials(const CodexBarProviderConfig *config) {
    char *key = resolve_value(config ? config->api_key : NULL, "SUB2API_API_KEY", TRUE);
    char *base = resolve_value(config ? config->enterprise_host : NULL, "SUB2API_BASE_URL", FALSE);
    gboolean available = key && base;
    g_free(key);
    g_free(base);
    return available;
}

static char *sub2api_usage_url(const char *base) {
    size_t length = strlen(base);
    while (length > 0 && base[length - 1] == '/') length--;
    if (length >= 9 && g_ascii_strncasecmp(base + length - 9, "/v1/usage", 9) == 0) {
        return g_strndup(base, length);
    }
    if (length >= 3 && g_ascii_strncasecmp(base + length - 3, "/v1", 3) == 0) {
        return g_strdup_printf("%.*s/usage", (int)length, base);
    }
    return g_strdup_printf("%.*s/v1/usage", (int)length, base);
}

CodexBarProvider *codexbar_sub2api_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders5Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *key = resolve_value(config ? config->api_key : NULL, "SUB2API_API_KEY", TRUE);
    char *raw_base = resolve_value(config ? config->enterprise_host : NULL, "SUB2API_BASE_URL", FALSE);
    if (!key || !raw_base) {
        g_free(key);
        g_free(raw_base);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "sub2api API key and base URL are required");
        return NULL;
    }
    char *base = normalize_base(raw_base, CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP, FALSE, error);
    g_free(raw_base);
    if (!base) {
        g_free(key);
        return NULL;
    }
    char *usage_base = sub2api_usage_url(base);
    GTimeZone *local_timezone = g_time_zone_new_local();
    const char *timezone = g_time_zone_get_identifier(local_timezone);
    char *escaped_timezone = g_uri_escape_string(timezone && *timezone ? timezone : "UTC", NULL, FALSE);
    char *url = g_strdup_printf("%s?days=30&timezone=%s", usage_base, escaped_timezone);
    char *authorization = g_strdup_printf("Bearer %s", key);
    CodexBarHttpRequestHeader headers[] = {{"Authorization", authorization}, {"Accept", "application/json"}};
    CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 15,
        .maximum_response_bytes = PROVIDER_RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    CodexBarProvider *provider = NULL;
    if (!response) goto sub_fetch_done;
    if (response->status == 401 || response->status == 403) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "sub2api rejected the API key");
    } else if (response->status < 200 || response->status >= 300) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "sub2api API returned HTTP %ld", response->status);
    } else {
        provider = codexbar_sub2api_parse_usage_bytes(response->body, response->body_length, now_ms, error);
    }
    codexbar_http_response_free(response);

sub_fetch_done:
    g_free(authorization);
    g_free(url);
    g_free(escaped_timezone);
    g_time_zone_unref(local_timezone);
    g_free(usage_base);
    g_free(base);
    g_free(key);
    return provider;
}

CodexBarProvider *codexbar_sub2api_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarApiProviders5Transport transport,
                                                        gint64 now_ms,
                                                        GError **error) {
    return codexbar_sub2api_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_sub2api_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error) {
    return codexbar_sub2api_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_sub2api_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_sub2api_fetch_with_cancellable(config, NULL, error);
}

/* AWS Bedrock */

typedef struct {
    guint32 state[8];
    guint64 bit_count;
    guint8 buffer[64];
    size_t buffer_length;
} Sha256Context;

static guint32 rotate_right(guint32 value, guint shift) {
    return (value >> shift) | (value << (32 - shift));
}

static void sha256_transform(Sha256Context *context, const guint8 block[64]) {
    static const guint32 constants[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2,
    };
    guint32 words[64];
    for (guint index = 0; index < 16; index++) {
        words[index] = ((guint32)block[index * 4] << 24) | ((guint32)block[index * 4 + 1] << 16) |
                       ((guint32)block[index * 4 + 2] << 8) | block[index * 4 + 3];
    }
    for (guint index = 16; index < 64; index++) {
        guint32 left = rotate_right(words[index - 15], 7) ^ rotate_right(words[index - 15], 18) ^
                       (words[index - 15] >> 3);
        guint32 right = rotate_right(words[index - 2], 17) ^ rotate_right(words[index - 2], 19) ^
                        (words[index - 2] >> 10);
        words[index] = words[index - 16] + left + words[index - 7] + right;
    }
    guint32 a = context->state[0], b = context->state[1], c = context->state[2], d = context->state[3];
    guint32 e = context->state[4], f = context->state[5], g = context->state[6], h = context->state[7];
    for (guint index = 0; index < 64; index++) {
        guint32 sum1 = rotate_right(e, 6) ^ rotate_right(e, 11) ^ rotate_right(e, 25);
        guint32 choice = (e & f) ^ ((~e) & g);
        guint32 temp1 = h + sum1 + choice + constants[index] + words[index];
        guint32 sum0 = rotate_right(a, 2) ^ rotate_right(a, 13) ^ rotate_right(a, 22);
        guint32 majority = (a & b) ^ (a & c) ^ (b & c);
        guint32 temp2 = sum0 + majority;
        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }
    context->state[0] += a;
    context->state[1] += b;
    context->state[2] += c;
    context->state[3] += d;
    context->state[4] += e;
    context->state[5] += f;
    context->state[6] += g;
    context->state[7] += h;
}

static void sha256_init(Sha256Context *context) {
    *context = (Sha256Context){
        .state = {0x6a09e667,
                  0xbb67ae85,
                  0x3c6ef372,
                  0xa54ff53a,
                  0x510e527f,
                  0x9b05688c,
                  0x1f83d9ab,
                  0x5be0cd19},
    };
}

static void sha256_update(Sha256Context *context, const guint8 *data, size_t length) {
    context->bit_count += (guint64)length * 8;
    while (length > 0) {
        size_t available = 64 - context->buffer_length;
        size_t amount = MIN(available, length);
        memcpy(context->buffer + context->buffer_length, data, amount);
        context->buffer_length += amount;
        data += amount;
        length -= amount;
        if (context->buffer_length == 64) {
            sha256_transform(context, context->buffer);
            context->buffer_length = 0;
        }
    }
}

static void sha256_final(Sha256Context *context, guint8 digest[32]) {
    context->buffer[context->buffer_length++] = 0x80;
    if (context->buffer_length > 56) {
        memset(context->buffer + context->buffer_length, 0, 64 - context->buffer_length);
        sha256_transform(context, context->buffer);
        context->buffer_length = 0;
    }
    memset(context->buffer + context->buffer_length, 0, 56 - context->buffer_length);
    for (guint index = 0; index < 8; index++) {
        context->buffer[63 - index] = (guint8)(context->bit_count >> (index * 8));
    }
    sha256_transform(context, context->buffer);
    for (guint index = 0; index < 8; index++) {
        digest[index * 4] = (guint8)(context->state[index] >> 24);
        digest[index * 4 + 1] = (guint8)(context->state[index] >> 16);
        digest[index * 4 + 2] = (guint8)(context->state[index] >> 8);
        digest[index * 4 + 3] = (guint8)context->state[index];
    }
}

static void sha256_bytes(const void *data, size_t length, guint8 digest[32]) {
    Sha256Context context;
    sha256_init(&context);
    sha256_update(&context, data, length);
    sha256_final(&context, digest);
}

static char *hex_bytes(const guint8 *data, size_t length) {
    static const char alphabet[] = "0123456789abcdef";
    char *result = g_malloc(length * 2 + 1);
    for (size_t index = 0; index < length; index++) {
        result[index * 2] = alphabet[data[index] >> 4];
        result[index * 2 + 1] = alphabet[data[index] & 0xf];
    }
    result[length * 2] = '\0';
    return result;
}

static void hmac_sha256(const guint8 *key,
                        size_t key_length,
                        const guint8 *data,
                        size_t data_length,
                        guint8 digest[32]) {
    guint8 normalized[64] = {0};
    if (key_length > sizeof(normalized)) {
        sha256_bytes(key, key_length, normalized);
    } else {
        memcpy(normalized, key, key_length);
    }
    guint8 inner_pad[64], outer_pad[64];
    for (size_t index = 0; index < 64; index++) {
        inner_pad[index] = normalized[index] ^ 0x36;
        outer_pad[index] = normalized[index] ^ 0x5c;
    }
    Sha256Context context;
    guint8 inner[32];
    sha256_init(&context);
    sha256_update(&context, inner_pad, sizeof(inner_pad));
    sha256_update(&context, data, data_length);
    sha256_final(&context, inner);
    sha256_init(&context);
    sha256_update(&context, outer_pad, sizeof(outer_pad));
    sha256_update(&context, inner, sizeof(inner));
    sha256_final(&context, digest);
    memset(normalized, 0, sizeof(normalized));
    memset(inner, 0, sizeof(inner));
}

static char *aws_uri_encode(const char *text, gboolean preserve_slash) {
    static const char hex[] = "0123456789ABCDEF";
    GString *encoded = g_string_new(NULL);
    for (const guint8 *cursor = (const guint8 *)text; *cursor; cursor++) {
        guint8 byte = *cursor;
        if (g_ascii_isalnum(byte) || byte == '-' || byte == '.' || byte == '_' || byte == '~' ||
            (preserve_slash && byte == '/')) {
            g_string_append_c(encoded, (char)byte);
        } else {
            g_string_append_c(encoded, '%');
            g_string_append_c(encoded, hex[byte >> 4]);
            g_string_append_c(encoded, hex[byte & 0xf]);
        }
    }
    return g_string_free(encoded, FALSE);
}

typedef struct {
    char *authorization;
    char *host;
    char *content_hash;
} AwsSignature;

static void aws_signature_clear(AwsSignature *signature) {
    g_free(signature->authorization);
    g_free(signature->host);
    g_free(signature->content_hash);
}

static gboolean aws_sign(const char *method,
                         const char *url,
                         const void *body,
                         size_t body_length,
                         const char *content_type,
                         const char *target,
                         const char *access_key_id,
                         const char *secret_access_key,
                         const char *session_token,
                         const char *region,
                         const char *service,
                         const char *amz_date,
                         AwsSignature *signature,
                         GError **error) {
    GError *uri_error = NULL;
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_NONE, &uri_error);
    if (!uri || !g_uri_get_host(uri) || strlen(amz_date) < 8) {
        if (uri) g_uri_unref(uri);
        g_clear_error(&uri_error);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "AWS signing input is invalid");
        return FALSE;
    }
    signature->host = g_strdup(g_uri_get_host(uri));
    const char *raw_path = g_uri_get_path(uri);
    char *path = aws_uri_encode(raw_path && *raw_path ? raw_path : "/", TRUE);
    const char *query = g_uri_get_query(uri);
    char *canonical_query = query ? g_strdup(query) : g_strdup("");
    guint8 body_digest[32];
    sha256_bytes(body ? body : "", body ? body_length : 0, body_digest);
    signature->content_hash = hex_bytes(body_digest, sizeof(body_digest));
    const char *signed_headers = session_token
                                     ? "content-type;host;x-amz-content-sha256;x-amz-date;x-amz-security-token;x-amz-target"
                                     : "content-type;host;x-amz-content-sha256;x-amz-date;x-amz-target";
    char *canonical_headers = session_token
                                  ? g_strdup_printf("content-type:%s\nhost:%s\nx-amz-content-sha256:%s\n"
                                                    "x-amz-date:%s\nx-amz-security-token:%s\nx-amz-target:%s\n",
                                                    content_type,
                                                    signature->host,
                                                    signature->content_hash,
                                                    amz_date,
                                                    session_token,
                                                    target)
                                  : g_strdup_printf("content-type:%s\nhost:%s\nx-amz-content-sha256:%s\n"
                                                    "x-amz-date:%s\nx-amz-target:%s\n",
                                                    content_type,
                                                    signature->host,
                                                    signature->content_hash,
                                                    amz_date,
                                                    target);
    char *canonical_request = g_strdup_printf("%s\n%s\n%s\n%s\n%s\n%s",
                                              method,
                                              path,
                                              canonical_query,
                                              canonical_headers,
                                              signed_headers,
                                              signature->content_hash);
    guint8 canonical_digest[32];
    sha256_bytes(canonical_request, strlen(canonical_request), canonical_digest);
    char *canonical_hash = hex_bytes(canonical_digest, sizeof(canonical_digest));
    char date_stamp[9];
    memcpy(date_stamp, amz_date, 8);
    date_stamp[8] = '\0';
    char *scope = g_strdup_printf("%s/%s/%s/aws4_request", date_stamp, region, service);
    char *string_to_sign =
        g_strdup_printf("AWS4-HMAC-SHA256\n%s\n%s\n%s", amz_date, scope, canonical_hash);
    char *initial_key = g_strdup_printf("AWS4%s", secret_access_key);
    guint8 date_key[32], region_key[32], service_key[32], signing_key[32], signature_bytes[32];
    hmac_sha256((const guint8 *)initial_key,
                strlen(initial_key),
                (const guint8 *)date_stamp,
                strlen(date_stamp),
                date_key);
    hmac_sha256(date_key, sizeof(date_key), (const guint8 *)region, strlen(region), region_key);
    hmac_sha256(region_key, sizeof(region_key), (const guint8 *)service, strlen(service), service_key);
    hmac_sha256(service_key,
                sizeof(service_key),
                (const guint8 *)"aws4_request",
                strlen("aws4_request"),
                signing_key);
    hmac_sha256(signing_key,
                sizeof(signing_key),
                (const guint8 *)string_to_sign,
                strlen(string_to_sign),
                signature_bytes);
    char *signature_hex = hex_bytes(signature_bytes, sizeof(signature_bytes));
    signature->authorization = g_strdup_printf("AWS4-HMAC-SHA256 Credential=%s/%s, SignedHeaders=%s, Signature=%s",
                                               access_key_id,
                                               scope,
                                               signed_headers,
                                               signature_hex);
    memset(date_key, 0, sizeof(date_key));
    memset(region_key, 0, sizeof(region_key));
    memset(service_key, 0, sizeof(service_key));
    memset(signing_key, 0, sizeof(signing_key));
    memset(signature_bytes, 0, sizeof(signature_bytes));
    g_free(signature_hex);
    g_free(initial_key);
    g_free(string_to_sign);
    g_free(scope);
    g_free(canonical_hash);
    g_free(canonical_request);
    g_free(canonical_headers);
    g_free(canonical_query);
    g_free(path);
    g_uri_unref(uri);
    return TRUE;
}

char *codexbar_bedrock_sign_for_testing(const char *method,
                                        const char *url,
                                        const char *body,
                                        const char *access_key_id,
                                        const char *secret_access_key,
                                        const char *session_token,
                                        const char *region,
                                        const char *service,
                                        const char *amz_date,
                                        GError **error) {
    AwsSignature signature = {0};
    if (!method || !url || !access_key_id || !secret_access_key || !region || !service || !amz_date ||
        !aws_sign(method,
                  url,
                  body,
                  body ? strlen(body) : 0,
                  "application/x-amz-json-1.1",
                  "AWSInsightsIndexService.GetCostAndUsage",
                  access_key_id,
                  secret_access_key,
                  session_token,
                  region,
                  service,
                  amz_date,
                  &signature,
                  error)) {
        aws_signature_clear(&signature);
        return NULL;
    }
    char *authorization = g_steal_pointer(&signature.authorization);
    aws_signature_clear(&signature);
    return authorization;
}

typedef struct {
    char *access_key_id;
    char *secret_access_key;
    char *session_token;
    char *region;
    double budget;
    gboolean has_budget;
    gboolean has_explicit_region;
} BedrockSettings;

static void bedrock_settings_clear(BedrockSettings *settings) {
    g_free(settings->access_key_id);
    if (settings->secret_access_key) memset(settings->secret_access_key, 0, strlen(settings->secret_access_key));
    g_free(settings->secret_access_key);
    if (settings->session_token) memset(settings->session_token, 0, strlen(settings->session_token));
    g_free(settings->session_token);
    g_free(settings->region);
}

static gboolean valid_region(const char *region) {
    if (!region) return FALSE;
    GRegex *regex = g_regex_new("^[a-z0-9]+(?:-[a-z0-9]+)+-[0-9]+$", 0, 0, NULL);
    gboolean valid = g_regex_match(regex, region, 0, NULL);
    g_regex_unref(regex);
    return valid;
}

static gboolean bedrock_display_settings(const CodexBarProviderConfig *config,
                                         BedrockSettings *settings,
                                         GError **error) {
    settings->region = clean_secret(config ? config->region : NULL);
    if (!settings->region) settings->region = clean_secret(g_getenv("AWS_REGION"));
    if (!settings->region) settings->region = clean_secret(g_getenv("AWS_DEFAULT_REGION"));
    settings->has_explicit_region = settings->region != NULL;
    if (!settings->region) settings->region = g_strdup("us-east-1");
    char *budget = clean_text(g_getenv("CODEXBAR_BEDROCK_BUDGET"));
    if (budget) {
        char *end = NULL;
        double value = g_ascii_strtod(budget, &end);
        if (*budget && end && !*end && isfinite(value) && value > 0) {
            settings->budget = value;
            settings->has_budget = TRUE;
        }
        g_free(budget);
    }
    if (!valid_region(settings->region)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "AWS region is invalid");
        return FALSE;
    }
    return TRUE;
}

static gboolean bedrock_static_settings(const CodexBarProviderConfig *config,
                                        BedrockSettings *settings,
                                        GError **error) {
    if (!bedrock_display_settings(config, settings, error)) return FALSE;
    settings->access_key_id = resolve_value(config ? config->api_key : NULL, "AWS_ACCESS_KEY_ID", TRUE);
    settings->secret_access_key =
        resolve_value(config ? config->secret_key : NULL, "AWS_SECRET_ACCESS_KEY", TRUE);
    settings->session_token = resolve_value(NULL, "AWS_SESSION_TOKEN", TRUE);
    if (settings->access_key_id && settings->secret_access_key) return TRUE;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "AWS access key and secret key are required");
    return FALSE;
}

static gboolean bedrock_profile_mode(const CodexBarProviderConfig *config) {
    char *mode = clean_text(config ? config->aws_auth_mode : NULL);
    if (!mode) mode = clean_text(g_getenv("CODEXBAR_BEDROCK_AUTH_MODE"));
    gboolean explicit_profile = mode && g_ascii_strcasecmp(mode, "profile") == 0;
    gboolean explicit_keys = mode && g_ascii_strcasecmp(mode, "keys") == 0;
    g_free(mode);
    if (explicit_profile) return TRUE;
    if (explicit_keys) return FALSE;
    char *profile = resolve_value(config ? config->aws_profile : NULL, "AWS_PROFILE", FALSE);
    char *access_key = resolve_value(config ? config->api_key : NULL, "AWS_ACCESS_KEY_ID", TRUE);
    char *secret_key = resolve_value(config ? config->secret_key : NULL, "AWS_SECRET_ACCESS_KEY", TRUE);
    gboolean inferred = profile && (!access_key || !secret_key);
    g_free(profile);
    g_free(access_key);
    g_free(secret_key);
    return inferred;
}

static char *bedrock_aws_binary(void) {
    char *override = clean_text(g_getenv("AWS_CLI_PATH"));
    if (override) {
        if (g_path_is_absolute(override) && g_file_test(override, G_FILE_TEST_IS_EXECUTABLE)) return override;
        g_free(override);
        return NULL;
    }
    char *resolved = g_find_program_in_path("aws");
    if (resolved) return resolved;
    char *user_binary = g_build_filename(g_get_home_dir(), ".local", "bin", "aws", NULL);
    if (g_file_test(user_binary, G_FILE_TEST_IS_EXECUTABLE)) return user_binary;
    g_free(user_binary);
    const char *system_paths[] = {"/usr/local/bin/aws", "/usr/bin/aws"};
    for (size_t index = 0; index < G_N_ELEMENTS(system_paths); index++) {
        if (g_file_test(system_paths[index], G_FILE_TEST_IS_EXECUTABLE)) return g_strdup(system_paths[index]);
    }
    return NULL;
}

static CodexBarProcessResult *bedrock_run_aws(const char *const *arguments,
                                              GCancellable *cancellable,
                                              GError **error) {
    char **environment = g_get_environ();
    environment = g_environ_unsetenv(environment, "AWS_PROFILE");
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .environment = (const char *const *)environment,
        .timeout_milliseconds = 20000,
        .termination_grace_milliseconds = 1000,
        .maximum_output_bytes = 1024U * 1024U,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, cancellable, error);
    g_strfreev(environment);
    return result;
}

static gboolean bedrock_profile_settings(const CodexBarProviderConfig *config,
                                         BedrockSettings *settings,
                                         GCancellable *cancellable,
                                         GError **error) {
    if (!bedrock_display_settings(config, settings, error)) return FALSE;
    char *profile = resolve_value(config ? config->aws_profile : NULL, "AWS_PROFILE", FALSE);
    char *binary = bedrock_aws_binary();
    if (!profile || !binary) {
        gboolean missing_profile = profile == NULL;
        g_free(profile);
        g_free(binary);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            missing_profile ? "AWS profile is required" : "AWS CLI was not found");
        return FALSE;
    }
    const char *export_arguments[] = {
        binary, "configure", "export-credentials", "--profile", profile, "--format", "process", NULL,
    };
    CodexBarProcessResult *result = bedrock_run_aws(export_arguments, cancellable, error);
    if (!result) goto profile_fail;
    if (!codexbar_process_result_succeeded(result)) {
        char *lower = g_utf8_strdown(result->standard_error ? result->standard_error : "", -1);
        gboolean expired = strstr(lower, "sso login") || strstr(lower, "expired") ||
                           strstr(lower, "token has expired");
        g_set_error(error,
                    G_IO_ERROR,
                    expired ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_FAILED,
                    expired ? "AWS profile session expired for %s" : "AWS CLI could not export profile %s",
                    profile);
        g_free(lower);
        codexbar_process_result_free(result);
        goto profile_fail;
    }
    json_object *root = parse_json_document(result->standard_output, result->standard_output_length);
    codexbar_process_result_free(result);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AWS CLI credentials output is malformed");
        goto profile_fail;
    }
    char *raw_access_key = json_string(root, "AccessKeyId");
    char *raw_secret_key = json_string(root, "SecretAccessKey");
    char *raw_session_token = json_string(root, "SessionToken");
    settings->access_key_id = clean_secret(raw_access_key);
    settings->secret_access_key = clean_secret(raw_secret_key);
    settings->session_token = clean_secret(raw_session_token);
    g_free(raw_access_key);
    g_free(raw_secret_key);
    g_free(raw_session_token);
    json_object_put(root);
    if (!settings->access_key_id || !settings->secret_access_key) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AWS CLI credentials output is incomplete");
        goto profile_fail;
    }
    if (!settings->has_explicit_region) {
        const char *region_arguments[] = {binary, "configure", "get", "region", "--profile", profile, NULL};
        result = bedrock_run_aws(region_arguments, cancellable, error);
        if (!result) goto profile_fail;
        if (codexbar_process_result_succeeded(result)) {
            char *resolved_region = clean_secret(result->standard_output);
            if (resolved_region) {
                g_free(settings->region);
                settings->region = resolved_region;
            }
        }
        codexbar_process_result_free(result);
        if (!valid_region(settings->region)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AWS profile region is invalid");
            goto profile_fail;
        }
    }
    g_free(profile);
    g_free(binary);
    return TRUE;

profile_fail:
    g_free(profile);
    g_free(binary);
    return FALSE;
}

static gboolean bedrock_resolve_settings(const CodexBarProviderConfig *config,
                                         BedrockSettings *settings,
                                         GCancellable *cancellable,
                                         GError **error) {
    return bedrock_profile_mode(config) ? bedrock_profile_settings(config, settings, cancellable, error)
                                        : bedrock_static_settings(config, settings, error);
}

gboolean codexbar_bedrock_has_static_credentials(const CodexBarProviderConfig *config) {
    BedrockSettings settings = {0};
    GError *error = NULL;
    gboolean available = bedrock_static_settings(config, &settings, &error);
    g_clear_error(&error);
    bedrock_settings_clear(&settings);
    return available;
}

gboolean codexbar_bedrock_has_credentials(const CodexBarProviderConfig *config) {
    if (bedrock_profile_mode(config)) {
        char *profile = resolve_value(config ? config->aws_profile : NULL, "AWS_PROFILE", FALSE);
        gboolean available = profile != NULL;
        g_free(profile);
        return available;
    }
    return codexbar_bedrock_has_static_credentials(config);
}

static gboolean service_is_bedrock(const char *service) {
    if (!service) return FALSE;
    char *lower = g_utf8_strdown(service, -1);
    gboolean result = strstr(lower, "bedrock") != NULL;
    g_free(lower);
    return result;
}

static gboolean bedrock_parse_cost_root(json_object *root, double *total, char **next_token, GError **error) {
    json_object *results = NULL;
    if (!root || !json_object_object_get_ex(root, "ResultsByTime", &results) ||
        !json_object_is_type(results, json_type_array)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "AWS Cost Explorer response is missing ResultsByTime");
        return FALSE;
    }
    size_t result_count = json_object_array_length(results);
    for (size_t result_index = 0; result_index < result_count; result_index++) {
        json_object *result = json_object_array_get_idx(results, result_index);
        json_object *groups = NULL;
        if (!json_object_object_get_ex(result, "Groups", &groups) ||
            !json_object_is_type(groups, json_type_array)) {
            continue;
        }
        size_t group_count = json_object_array_length(groups);
        for (size_t group_index = 0; group_index < group_count; group_index++) {
            json_object *group = json_object_array_get_idx(groups, group_index);
            json_object *keys = NULL;
            if (!json_object_object_get_ex(group, "Keys", &keys) ||
                !json_object_is_type(keys, json_type_array) || json_object_array_length(keys) == 0) {
                continue;
            }
            json_object *service_value = json_object_array_get_idx(keys, 0);
            if (!json_object_is_type(service_value, json_type_string) ||
                !service_is_bedrock(json_object_get_string(service_value))) {
                continue;
            }
            json_object *amount = object_member(object_member(group, "Metrics"), "UnblendedCost");
            char *amount_text = json_string(amount, "Amount");
            if (!amount_text) continue;
            char *end = NULL;
            double parsed = g_ascii_strtod(amount_text, &end);
            if (*amount_text && end && !*end && isfinite(parsed)) *total += parsed;
            g_free(amount_text);
        }
    }
    *next_token = json_string(root, "NextPageToken");
    return TRUE;
}

typedef struct {
    gint64 input_tokens;
    gint64 output_tokens;
    gint64 requests;
    char *next_token;
} CloudWatchPage;

static gboolean cloudwatch_parse_root(json_object *root, CloudWatchPage *page, GError **error) {
    json_object *messages = NULL;
    if (json_object_object_get_ex(root, "Messages", &messages) && json_object_is_type(messages, json_type_array) &&
        json_object_array_length(messages) > 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "CloudWatch reported incomplete results");
        return FALSE;
    }
    json_object *results = NULL;
    if (!json_object_object_get_ex(root, "MetricDataResults", &results)) {
        page->next_token = json_string(root, "NextToken");
        return TRUE;
    }
    if (!json_object_is_type(results, json_type_array)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "CloudWatch metrics are malformed");
        return FALSE;
    }
    size_t count = json_object_array_length(results);
    for (size_t index = 0; index < count; index++) {
        json_object *result = json_object_array_get_idx(results, index);
        char *id = json_string(result, "Id");
        char *status = json_string(result, "StatusCode");
        if (!id || (!g_str_equal(id, "inputTokens") && !g_str_equal(id, "outputTokens") &&
                    !g_str_equal(id, "requests"))) {
            g_free(id);
            g_free(status);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "CloudWatch metric has an unknown ID");
            return FALSE;
        }
        if (!status || !g_str_equal(status, "Complete")) {
            g_free(id);
            g_free(status);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "CloudWatch metric is incomplete");
            return FALSE;
        }
        json_object *values = NULL;
        double total = 0;
        if (json_object_object_get_ex(result, "Values", &values)) {
            if (!json_object_is_type(values, json_type_array)) {
                g_free(id);
                g_free(status);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "CloudWatch values are malformed");
                return FALSE;
            }
            size_t value_count = json_object_array_length(values);
            for (size_t value_index = 0; value_index < value_count; value_index++) {
                double number;
                if (!json_number(json_object_array_get_idx(values, value_index), &number) || number < 0 ||
                    number > (double)G_MAXINT64 || total > (double)G_MAXINT64 - number) {
                    g_free(id);
                    g_free(status);
                    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "CloudWatch metric is invalid");
                    return FALSE;
                }
                total += number;
            }
        }
        gint64 rounded = (gint64)llround(total);
        if (g_str_equal(id, "inputTokens")) page->input_tokens += rounded;
        if (g_str_equal(id, "outputTokens")) page->output_tokens += rounded;
        if (g_str_equal(id, "requests")) page->requests += rounded;
        g_free(id);
        g_free(status);
    }
    page->next_token = json_string(root, "NextToken");
    return TRUE;
}

static gint64 end_of_month_ms(gint64 now_ms) {
    GDateTime *now = g_date_time_new_from_unix_utc(now_ms / 1000);
    GDateTime *start = g_date_time_new_utc(g_date_time_get_year(now), g_date_time_get_month(now), 1, 0, 0, 0);
    GDateTime *end = g_date_time_add_months(start, 1);
    gint64 result = g_date_time_to_unix(end) * 1000;
    g_date_time_unref(end);
    g_date_time_unref(start);
    g_date_time_unref(now);
    return result;
}

static CodexBarProvider *bedrock_provider(double spend,
                                         const BedrockSettings *settings,
                                         const CloudWatchPage *activity,
                                         gint64 now_ms) {
    CodexBarProvider *provider = new_provider("bedrock", now_ms);
    provider->explicit_quota_slots = TRUE;
    if (settings->has_budget) {
        CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "Monthly budget");
        window->usage_known = TRUE;
        window->used_percent = CLAMP(spend / settings->budget * 100, 0, 100);
        window->has_resets_at = TRUE;
        window->resets_at_ms = end_of_month_ms(now_ms);
        window->reset_description = g_strdup("Monthly budget");
        codexbar_provider_add_quota_window(provider, window);
    }
    provider->provider_cost = g_new0(CodexBarProviderCost, 1);
    provider->provider_cost->used = spend;
    provider->provider_cost->limit = settings->has_budget ? settings->budget : 0;
    provider->provider_cost->currency = g_strdup("USD");
    provider->provider_cost->period = g_strdup("Monthly");
    provider->provider_cost->has_resets_at = TRUE;
    provider->provider_cost->resets_at_ms = end_of_month_ms(now_ms);
    provider->provider_cost->has_updated_at = TRUE;
    provider->provider_cost->updated_at_ms = now_ms;
    GString *identity = g_string_new(NULL);
    g_string_append_printf(identity, "Spend: $%.2f", spend);
    if (settings->has_budget) g_string_append_printf(identity, " - Budget: $%.2f", settings->budget);
    if (activity) {
        gint64 tokens = activity->input_tokens + activity->output_tokens;
        g_string_append_printf(identity,
                               " - Claude 14d: %" G_GINT64_FORMAT " tokens - Requests: %" G_GINT64_FORMAT,
                               tokens,
                               activity->requests);
        json_object *details = json_object_new_object();
        json_object_object_add(details, "inputTokens", json_object_new_int64(activity->input_tokens));
        json_object_object_add(details, "outputTokens", json_object_new_int64(activity->output_tokens));
        json_object_object_add(details, "requestCount", json_object_new_int64(activity->requests));
        json_object_object_add(provider->usage_extensions, "bedrockActivity", details);
    }
    provider->identity->login_method = g_string_free(identity, FALSE);
    provider->identity->organization = g_strdup(settings->region);
    return provider;
}

CodexBarProvider *codexbar_bedrock_parse_usage_bytes(const char *cost_explorer_json,
                                                     size_t cost_explorer_length,
                                                     const char *cloudwatch_json,
                                                     size_t cloudwatch_length,
                                                     const CodexBarProviderConfig *config,
                                                     gint64 now_ms,
                                                     GError **error) {
    BedrockSettings settings = {0};
    if (!bedrock_display_settings(config, &settings, error)) {
        bedrock_settings_clear(&settings);
        return NULL;
    }
    json_object *cost_root = parse_json_document(cost_explorer_json, cost_explorer_length);
    if (!cost_root || !json_object_is_type(cost_root, json_type_object)) {
        if (cost_root) json_object_put(cost_root);
        bedrock_settings_clear(&settings);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AWS Cost Explorer response is malformed");
        return NULL;
    }
    double total = 0;
    char *next_token = NULL;
    if (!bedrock_parse_cost_root(cost_root, &total, &next_token, error) || next_token) {
        if (next_token && (!error || !*error)) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_DATA,
                                "AWS Cost Explorer response requires another page");
        }
        g_free(next_token);
        json_object_put(cost_root);
        bedrock_settings_clear(&settings);
        return NULL;
    }
    json_object_put(cost_root);
    CloudWatchPage activity = {0};
    CloudWatchPage *activity_pointer = NULL;
    if (cloudwatch_json) {
        json_object *cloudwatch_root = parse_json_document(cloudwatch_json, cloudwatch_length);
        if (!cloudwatch_root || !json_object_is_type(cloudwatch_root, json_type_object) ||
            !cloudwatch_parse_root(cloudwatch_root, &activity, error) || activity.next_token) {
            if (cloudwatch_root) json_object_put(cloudwatch_root);
            g_free(activity.next_token);
            bedrock_settings_clear(&settings);
            if (!error || !*error) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "CloudWatch response is malformed");
            }
            return NULL;
        }
        json_object_put(cloudwatch_root);
        activity_pointer = &activity;
    }
    CodexBarProvider *provider = bedrock_provider(total, &settings, activity_pointer, now_ms);
    bedrock_settings_clear(&settings);
    return provider;
}

CodexBarProvider *codexbar_bedrock_parse_usage(const char *cost_explorer_json,
                                               const char *cloudwatch_json,
                                               const CodexBarProviderConfig *config,
                                               gint64 now_ms,
                                               GError **error) {
    return codexbar_bedrock_parse_usage_bytes(cost_explorer_json,
                                              cost_explorer_json ? strlen(cost_explorer_json) : 0,
                                              cloudwatch_json,
                                              cloudwatch_json ? strlen(cloudwatch_json) : 0,
                                              config,
                                              now_ms,
                                              error);
}

static char *amz_date_from_ms(gint64 now_ms) {
    GDateTime *time = g_date_time_new_from_unix_utc(now_ms / 1000);
    char *result = g_date_time_format(time, "%Y%m%dT%H%M%SZ");
    g_date_time_unref(time);
    return result;
}

static CodexBarHttpResponse *bedrock_send_signed(const char *url,
                                                 const char *body,
                                                 const char *content_type,
                                                 const char *target,
                                                 const BedrockSettings *settings,
                                                 const char *region,
                                                 const char *service,
                                                 const char *amz_date,
                                                 CodexBarApiProviders5Transport transport,
                                                 GCancellable *cancellable,
                                                 GError **error) {
    AwsSignature signature = {0};
    if (!aws_sign("POST",
                  url,
                  body,
                  strlen(body),
                  content_type,
                  target,
                  settings->access_key_id,
                  settings->secret_access_key,
                  settings->session_token,
                  region,
                  service,
                  amz_date,
                  &signature,
                  error)) {
        return NULL;
    }
    CodexBarHttpRequestHeader headers[7] = {
        {"Authorization", signature.authorization},
        {"Content-Type", content_type},
        {"Host", signature.host},
        {"X-Amz-Content-SHA256", signature.content_hash},
        {"X-Amz-Date", amz_date},
        {"X-Amz-Target", target},
        {"X-Amz-Security-Token", settings->session_token},
    };
    CodexBarHttpRequest request = {
        .url = url,
        .method = "POST",
        .headers = headers,
        .header_count = settings->session_token ? G_N_ELEMENTS(headers) : G_N_ELEMENTS(headers) - 1,
        .body = body,
        .body_length = strlen(body),
        .timeout_seconds = 15,
        .maximum_response_bytes = BEDROCK_RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    aws_signature_clear(&signature);
    return response;
}

static char *cost_explorer_body(gint64 now_ms, const char *next_token) {
    GDateTime *now = g_date_time_new_from_unix_utc(now_ms / 1000);
    GDateTime *start = g_date_time_new_utc(g_date_time_get_year(now), g_date_time_get_month(now), 1, 0, 0, 0);
    GDateTime *tomorrow = g_date_time_add_days(now, 1);
    char *start_text = g_date_time_format(start, "%Y-%m-%d");
    char *end_text = g_date_time_format(tomorrow, "%Y-%m-%d");
    json_object *root = json_object_new_object();
    json_object *period = json_object_new_object();
    json_object_object_add(period, "Start", json_object_new_string(start_text));
    json_object_object_add(period, "End", json_object_new_string(end_text));
    json_object_object_add(root, "TimePeriod", period);
    json_object_object_add(root, "Granularity", json_object_new_string("MONTHLY"));
    json_object *metrics = json_object_new_array();
    json_object_array_add(metrics, json_object_new_string("UnblendedCost"));
    json_object_object_add(root, "Metrics", metrics);
    json_object *group_by = json_object_new_array();
    json_object *group = json_object_new_object();
    json_object_object_add(group, "Type", json_object_new_string("DIMENSION"));
    json_object_object_add(group, "Key", json_object_new_string("SERVICE"));
    json_object_array_add(group_by, group);
    json_object_object_add(root, "GroupBy", group_by);
    if (next_token) json_object_object_add(root, "NextPageToken", json_object_new_string(next_token));
    char *body = g_strdup(json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
    json_object_put(root);
    g_free(start_text);
    g_free(end_text);
    g_date_time_unref(tomorrow);
    g_date_time_unref(start);
    g_date_time_unref(now);
    return body;
}

static gboolean data_unavailable_response(const CodexBarHttpResponse *response) {
    if (!response || response->status != 400) return FALSE;
    json_object *root = parse_json_document(response->body, response->body_length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        return FALSE;
    }
    const char *keys[] = {"__type", "code", "Code", NULL};
    gboolean unavailable = FALSE;
    for (size_t index = 0; keys[index] && !unavailable; index++) {
        char *code = json_string(root, keys[index]);
        unavailable = code && g_str_has_suffix(code, "DataUnavailableException");
        g_free(code);
    }
    if (!unavailable) {
        char *code = json_string(object_member(root, "Error"), "Code");
        unavailable = code && g_str_has_suffix(code, "DataUnavailableException");
        g_free(code);
    }
    json_object_put(root);
    return unavailable;
}

static gboolean bedrock_fetch_cost(const char *endpoint,
                                   const BedrockSettings *settings,
                                   const char *amz_date,
                                   CodexBarApiProviders5Transport transport,
                                   GCancellable *cancellable,
                                   gint64 now_ms,
                                   double *total,
                                   GError **error) {
    GHashTable *seen_tokens = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *next_token = NULL;
    gboolean success = FALSE;
    for (guint page_index = 0; page_index < BEDROCK_MAXIMUM_PAGES; page_index++) {
        char *body = cost_explorer_body(now_ms, next_token);
        CodexBarHttpResponse *response = bedrock_send_signed(endpoint,
                                                            body,
                                                            "application/x-amz-json-1.1",
                                                            "AWSInsightsIndexService.GetCostAndUsage",
                                                            settings,
                                                            "us-east-1",
                                                            "ce",
                                                            amz_date,
                                                            transport,
                                                            cancellable,
                                                            error);
        g_free(body);
        if (!response) goto done;
        if (response->body_length > BEDROCK_RESPONSE_LIMIT) {
            codexbar_http_response_free(response);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "AWS response exceeded 4 MiB");
            goto done;
        }
        if (data_unavailable_response(response)) {
            codexbar_http_response_free(response);
            success = TRUE;
            goto done;
        }
        if (response->status != 200) {
            g_set_error(error,
                        G_IO_ERROR,
                        response->status == 401 || response->status == 403 ? G_IO_ERROR_PERMISSION_DENIED
                                                                          : G_IO_ERROR_FAILED,
                        "AWS Cost Explorer returned HTTP %ld",
                        response->status);
            codexbar_http_response_free(response);
            goto done;
        }
        json_object *root = parse_json_document(response->body, response->body_length);
        codexbar_http_response_free(response);
        if (!root || !json_object_is_type(root, json_type_object)) {
            if (root) json_object_put(root);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AWS Cost Explorer response is malformed");
            goto done;
        }
        char *new_token = NULL;
        gboolean parsed = bedrock_parse_cost_root(root, total, &new_token, error);
        json_object_put(root);
        if (!parsed) {
            g_free(new_token);
            goto done;
        }
        g_free(next_token);
        next_token = new_token;
        if (!next_token) {
            success = TRUE;
            goto done;
        }
        if (g_hash_table_contains(seen_tokens, next_token)) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_DATA,
                                "AWS Cost Explorer returned repeated NextPageToken");
            goto done;
        }
        g_hash_table_add(seen_tokens, g_strdup(next_token));
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "AWS Cost Explorer returned too many pages");

done:
    g_free(next_token);
    g_hash_table_unref(seen_tokens);
    return success;
}

static const char *aws_partition_suffix(const char *region) {
    if (g_str_has_prefix(region, "cn-")) return "amazonaws.com.cn";
    if (g_str_has_prefix(region, "eusc-")) return "amazonaws.eu";
    if (g_str_has_prefix(region, "us-iso-")) return "c2s.ic.gov";
    if (g_str_has_prefix(region, "us-isob-")) return "sc2s.sgov.gov";
    if (g_str_has_prefix(region, "eu-isoe-")) return "cloud.adc-e.uk";
    if (g_str_has_prefix(region, "us-isof-")) return "csp.hci.ic.gov";
    return "amazonaws.com";
}

static char *cloudwatch_body(gint64 now_ms, const char *next_token) {
    static const char *ids[] = {"inputTokens", "outputTokens", "requests"};
    static const char *metrics[] = {"InputTokenCount", "OutputTokenCount", "Invocations"};
    json_object *root = json_object_new_object();
    json_object_object_add(root, "StartTime", json_object_new_double((double)now_ms / 1000 - 14 * 86400));
    json_object_object_add(root, "EndTime", json_object_new_double((double)now_ms / 1000));
    json_object_object_add(root, "ScanBy", json_object_new_string("TimestampAscending"));
    json_object *queries = json_object_new_array();
    for (size_t index = 0; index < G_N_ELEMENTS(ids); index++) {
        json_object *query = json_object_new_object();
        char *expression = g_strdup_printf(
            "SUM(SEARCH('{AWS/Bedrock,ModelId} MetricName=\"%s\" claude', 'Sum', 86400))", metrics[index]);
        json_object_object_add(query, "Id", json_object_new_string(ids[index]));
        json_object_object_add(query, "Expression", json_object_new_string(expression));
        json_object_object_add(query, "ReturnData", json_object_new_boolean(TRUE));
        json_object_array_add(queries, query);
        g_free(expression);
    }
    json_object_object_add(root, "MetricDataQueries", queries);
    if (next_token) json_object_object_add(root, "NextToken", json_object_new_string(next_token));
    char *body = g_strdup(json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
    json_object_put(root);
    return body;
}

static gboolean bedrock_fetch_cloudwatch(const char *endpoint,
                                         const BedrockSettings *settings,
                                         const char *amz_date,
                                         CodexBarApiProviders5Transport transport,
                                         GCancellable *cancellable,
                                         gint64 now_ms,
                                         CloudWatchPage *totals,
                                         GError **error) {
    GHashTable *seen_tokens = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *next_token = NULL;
    gboolean success = FALSE;
    for (guint page_index = 0; page_index < BEDROCK_MAXIMUM_PAGES; page_index++) {
        char *body = cloudwatch_body(now_ms, next_token);
        CodexBarHttpResponse *response = bedrock_send_signed(endpoint,
                                                            body,
                                                            "application/x-amz-json-1.0",
                                                            "GraniteServiceVersion20100801.GetMetricData",
                                                            settings,
                                                            settings->region,
                                                            "monitoring",
                                                            amz_date,
                                                            transport,
                                                            cancellable,
                                                            error);
        g_free(body);
        if (!response) goto done;
        if (response->body_length > BEDROCK_RESPONSE_LIMIT) {
            codexbar_http_response_free(response);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE, "CloudWatch response exceeded 4 MiB");
            goto done;
        }
        if (response->status != 200) {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "AWS CloudWatch returned HTTP %ld",
                        response->status);
            codexbar_http_response_free(response);
            goto done;
        }
        json_object *root = parse_json_document(response->body, response->body_length);
        codexbar_http_response_free(response);
        if (!root || !json_object_is_type(root, json_type_object)) {
            if (root) json_object_put(root);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "CloudWatch response is malformed");
            goto done;
        }
        CloudWatchPage page = {0};
        gboolean parsed = cloudwatch_parse_root(root, &page, error);
        json_object_put(root);
        if (!parsed) {
            g_free(page.next_token);
            goto done;
        }
        if (G_MAXINT64 - totals->input_tokens < page.input_tokens ||
            G_MAXINT64 - totals->output_tokens < page.output_tokens ||
            G_MAXINT64 - totals->requests < page.requests) {
            g_free(page.next_token);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "CloudWatch metric total overflowed");
            goto done;
        }
        totals->input_tokens += page.input_tokens;
        totals->output_tokens += page.output_tokens;
        totals->requests += page.requests;
        g_free(next_token);
        next_token = page.next_token;
        if (!next_token) {
            success = TRUE;
            goto done;
        }
        if (g_hash_table_contains(seen_tokens, next_token)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "CloudWatch returned repeated NextToken");
            goto done;
        }
        g_hash_table_add(seen_tokens, g_strdup(next_token));
    }
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "CloudWatch returned too many pages");

done:
    g_free(next_token);
    g_hash_table_unref(seen_tokens);
    return success;
}

CodexBarProvider *codexbar_bedrock_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders5Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    BedrockSettings settings = {0};
    if (!bedrock_resolve_settings(config, &settings, cancellable, error)) {
        bedrock_settings_clear(&settings);
        return NULL;
    }
    char *cost_override = clean_text(g_getenv("CODEXBAR_BEDROCK_API_URL"));
    char *cloudwatch_override = clean_text(g_getenv("CODEXBAR_BEDROCK_CLOUDWATCH_API_URL"));
    char *cloudwatch_endpoint = NULL;
    char *cost_endpoint = cost_override
                              ? normalize_base(cost_override, CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP, FALSE, error)
                              : g_strdup("https://ce.us-east-1.amazonaws.com");
    if (!cost_endpoint) goto bedrock_fetch_fail;
    gboolean should_fetch_cloudwatch = !cost_override || cloudwatch_override;
    if (should_fetch_cloudwatch) {
        if (cloudwatch_override) {
            GError *cloudwatch_endpoint_error = NULL;
            cloudwatch_endpoint = normalize_base(cloudwatch_override,
                                                  CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP,
                                                  FALSE,
                                                  &cloudwatch_endpoint_error);
            g_clear_error(&cloudwatch_endpoint_error);
        } else {
            cloudwatch_endpoint = g_strdup_printf("https://monitoring.%s.%s",
                                                  settings.region,
                                                  aws_partition_suffix(settings.region));
        }
    }
    char *amz_date = amz_date_from_ms(now_ms);
    double total = 0;
    if (!bedrock_fetch_cost(cost_endpoint,
                            &settings,
                            amz_date,
                            transport,
                            cancellable,
                            now_ms,
                            &total,
                            error)) {
        g_free(amz_date);
        goto bedrock_fetch_fail;
    }
    CloudWatchPage activity = {0};
    CloudWatchPage *activity_pointer = NULL;
    if (cloudwatch_endpoint) {
        GError *cloudwatch_error = NULL;
        if (bedrock_fetch_cloudwatch(cloudwatch_endpoint,
                                     &settings,
                                     amz_date,
                                     transport,
                                     cancellable,
                                     now_ms,
                                     &activity,
                                     &cloudwatch_error)) {
            activity_pointer = &activity;
        } else if (g_error_matches(cloudwatch_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_propagate_error(error, cloudwatch_error);
            g_free(amz_date);
            goto bedrock_fetch_fail;
        } else {
            g_clear_error(&cloudwatch_error);
        }
    }
    CodexBarProvider *provider = bedrock_provider(total, &settings, activity_pointer, now_ms);
    g_free(amz_date);
    g_free(cloudwatch_endpoint);
    g_free(cost_endpoint);
    g_free(cloudwatch_override);
    g_free(cost_override);
    bedrock_settings_clear(&settings);
    return provider;

bedrock_fetch_fail:
    g_free(cloudwatch_endpoint);
    g_free(cost_endpoint);
    g_free(cloudwatch_override);
    g_free(cost_override);
    bedrock_settings_clear(&settings);
    return NULL;
}

CodexBarProvider *codexbar_bedrock_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarApiProviders5Transport transport,
                                                        gint64 now_ms,
                                                        GError **error) {
    return codexbar_bedrock_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_bedrock_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error) {
    return codexbar_bedrock_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_bedrock_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_bedrock_fetch_with_cancellable(config, NULL, error);
}
