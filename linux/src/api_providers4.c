#include "api_providers4.h"

#include "local_providers.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>

#define PROVIDER_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)
#define PROVIDER_MAXIMUM_CREDENTIAL_BYTES 16384U
#define PROVIDER_TIMEOUT_SECONDS 15
#define OLLAMA_TIMEOUT_SECONDS 20

#define FACTORY_API_BASE "https://api.factory.ai"
#define FACTORY_APP_BASE "https://app.factory.ai"
#define GEMINI_CODE_ASSIST_URL "https://cloudcode-pa.googleapis.com/v1internal:loadCodeAssist"
#define GEMINI_PROJECTS_URL "https://cloudresourcemanager.googleapis.com/v1/projects"
#define GEMINI_QUOTA_URL "https://cloudcode-pa.googleapis.com/v1internal:retrieveUserQuota"
#define GEMINI_REFRESH_URL "https://oauth2.googleapis.com/token"
#define ANTIGRAVITY_CODE_ASSIST_URL "https://cloudcode-pa.googleapis.com/v1internal:loadCodeAssist"
#define ANTIGRAVITY_ONBOARD_URL "https://cloudcode-pa.googleapis.com/v1internal:onboardUser"
#define ANTIGRAVITY_MODELS_URL "https://cloudcode-pa.googleapis.com/v1internal:fetchAvailableModels"
#define ANTIGRAVITY_QUOTA_URL "https://cloudcode-pa.googleapis.com/v1internal:retrieveUserQuota"
#define ANTIGRAVITY_REFRESH_URL "https://oauth2.googleapis.com/token"
#define OLLAMA_TAGS_URL "https://ollama.com/api/tags"
#define OLLAMA_VALIDATION_URL "https://ollama.com/api/web_search"
#define OLLAMA_SETTINGS_URL "https://ollama.com/settings"

static gboolean json_whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

static json_object *parse_json_document(const char *json, size_t length) {
    if (!json || length == 0 || length > G_MAXINT || length > PROVIDER_MAXIMUM_RESPONSE_BYTES ||
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

static char *clean_credential(const char *raw) {
    if (!raw || !g_utf8_validate(raw, -1, NULL) || strlen(raw) > PROVIDER_MAXIMUM_CREDENTIAL_BYTES) return NULL;
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
        if (*cursor < 33 || *cursor == 127) {
            g_free(value);
            return NULL;
        }
    }
    if (value[0] != '\0') return value;
    g_free(value);
    return NULL;
}

static char *json_string(json_object *object, const char *key) {
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

static gboolean json_number(json_object *value, double *result) {
    if (!value || json_object_is_type(value, json_type_null) || json_object_is_type(value, json_type_boolean)) {
        return FALSE;
    }
    double number = 0;
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        number = json_object_get_double(value);
    } else if (json_object_is_type(value, json_type_string)) {
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

static gboolean object_number(json_object *object, const char *key, double *result) {
    return json_number(object_member(object, key), result);
}

static gboolean object_boolean(json_object *object, const char *key, gboolean fallback) {
    json_object *value = object_member(object, key);
    return value && json_object_is_type(value, json_type_boolean) ? json_object_get_boolean(value) : fallback;
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
                                          CodexBarApiProviders4Transport transport,
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

static CodexBarProvider *provider_new(const char *id, gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup(id);
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    return provider;
}

static CodexBarQuotaWindow *add_window(CodexBarProvider *provider,
                                       const char *id,
                                       const char *title,
                                       double used_percent,
                                       gint64 window_minutes,
                                       gint64 resets_at_ms,
                                       const char *detail) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = CLAMP(used_percent, 0.0, 100.0);
    if (window_minutes > 0) {
        window->has_window_minutes = TRUE;
        window->window_minutes = window_minutes;
    }
    if (resets_at_ms > 0) {
        window->has_resets_at = TRUE;
        window->resets_at_ms = resets_at_ms;
    }
    window->detail = g_strdup(detail);
    codexbar_provider_add_quota_window(provider, window);
    return window;
}

static char *factory_dotenv_api_key(void) {
    const char *home = g_getenv("HOME");
    if (!home || home[0] == '\0') return NULL;
    char *path = g_build_filename(home, ".factory", ".env", NULL);
    GStatBuf status;
    if (g_stat(path, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
        (guint64)status.st_size > PROVIDER_MAXIMUM_RESPONSE_BYTES) {
        g_free(path);
        return NULL;
    }
    char *contents = NULL;
    gsize length = 0;
    gboolean loaded = g_file_get_contents(path, &contents, &length, NULL);
    g_free(path);
    if (!loaded || length == 0 || length > PROVIDER_MAXIMUM_RESPONSE_BYTES) {
        g_free(contents);
        return NULL;
    }
    char *result = NULL;
    char **lines = g_strsplit(contents, "\n", -1);
    for (size_t index = 0; lines[index] && !result; index++) {
        char *line = g_strstrip(lines[index]);
        if (line[0] == '\0' || line[0] == '#') continue;
        if (g_str_has_prefix(line, "export ")) line = g_strstrip(line + strlen("export "));
        char *separator = strchr(line, '=');
        if (!separator) continue;
        *separator = '\0';
        if (!g_str_equal(g_strstrip(line), "FACTORY_API_KEY")) continue;
        result = clean_credential(separator + 1);
    }
    g_strfreev(lines);
    g_free(contents);
    return result;
}

static char *factory_api_key(const CodexBarProviderConfig *config) {
    char *key = clean_credential(config ? config->api_key : NULL);
    if (!key) key = clean_credential(g_getenv("FACTORY_API_KEY"));
    if (!key) key = factory_dotenv_api_key();
    return key;
}

gboolean codexbar_factory_has_api_key(const CodexBarProviderConfig *config) {
    char *key = factory_api_key(config);
    gboolean present = key != NULL;
    g_free(key);
    return present;
}

static void factory_add_identity(CodexBarProvider *provider, json_object *auth) {
    json_object *organization = object_member(auth, "organization");
    json_object *subscription = object_member(organization, "subscription");
    json_object *orb = object_member(subscription, "orbSubscription");
    json_object *plan_object = object_member(orb, "plan");
    char *plan = json_string(plan_object, "name");
    char *tier = json_string(subscription, "factoryTier");
    char *organization_name = json_string(organization, "name");
    GString *method = g_string_new(NULL);
    if (tier) {
        char *title = g_ascii_strdown(tier, -1);
        title[0] = g_ascii_toupper(title[0]);
        g_string_append_printf(method, "Factory %s", title);
        g_free(title);
    }
    char *lower_plan = plan ? g_ascii_strdown(plan, -1) : NULL;
    if (plan && !strstr(lower_plan, "factory")) {
        if (method->len > 0) g_string_append(method, " - ");
        g_string_append(method, plan);
    }
    provider->plan = plan ? g_strdup(plan) : NULL;
    if (method->len > 0 || organization_name) {
        provider->identity = g_new0(CodexBarProviderIdentity, 1);
        provider->identity->organization = g_strdup(organization_name);
        provider->identity->login_method = method->len > 0 ? g_strdup(method->str) : NULL;
    }
    g_string_free(method, TRUE);
    g_free(lower_plan);
    g_free(organization_name);
    g_free(tier);
    g_free(plan);
}

static double factory_legacy_percent(json_object *pool) {
    double used = 0;
    double allowance = 0;
    double ratio = 0;
    gboolean has_used = object_number(pool, "userTokens", &used);
    gboolean has_allowance = object_number(pool, "totalAllowance", &allowance);
    gboolean has_ratio = object_number(pool, "usedRatio", &ratio);
    const double unlimited_threshold = 1000000000000.0;
    if (has_ratio && !(ratio == 0 && has_used && used > 0 && has_allowance && allowance > 0 &&
                       allowance <= unlimited_threshold)) {
        if (ratio >= -0.001 && ratio <= 1.001) return CLAMP(ratio * 100.0, 0.0, 100.0);
        if ((!has_allowance || allowance <= 0 || allowance > unlimited_threshold) && ratio >= -0.1 &&
            ratio <= 100.1) {
            return CLAMP(ratio, 0.0, 100.0);
        }
    }
    if (has_allowance && allowance > unlimited_threshold) return CLAMP(used / 100000000.0 * 100.0, 0.0, 100.0);
    return has_used && has_allowance && allowance > 0 ? CLAMP(used / allowance * 100.0, 0.0, 100.0) : 0;
}

static gboolean factory_parse_roots(const char *auth_json,
                                    size_t auth_length,
                                    const char *data_json,
                                    size_t data_length,
                                    const char *kind,
                                    json_object **auth,
                                    json_object **data,
                                    GError **error) {
    *auth = parse_json_document(auth_json, auth_length);
    *data = parse_json_document(data_json, data_length);
    if (*auth && json_object_is_type(*auth, json_type_object) && *data &&
        json_object_is_type(*data, json_type_object)) {
        return TRUE;
    }
    if (*auth) json_object_put(*auth);
    if (*data) json_object_put(*data);
    *auth = NULL;
    *data = NULL;
    g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Factory %s response is malformed", kind);
    return FALSE;
}

CodexBarProvider *codexbar_factory_parse_legacy_usage(const char *auth_json,
                                                      size_t auth_length,
                                                      const char *usage_json,
                                                      size_t usage_length,
                                                      gint64 now_ms,
                                                      GError **error) {
    json_object *auth = NULL;
    json_object *root = NULL;
    if (!factory_parse_roots(auth_json, auth_length, usage_json, usage_length, "usage", &auth, &root, error)) {
        return NULL;
    }
    json_object *usage = object_member(root, "usage");
    if (usage && !json_object_is_type(usage, json_type_object)) usage = NULL;
    json_object *standard = object_member(usage, "standard");
    json_object *premium = object_member(usage, "premium");
    gint64 resets_at_ms = 0;
    parse_timestamp_ms(object_member(usage, "endDate"), &resets_at_ms);

    CodexBarProvider *provider = provider_new("factory", now_ms);
    provider->explicit_quota_slots = TRUE;
    factory_add_identity(provider, auth);
    add_window(provider, "primary", "Standard", factory_legacy_percent(standard), 0, resets_at_ms, NULL);
    add_window(provider, "secondary", "Premium", factory_legacy_percent(premium), 0, resets_at_ms, NULL);
    json_object_put(root);
    json_object_put(auth);
    return provider;
}

static gboolean factory_add_billing_window(CodexBarProvider *provider,
                                           json_object *pool,
                                           const char *key,
                                           const char *id,
                                           const char *title,
                                           gint64 minutes,
                                           gint64 now_ms) {
    json_object *window = object_member(pool, key);
    if (!window || !json_object_is_type(window, json_type_object)) return FALSE;
    double used_percent = 0;
    if (!object_number(window, "usedPercent", &used_percent)) return FALSE;
    gint64 reset_ms = 0;
    double seconds_remaining = 0;
    gboolean has_seconds = object_number(window, "secondsRemaining", &seconds_remaining);
    if (has_seconds && seconds_remaining > 0 &&
        seconds_remaining <= (double)(G_MAXINT64 - now_ms) / 1000.0) {
        reset_ms = now_ms + (gint64)llround(seconds_remaining * 1000.0);
    } else {
        gint64 end_ms = 0;
        if (parse_timestamp_ms(object_member(window, "windowEnd"), &end_ms) && end_ms > now_ms) reset_ms = end_ms;
        if (object_member(window, "windowEnd") && !has_seconds && reset_ms == 0) used_percent = 0;
    }
    add_window(provider, id, title, used_percent, minutes, reset_ms, NULL);
    return TRUE;
}

static gboolean factory_pool_has_usage_data(json_object *pool) {
    const char *keys[] = {"fiveHour", "weekly", "monthly"};
    for (size_t index = 0; index < G_N_ELEMENTS(keys); index++) {
        json_object *window = object_member(pool, keys[index]);
        double used_percent = 0;
        if (object_number(window, "usedPercent", &used_percent) && used_percent > 0) return TRUE;
        if (object_member(window, "windowEnd") || object_member(window, "secondsRemaining")) return TRUE;
    }
    return FALSE;
}

CodexBarProvider *codexbar_factory_parse_billing_limits(const char *auth_json,
                                                        size_t auth_length,
                                                        const char *limits_json,
                                                        size_t limits_length,
                                                        gint64 now_ms,
                                                        GError **error) {
    json_object *auth = NULL;
    json_object *root = NULL;
    if (!factory_parse_roots(auth_json, auth_length, limits_json, limits_length, "billing limits", &auth, &root,
                             error)) {
        return NULL;
    }
    json_object *limits = object_member(root, "limits");
    json_object *standard = object_member(limits, "standard");
    if (!object_boolean(root, "usesTokenRateLimitsBilling", FALSE) || !standard ||
        !json_object_is_type(standard, json_type_object)) {
        json_object_put(root);
        json_object_put(auth);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Factory billing limits are unavailable");
        return NULL;
    }
    CodexBarProvider *provider = provider_new("factory", now_ms);
    provider->explicit_quota_slots = TRUE;
    factory_add_identity(provider, auth);
    char *overage_preference = json_string(root, "overagePreference");
    if (overage_preference) {
        if (!provider->identity) provider->identity = g_new0(CodexBarProviderIdentity, 1);
        char *method = provider->identity->login_method
                           ? g_strdup_printf("%s - Fallback: %s", provider->identity->login_method,
                                             overage_preference)
                           : g_strdup_printf("Fallback: %s", overage_preference);
        g_free(provider->identity->login_method);
        provider->identity->login_method = method;
    }
    gboolean valid = factory_add_billing_window(provider, standard, "fiveHour", "primary", "5h", 300, now_ms) &&
                     factory_add_billing_window(provider, standard, "weekly", "secondary", "7-day", 10080,
                                                now_ms) &&
                     factory_add_billing_window(provider, standard, "monthly", "tertiary", "Monthly", 0,
                                                now_ms);
    json_object *core = object_member(limits, "core");
    if (core && json_object_is_type(core, json_type_object) && factory_pool_has_usage_data(core)) {
        gboolean core_valid = factory_add_billing_window(
                                  provider, core, "fiveHour", "factory-core-5h", "Core 5h", 300, now_ms) &&
                              factory_add_billing_window(
                                  provider, core, "weekly", "factory-core-7d", "Core 7-day", 10080, now_ms) &&
                              factory_add_billing_window(
                                  provider, core, "monthly", "factory-core-monthly", "Core Monthly", 0, now_ms);
        valid = valid && core_valid;
    }
    double balance_cents = 0;
    object_number(root, "extraUsageBalanceCents", &balance_cents);
    provider->provider_cost = g_new0(CodexBarProviderCost, 1);
    provider->provider_cost->used = balance_cents / 100.0;
    provider->provider_cost->currency = g_strdup("USD");
    provider->provider_cost->period = g_strdup("Extra usage balance");
    provider->provider_cost->has_updated_at = TRUE;
    provider->provider_cost->updated_at_ms = now_ms;
    g_free(overage_preference);
    json_object_put(root);
    json_object_put(auth);
    if (valid) return provider;
    codexbar_provider_free(provider);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Factory billing limits are malformed");
    return NULL;
}

static CodexBarHttpResponse *factory_request(const char *url,
                                             const char *authorization,
                                             CodexBarApiProviders4Transport transport,
                                             GCancellable *cancellable,
                                             GError **error) {
    const CodexBarHttpRequestHeader headers[] = {
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"Origin", FACTORY_APP_BASE},
        {"Referer", FACTORY_APP_BASE "/"},
        {"x-factory-client", "web-app"},
        {"Authorization", authorization},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = PROVIDER_TIMEOUT_SECONDS,
        .maximum_response_bytes = PROVIDER_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    return send_request(&request, transport, error);
}

static gboolean require_success(CodexBarHttpResponse *response, const char *provider, GError **error) {
    if (response->status == 401 || response->status == 403) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "%s authentication failed", provider);
        return FALSE;
    }
    if (response->status < 200 || response->status >= 300) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "%s request failed with HTTP %ld", provider,
                    response->status);
        return FALSE;
    }
    return TRUE;
}

static char *factory_user_id(json_object *auth) {
    return json_string(object_member(auth, "userProfile"), "id");
}

static CodexBarProvider *factory_fetch_base(const char *base,
                                            const char *authorization,
                                            CodexBarApiProviders4Transport transport,
                                            GCancellable *cancellable,
                                            gint64 now_ms,
                                            GError **error) {
    char *auth_url = g_strdup_printf("%s/api/app/auth/me", base);
    CodexBarHttpResponse *auth_response = factory_request(auth_url, authorization, transport, cancellable, error);
    g_free(auth_url);
    if (!auth_response) return NULL;
    if (!require_success(auth_response, "Factory", error)) {
        codexbar_http_response_free(auth_response);
        return NULL;
    }
    json_object *auth_root = parse_json_document(auth_response->body, auth_response->body_length);
    if (!auth_root || !json_object_is_type(auth_root, json_type_object)) {
        if (auth_root) json_object_put(auth_root);
        codexbar_http_response_free(auth_response);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Factory auth response is malformed");
        return NULL;
    }
    char *user_id = factory_user_id(auth_root);
    json_object_put(auth_root);

    GError *billing_error = NULL;
    CodexBarHttpResponse *billing = factory_request(FACTORY_API_BASE "/api/billing/limits", authorization,
                                                    transport, cancellable, &billing_error);
    if (billing && billing->status == 200) {
        GError *parse_error = NULL;
        CodexBarProvider *provider = codexbar_factory_parse_billing_limits(
            auth_response->body, auth_response->body_length, billing->body, billing->body_length, now_ms,
            &parse_error);
        codexbar_http_response_free(billing);
        if (provider) {
            g_free(user_id);
            codexbar_http_response_free(auth_response);
            return provider;
        }
        g_clear_error(&parse_error);
    } else {
        codexbar_http_response_free(billing);
    }
    if (billing_error && g_error_matches(billing_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_free(user_id);
        codexbar_http_response_free(auth_response);
        g_propagate_error(error, billing_error);
        return NULL;
    }
    g_clear_error(&billing_error);

    char *escaped_user = user_id ? g_uri_escape_string(user_id, NULL, TRUE) : NULL;
    char *usage_url = escaped_user
                          ? g_strdup_printf("%s/api/organization/subscription/usage?useCache=true&userId=%s", base,
                                            escaped_user)
                          : g_strdup_printf("%s/api/organization/subscription/usage?useCache=true", base);
    g_free(escaped_user);
    g_free(user_id);
    CodexBarHttpResponse *usage = factory_request(usage_url, authorization, transport, cancellable, error);
    g_free(usage_url);
    if (!usage) {
        codexbar_http_response_free(auth_response);
        return NULL;
    }
    if (!require_success(usage, "Factory", error)) {
        codexbar_http_response_free(usage);
        codexbar_http_response_free(auth_response);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_factory_parse_legacy_usage(
        auth_response->body, auth_response->body_length, usage->body, usage->body_length, now_ms, error);
    codexbar_http_response_free(usage);
    codexbar_http_response_free(auth_response);
    return provider;
}

CodexBarProvider *codexbar_factory_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    if (!transport) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Factory transport is missing");
        return NULL;
    }
    char *key = factory_api_key(config);
    if (!key) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "Factory API key missing. Set FACTORY_API_KEY or configure api_key.");
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", key);
    g_free(key);
    const char *bases[] = {FACTORY_API_BASE, FACTORY_APP_BASE};
    GError *last_error = NULL;
    GError *auth_error = NULL;
    CodexBarProvider *provider = NULL;
    for (size_t index = 0; index < G_N_ELEMENTS(bases) && !provider; index++) {
        GError *candidate_error = NULL;
        provider = factory_fetch_base(bases[index], authorization, transport, cancellable, now_ms,
                                      &candidate_error);
        if (candidate_error && g_error_matches(candidate_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_clear_error(&last_error);
            g_clear_error(&auth_error);
            g_free(authorization);
            g_propagate_error(error, candidate_error);
            return NULL;
        }
        if (candidate_error && g_error_matches(candidate_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED) &&
            !auth_error) {
            auth_error = g_error_copy(candidate_error);
        }
        g_clear_error(&last_error);
        last_error = candidate_error;
    }
    g_free(authorization);
    if (provider) {
        g_clear_error(&last_error);
        g_clear_error(&auth_error);
        return provider;
    }
    if (auth_error) {
        g_clear_error(&last_error);
        g_propagate_error(error, auth_error);
    } else if (last_error) {
        g_propagate_error(error, last_error);
    } else {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Factory request failed");
    }
    return NULL;
}

CodexBarProvider *codexbar_factory_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarApiProviders4Transport transport,
                                                        gint64 now_ms,
                                                        GError **error) {
    return codexbar_factory_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_factory_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error) {
    return codexbar_factory_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_factory_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_factory_fetch_with_cancellable(config, NULL, error);
}

CodexBarProvider *codexbar_factory_fetch_for_source_with_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    GCancellable *cancellable,
    GError **error) {
    const char *mode = source && source[0] ? source : "auto";
    if (g_str_equal(mode, "web")) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Factory web sessions are unavailable on Linux; configure FACTORY_API_KEY instead");
        return NULL;
    }
    if (!g_str_equal(mode, "auto") && !g_str_equal(mode, "api") && !g_str_equal(mode, "cli")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Factory source '%s' is unsupported", mode);
        return NULL;
    }
    return codexbar_factory_fetch_with_cancellable(config, cancellable, error);
}

typedef struct {
    char *access_token;
    char *id_token;
    char *refresh_token;
    gint64 expiry_ms;
} GeminiCredentials;

typedef struct {
    char *project_id;
    char *tier;
    char *paid_tier_name;
    char *plan_type;
} GeminiCodeAssist;

static void gemini_credentials_clear(GeminiCredentials *credentials) {
    g_free(credentials->access_token);
    g_free(credentials->id_token);
    g_free(credentials->refresh_token);
    *credentials = (GeminiCredentials){0};
}

static void gemini_code_assist_clear(GeminiCodeAssist *status) {
    g_free(status->project_id);
    g_free(status->tier);
    g_free(status->paid_tier_name);
    g_free(status->plan_type);
    *status = (GeminiCodeAssist){0};
}

static gboolean regular_file_within_limit(const char *path, gsize maximum, GError **error) {
    GStatBuf status;
    if (g_stat(path, &status) != 0) {
        if (error) {
            int saved_errno = errno;
            g_set_error(error, G_IO_ERROR, g_io_error_from_errno(saved_errno), "Credentials not found at %s", path);
        }
        return FALSE;
    }
    if (!S_ISREG(status.st_mode) || status.st_size <= 0 || (guint64)status.st_size > maximum) {
        if (error) g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Credentials are invalid at %s", path);
        return FALSE;
    }
    return TRUE;
}

static gboolean gemini_auth_type_supported(const char *home, GError **error) {
    char *path = g_build_filename(home, ".gemini", "settings.json", NULL);
    if (!regular_file_within_limit(path, PROVIDER_MAXIMUM_RESPONSE_BYTES, NULL)) {
        g_free(path);
        return TRUE;
    }
    char *contents = NULL;
    gsize length = 0;
    gboolean loaded = g_file_get_contents(path, &contents, &length, NULL);
    g_free(path);
    if (!loaded || length == 0 || length > PROVIDER_MAXIMUM_RESPONSE_BYTES) {
        g_free(contents);
        return TRUE;
    }
    json_object *root = parse_json_document(contents, length);
    g_free(contents);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        return TRUE;
    }
    char *selected = json_string(object_member(object_member(root, "security"), "auth"), "selectedType");
    json_object_put(root);
    gboolean supported = !selected || (!g_str_equal(selected, "api-key") &&
                                       !g_str_equal(selected, "gemini-api-key") &&
                                       !g_str_equal(selected, "vertex-ai"));
    if (!supported && error) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "Gemini %s auth is unsupported; use Google account OAuth instead",
                    g_str_equal(selected, "vertex-ai") ? "Vertex AI" : "API key");
    }
    g_free(selected);
    return supported;
}

static gboolean gemini_load_credentials(GeminiCredentials *credentials, GError **error) {
    const char *home = g_getenv("HOME");
    if (!home || home[0] == '\0') home = g_get_home_dir();
    if (!home || home[0] == '\0') {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Gemini home directory is unavailable");
        return FALSE;
    }
    if (!gemini_auth_type_supported(home, error)) return FALSE;
    char *path = g_build_filename(home, ".gemini", "oauth_creds.json", NULL);
    if (!regular_file_within_limit(path, PROVIDER_MAXIMUM_RESPONSE_BYTES, error)) {
        if (error) g_prefix_error(error, "Gemini OAuth ");
        g_free(path);
        return FALSE;
    }
    char *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &contents, &length, error)) {
        if (error) g_prefix_error(error, "Gemini OAuth credentials are unreadable at %s: ", path);
        g_free(path);
        return FALSE;
    }
    g_free(path);
    json_object *root = parse_json_document(contents, length);
    g_free(contents);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Gemini OAuth credentials are malformed");
        return FALSE;
    }
    credentials->access_token = json_string(root, "access_token");
    credentials->id_token = json_string(root, "id_token");
    credentials->refresh_token = json_string(root, "refresh_token");
    double expiry = 0;
    if (object_number(root, "expiry_date", &expiry) && expiry > 0 && expiry <= (double)G_MAXINT64) {
        credentials->expiry_ms = (gint64)llround(expiry);
    }
    json_object_put(root);
    if (credentials->access_token || credentials->refresh_token) return TRUE;
    gemini_credentials_clear(credentials);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                        "Gemini OAuth credentials are missing; run `gemini` to authenticate");
    return FALSE;
}

gboolean codexbar_gemini_has_oauth_credentials(const CodexBarProviderConfig *config) {
    (void)config;
    GeminiCredentials credentials = {0};
    gboolean loaded = gemini_load_credentials(&credentials, NULL);
    gemini_credentials_clear(&credentials);
    return loaded;
}

static gboolean response_is_consumer_deprecation(const CodexBarHttpResponse *response) {
    if (!response || !response->body || response->body_length == 0 ||
        !g_utf8_validate(response->body, (gssize)response->body_length, NULL)) {
        return FALSE;
    }
    char *lower = g_ascii_strdown(response->body, (gssize)response->body_length);
    gboolean result = strstr(lower, "unsupported_client") || strstr(lower, "ineligibletiererror") ||
                      (strstr(lower, "no longer supported") && strstr(lower, "gemini code assist")) ||
                      (strstr(lower, "migrate") && strstr(lower, "antigravity") && strstr(lower, "gemini"));
    g_free(lower);
    return result;
}

static char *gemini_bearer(const char *token, GError **error) {
    char *clean = clean_credential(token);
    if (!clean) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Gemini OAuth token is invalid");
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", clean);
    g_free(clean);
    return authorization;
}

static CodexBarHttpResponse *gemini_request(const char *url,
                                            const char *method,
                                            const char *authorization,
                                            const char *body,
                                            CodexBarApiProviders4Transport transport,
                                            GCancellable *cancellable,
                                            GError **error) {
    const CodexBarHttpRequestHeader headers[] = {
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"Authorization", authorization},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .body = body,
        .body_length = body ? strlen(body) : 0,
        .timeout_seconds = 10,
        .maximum_response_bytes = PROVIDER_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    return send_request(&request, transport, error);
}

static void gemini_parse_code_assist(const char *json, size_t length, GeminiCodeAssist *status) {
    json_object *root = parse_json_document(json, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        return;
    }
    json_object *project = object_member(root, "cloudaicompanionProject");
    if (project && json_object_is_type(project, json_type_string)) {
        status->project_id = json_string(root, "cloudaicompanionProject");
    } else if (project && json_object_is_type(project, json_type_object)) {
        status->project_id = json_string(project, "id");
        if (!status->project_id) status->project_id = json_string(project, "projectId");
    }
    status->tier = json_string(object_member(root, "currentTier"), "id");
    status->paid_tier_name = json_string(object_member(root, "paidTier"), "name");
    status->plan_type = json_string(object_member(root, "planInfo"), "planType");
    json_object_put(root);
}

static char *gemini_discover_project(const CodexBarHttpResponse *response) {
    if (!response || response->status != 200) return NULL;
    json_object *root = parse_json_document(response->body, response->body_length);
    json_object *projects = object_member(root, "projects");
    if (!root || !projects || !json_object_is_type(projects, json_type_array)) {
        if (root) json_object_put(root);
        return NULL;
    }
    char *result = NULL;
    size_t count = json_object_array_length(projects);
    for (size_t index = 0; index < count && !result; index++) {
        json_object *project = json_object_array_get_idx(projects, index);
        char *project_id = json_string(project, "projectId");
        if (!project_id) continue;
        gboolean selected = g_str_has_prefix(project_id, "gen-lang-client");
        json_object *labels = object_member(project, "labels");
        if (!selected && labels && json_object_is_type(labels, json_type_object) &&
            object_member(labels, "generative-language")) {
            selected = TRUE;
        }
        if (selected) result = project_id;
        else g_free(project_id);
    }
    json_object_put(root);
    return result;
}

static char *gemini_jwt_claim(const char *token, const char *claim) {
    if (!token) return NULL;
    char **parts = g_strsplit(token, ".", 3);
    if (!parts[0] || !parts[1]) {
        g_strfreev(parts);
        return NULL;
    }
    char *payload = g_strdup(parts[1]);
    for (char *cursor = payload; *cursor; cursor++) {
        if (*cursor == '-') *cursor = '+';
        else if (*cursor == '_') *cursor = '/';
    }
    size_t length = strlen(payload);
    size_t padding = (4 - length % 4) % 4;
    char *padded = g_strconcat(payload, padding >= 1 ? "=" : "", padding >= 2 ? "=" : "", NULL);
    g_free(payload);
    gsize decoded_length = 0;
    guchar *decoded = g_base64_decode(padded, &decoded_length);
    g_free(padded);
    g_strfreev(parts);
    json_object *root = parse_json_document((const char *)decoded, decoded_length);
    g_free(decoded);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        return NULL;
    }
    char *value = json_string(root, claim);
    json_object_put(root);
    return value;
}

typedef struct {
    gboolean set;
    double remaining_fraction;
    char *model_id;
    gint64 reset_ms;
} GeminiQuotaTier;

static void gemini_quota_tier_clear(GeminiQuotaTier *tier) {
    g_free(tier->model_id);
    *tier = (GeminiQuotaTier){0};
}

static void gemini_consider_quota(GeminiQuotaTier *tier,
                                  const char *model_id,
                                  double remaining_fraction,
                                  json_object *reset_value) {
    if (tier->set && remaining_fraction >= tier->remaining_fraction) return;
    tier->set = TRUE;
    tier->remaining_fraction = remaining_fraction;
    g_free(tier->model_id);
    tier->model_id = g_strdup(model_id);
    tier->reset_ms = 0;
    parse_timestamp_ms(reset_value, &tier->reset_ms);
}

static void gemini_add_window(CodexBarProvider *provider,
                              const char *id,
                              const char *title,
                              const GeminiQuotaTier *tier,
                              gint64 now_ms) {
    CodexBarQuotaWindow *window = add_window(provider, id, title,
                                             (1.0 - tier->remaining_fraction) * 100.0, 1440,
                                             tier->reset_ms, tier->model_id);
    if (tier->reset_ms <= 0) return;
    gint64 remaining_minutes = MAX((tier->reset_ms - now_ms) / 60000, 0);
    window->reset_description = remaining_minutes >= 60
                                    ? g_strdup_printf("Resets in %" G_GINT64_FORMAT "h %" G_GINT64_FORMAT "m",
                                                      remaining_minutes / 60, remaining_minutes % 60)
                                    : remaining_minutes > 0
                                          ? g_strdup_printf("Resets in %" G_GINT64_FORMAT "m", remaining_minutes)
                                          : g_strdup("Resets soon");
}

static char *gemini_plan(const GeminiCodeAssist *status, const char *hosted_domain) {
    if (status && status->plan_type) return g_strdup(status->plan_type);
    if (status && status->paid_tier_name) return g_strdup(status->paid_tier_name);
    if (!status || !status->tier) return NULL;
    if (g_str_equal(status->tier, "standard-tier")) return g_strdup("Paid");
    if (g_str_equal(status->tier, "free-tier")) return g_strdup(hosted_domain ? "Workspace" : "Free");
    if (g_str_equal(status->tier, "legacy-tier")) return g_strdup("Legacy");
    return NULL;
}

CodexBarProvider *codexbar_gemini_parse_quota(const char *quota_json,
                                             size_t quota_length,
                                             const char *id_token,
                                             const char *code_assist_json,
                                             size_t code_assist_length,
                                             gint64 now_ms,
                                             GError **error) {
    json_object *root = parse_json_document(quota_json, quota_length);
    json_object *buckets = object_member(root, "buckets");
    if (!root || !buckets || !json_object_is_type(buckets, json_type_array) ||
        json_object_array_length(buckets) == 0) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Gemini quota response has no quota buckets");
        return NULL;
    }
    GeminiQuotaTier pro = {0};
    GeminiQuotaTier flash = {0};
    GeminiQuotaTier flash_lite = {0};
    size_t count = json_object_array_length(buckets);
    for (size_t index = 0; index < count; index++) {
        json_object *bucket = json_object_array_get_idx(buckets, index);
        char *model_id = json_string(bucket, "modelId");
        double fraction = 0;
        if (!model_id || !object_number(bucket, "remainingFraction", &fraction)) {
            g_free(model_id);
            continue;
        }
        char *lower = g_ascii_strdown(model_id, -1);
        if (strstr(lower, "flash-lite")) {
            gemini_consider_quota(&flash_lite, model_id, fraction, object_member(bucket, "resetTime"));
        } else if (strstr(lower, "flash")) {
            gemini_consider_quota(&flash, model_id, fraction, object_member(bucket, "resetTime"));
        } else if (strstr(lower, "pro")) {
            gemini_consider_quota(&pro, model_id, fraction, object_member(bucket, "resetTime"));
        }
        g_free(lower);
        g_free(model_id);
    }
    json_object_put(root);

    GeminiCodeAssist status = {0};
    if (code_assist_json && code_assist_length > 0) {
        gemini_parse_code_assist(code_assist_json, code_assist_length, &status);
    }
    char *email = gemini_jwt_claim(id_token, "email");
    char *hosted_domain = gemini_jwt_claim(id_token, "hd");
    char *plan = gemini_plan(&status, hosted_domain);
    CodexBarProvider *provider = provider_new("gemini", now_ms);
    provider->explicit_quota_slots = TRUE;
    provider->account = g_strdup(email);
    provider->plan = g_strdup(plan);
    if (email || plan) {
        provider->identity = g_new0(CodexBarProviderIdentity, 1);
        provider->identity->login_method = g_strdup(plan);
    }
    if (pro.set) gemini_add_window(provider, "primary", "Pro", &pro, now_ms);
    if (flash.set) gemini_add_window(provider, "secondary", "Flash", &flash, now_ms);
    if (flash_lite.set) gemini_add_window(provider, "tertiary", "Flash Lite", &flash_lite, now_ms);
    gemini_quota_tier_clear(&pro);
    gemini_quota_tier_clear(&flash);
    gemini_quota_tier_clear(&flash_lite);
    gemini_code_assist_clear(&status);
    g_free(hosted_domain);
    g_free(email);
    g_free(plan);
    return provider;
}

CodexBarProvider *codexbar_gemini_fetch_access_token_with_transport_and_cancellable(
    const char *access_token,
    const char *id_token,
    CodexBarApiProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    if (!transport) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Gemini transport is missing");
        return NULL;
    }
    char *authorization = gemini_bearer(access_token, error);
    if (!authorization) return NULL;
    const char *metadata = "{\"metadata\":{\"ideType\":\"GEMINI_CLI\",\"pluginType\":\"GEMINI\"}}";
    GError *assist_error = NULL;
    CodexBarHttpResponse *assist = gemini_request(GEMINI_CODE_ASSIST_URL, "POST", authorization, metadata,
                                                  transport, cancellable, &assist_error);
    if (!assist && assist_error && g_error_matches(assist_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_free(authorization);
        g_propagate_error(error, assist_error);
        return NULL;
    }
    g_clear_error(&assist_error);
    if (assist && assist->status != 200 && response_is_consumer_deprecation(assist)) {
        codexbar_http_response_free(assist);
        g_free(authorization);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                            "Gemini Code Assist consumer tier is no longer supported; migrate to Antigravity");
        return NULL;
    }
    GeminiCodeAssist assist_status = {0};
    if (assist && assist->status == 200) {
        gemini_parse_code_assist(assist->body, assist->body_length, &assist_status);
    }
    if (!assist_status.project_id) {
        GError *projects_error = NULL;
        CodexBarHttpResponse *projects = gemini_request(GEMINI_PROJECTS_URL, "GET", authorization, NULL,
                                                        transport, cancellable, &projects_error);
        if (!projects && projects_error && g_error_matches(projects_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            codexbar_http_response_free(assist);
            gemini_code_assist_clear(&assist_status);
            g_free(authorization);
            g_propagate_error(error, projects_error);
            return NULL;
        }
        g_clear_error(&projects_error);
        assist_status.project_id = gemini_discover_project(projects);
        codexbar_http_response_free(projects);
    }
    char *quota_body = NULL;
    if (assist_status.project_id) {
        json_object *body = json_object_new_object();
        json_object_object_add(body, "project", json_object_new_string(assist_status.project_id));
        quota_body = g_strdup(json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN));
        json_object_put(body);
    } else {
        quota_body = g_strdup("{}");
    }
    CodexBarHttpResponse *quota = gemini_request(GEMINI_QUOTA_URL, "POST", authorization, quota_body,
                                                 transport, cancellable, error);
    g_free(quota_body);
    g_free(authorization);
    if (!quota) {
        codexbar_http_response_free(assist);
        gemini_code_assist_clear(&assist_status);
        return NULL;
    }
    if (quota->status == 401 || quota->status == 403) {
        gboolean deprecated = response_is_consumer_deprecation(quota);
        codexbar_http_response_free(quota);
        codexbar_http_response_free(assist);
        gemini_code_assist_clear(&assist_status);
        g_set_error_literal(error, G_IO_ERROR,
                            deprecated ? G_IO_ERROR_NOT_SUPPORTED : G_IO_ERROR_PERMISSION_DENIED,
                            deprecated ? "Gemini Code Assist consumer tier is no longer supported; migrate to Antigravity"
                                       : "Gemini OAuth authentication failed");
        return NULL;
    }
    if (quota->status != 200) {
        long status_code = quota->status;
        gboolean deprecated = response_is_consumer_deprecation(quota);
        codexbar_http_response_free(quota);
        codexbar_http_response_free(assist);
        gemini_code_assist_clear(&assist_status);
        if (deprecated) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                                "Gemini Code Assist consumer tier is no longer supported; migrate to Antigravity");
        } else {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Gemini quota request failed with HTTP %ld",
                        status_code);
        }
        return NULL;
    }
    const char *assist_json = assist && assist->status == 200 ? assist->body : NULL;
    size_t assist_length = assist_json ? assist->body_length : 0;
    CodexBarProvider *provider = codexbar_gemini_parse_quota(
        quota->body, quota->body_length, id_token, assist_json, assist_length, now_ms, error);
    codexbar_http_response_free(quota);
    codexbar_http_response_free(assist);
    gemini_code_assist_clear(&assist_status);
    return provider;
}

static gboolean gemini_oauth_client(char **client_id, char **client_secret) {
    *client_id = clean_credential(g_getenv("GEMINI_OAUTH_CLIENT_ID"));
    *client_secret = clean_credential(g_getenv("GEMINI_OAUTH_CLIENT_SECRET"));
    if (*client_id && *client_secret) return TRUE;
    g_clear_pointer(client_id, g_free);
    g_clear_pointer(client_secret, g_free);
    const char *path = g_getenv("GEMINI_OAUTH2_JS_PATH");
    if (!path || !regular_file_within_limit(path, PROVIDER_MAXIMUM_RESPONSE_BYTES, NULL)) return FALSE;
    char *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &contents, &length, NULL) || length == 0 ||
        length > PROVIDER_MAXIMUM_RESPONSE_BYTES) {
        g_free(contents);
        return FALSE;
    }
    GRegex *id_regex = g_regex_new("(?:const|let|var)?\\s*OAUTH_CLIENT_ID\\s*=\\s*['\"]([A-Za-z0-9_.-]+)['\"]\\s*;",
                                   0, 0, NULL);
    GRegex *secret_regex = g_regex_new(
        "(?:const|let|var)?\\s*OAUTH_CLIENT_SECRET\\s*=\\s*['\"]([A-Za-z0-9_-]+)['\"]\\s*;", 0, 0,
        NULL);
    GMatchInfo *match = NULL;
    if (id_regex && g_regex_match(id_regex, contents, 0, &match)) *client_id = g_match_info_fetch(match, 1);
    g_clear_pointer(&match, g_match_info_free);
    if (secret_regex && g_regex_match(secret_regex, contents, 0, &match)) {
        *client_secret = g_match_info_fetch(match, 1);
    }
    g_clear_pointer(&match, g_match_info_free);
    g_clear_pointer(&id_regex, g_regex_unref);
    g_clear_pointer(&secret_regex, g_regex_unref);
    g_free(contents);
    if (*client_id && *client_secret) return TRUE;
    g_clear_pointer(client_id, g_free);
    g_clear_pointer(client_secret, g_free);
    return FALSE;
}

static char *gemini_refresh_access_token(const char *refresh_token,
                                         CodexBarApiProviders4Transport transport,
                                         GCancellable *cancellable,
                                         GError **error) {
    char *client_id = NULL;
    char *client_secret = NULL;
    if (!gemini_oauth_client(&client_id, &client_secret)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "Gemini OAuth client credentials are unavailable; set GEMINI_OAUTH_CLIENT_ID and "
                            "GEMINI_OAUTH_CLIENT_SECRET");
        return NULL;
    }
    char *escaped_id = g_uri_escape_string(client_id, NULL, TRUE);
    char *escaped_secret = g_uri_escape_string(client_secret, NULL, TRUE);
    char *escaped_refresh = g_uri_escape_string(refresh_token, NULL, TRUE);
    char *body = g_strdup_printf("client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
                                 escaped_id, escaped_secret, escaped_refresh);
    g_free(escaped_refresh);
    g_free(escaped_secret);
    g_free(escaped_id);
    g_free(client_secret);
    g_free(client_id);
    const CodexBarHttpRequestHeader headers[] = {
        {"Accept", "application/json"},
        {"Content-Type", "application/x-www-form-urlencoded"},
    };
    const CodexBarHttpRequest request = {
        .url = GEMINI_REFRESH_URL,
        .method = "POST",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .body = body,
        .body_length = strlen(body),
        .timeout_seconds = 10,
        .maximum_response_bytes = PROVIDER_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    g_free(body);
    if (!response) return NULL;
    if (response->status != 200) {
        gboolean deprecated = response_is_consumer_deprecation(response);
        codexbar_http_response_free(response);
        g_set_error_literal(error, G_IO_ERROR,
                            deprecated ? G_IO_ERROR_NOT_SUPPORTED : G_IO_ERROR_PERMISSION_DENIED,
                            deprecated ? "Gemini Code Assist consumer tier is no longer supported; migrate to Antigravity"
                                       : "Gemini OAuth token refresh failed");
        return NULL;
    }
    json_object *root = parse_json_document(response->body, response->body_length);
    char *access_token = json_string(root, "access_token");
    if (root) json_object_put(root);
    codexbar_http_response_free(response);
    if (access_token) return access_token;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Gemini OAuth refresh response is malformed");
    return NULL;
}

CodexBarProvider *codexbar_gemini_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    (void)config;
    if (!transport) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Gemini transport is missing");
        return NULL;
    }
    GeminiCredentials credentials = {0};
    if (!gemini_load_credentials(&credentials, error)) return NULL;
    char *access_token = NULL;
    if (!credentials.access_token || (credentials.expiry_ms > 0 && credentials.expiry_ms < now_ms)) {
        if (!credentials.refresh_token) {
            gemini_credentials_clear(&credentials);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                "Gemini OAuth token expired; run `gemini` to authenticate");
            return NULL;
        }
        access_token = gemini_refresh_access_token(credentials.refresh_token, transport, cancellable, error);
        if (!access_token) {
            gemini_credentials_clear(&credentials);
            return NULL;
        }
    } else {
        access_token = g_strdup(credentials.access_token);
    }
    CodexBarProvider *provider = codexbar_gemini_fetch_access_token_with_transport_and_cancellable(
        access_token, credentials.id_token, transport, cancellable, now_ms, error);
    g_free(access_token);
    gemini_credentials_clear(&credentials);
    return provider;
}

CodexBarProvider *codexbar_gemini_fetch_with_transport(const CodexBarProviderConfig *config,
                                                       CodexBarApiProviders4Transport transport,
                                                       gint64 now_ms,
                                                       GError **error) {
    return codexbar_gemini_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_gemini_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error) {
    return codexbar_gemini_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_gemini_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_gemini_fetch_with_cancellable(config, NULL, error);
}

typedef struct {
    char *access_token;
    char *refresh_token;
    char *id_token;
    char *email;
    char *project_id;
    char *client_id;
    char *client_secret;
    gint64 expiry_ms;
} AntigravityCredentials;

typedef struct {
    char *model_id;
    char *label;
    double remaining;
    gboolean has_remaining;
    gint64 reset_ms;
} AntigravityRemoteQuota;

static void antigravity_credentials_clear(AntigravityCredentials *credentials) {
    g_free(credentials->access_token);
    g_free(credentials->refresh_token);
    g_free(credentials->id_token);
    g_free(credentials->email);
    g_free(credentials->project_id);
    g_free(credentials->client_id);
    g_free(credentials->client_secret);
    *credentials = (AntigravityCredentials){0};
}

static void antigravity_remote_quota_free(gpointer data) {
    AntigravityRemoteQuota *quota = data;
    if (!quota) return;
    g_free(quota->model_id);
    g_free(quota->label);
    g_free(quota);
}

static char *json_alias_string(json_object *object, const char *snake, const char *camel) {
    char *value = json_string(object, snake);
    if (!value && camel) value = json_string(object, camel);
    return value;
}

static gboolean antigravity_parse_credentials(const char *json,
                                               size_t length,
                                               AntigravityCredentials *credentials) {
    json_object *root = parse_json_document(json, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        return FALSE;
    }
    credentials->access_token = json_alias_string(root, "access_token", "accessToken");
    credentials->refresh_token = json_alias_string(root, "refresh_token", "refreshToken");
    credentials->id_token = json_alias_string(root, "id_token", "idToken");
    credentials->email = json_string(root, "email");
    credentials->project_id = json_alias_string(root, "project_id", "projectId");
    credentials->client_id = json_alias_string(root, "client_id", "clientId");
    credentials->client_secret = json_alias_string(root, "client_secret", "clientSecret");
    double expiry = 0;
    if ((!object_number(root, "expiry_date", &expiry) && !object_number(root, "expiresAt", &expiry)) ||
        expiry <= 0 || expiry > (double)G_MAXINT64) {
        expiry = 0;
    }
    credentials->expiry_ms = (gint64)llround(expiry);
    json_object_put(root);
    if (credentials->access_token || credentials->refresh_token) return TRUE;
    antigravity_credentials_clear(credentials);
    return FALSE;
}

static char *antigravity_config_credentials(const CodexBarProviderConfig *config) {
    if (config && config->api_key && config->api_key[0]) return g_strdup(config->api_key);
    json_object *value = NULL;
    if (config && config->raw &&
        (json_object_object_get_ex(config->raw, "oauthCredentialsJSON", &value) ||
         json_object_object_get_ex(config->raw, "oauthCredentials", &value)) &&
        json_object_is_type(value, json_type_string)) {
        return g_strdup(json_object_get_string(value));
    }
    const char *environment = g_getenv("ANTIGRAVITY_OAUTH_CREDENTIALS_JSON");
    return environment && environment[0] ? g_strdup(environment) : NULL;
}

static gboolean antigravity_load_credentials(const CodexBarProviderConfig *config,
                                              AntigravityCredentials *credentials,
                                              GError **error) {
    char *contents = antigravity_config_credentials(config);
    gsize length = contents ? strlen(contents) : 0;
    if (!contents) {
        const char *home = g_getenv("HOME");
        if (!home || !home[0]) home = g_get_home_dir();
        char *path = home ? g_build_filename(home, ".codexbar", "antigravity", "oauth_creds.json", NULL)
                          : NULL;
        if (!path || !regular_file_within_limit(path, PROVIDER_MAXIMUM_CREDENTIAL_BYTES, NULL) ||
            !g_file_get_contents(path, &contents, &length, NULL)) {
            g_free(path);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                "Antigravity Google auth not found. Configure an OAuth account first.");
            return FALSE;
        }
        g_free(path);
    }
    gboolean parsed = length <= PROVIDER_MAXIMUM_CREDENTIAL_BYTES &&
                      antigravity_parse_credentials(contents, length, credentials);
    g_free(contents);
    if (parsed) return TRUE;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Antigravity OAuth credentials are malformed");
    return FALSE;
}

gboolean codexbar_antigravity_has_oauth_credentials(const CodexBarProviderConfig *config) {
    AntigravityCredentials credentials = {0};
    gboolean loaded = antigravity_load_credentials(config, &credentials, NULL);
    antigravity_credentials_clear(&credentials);
    return loaded;
}

static CodexBarHttpResponse *antigravity_request(const char *url,
                                                 const char *authorization,
                                                 const char *body,
                                                 CodexBarApiProviders4Transport transport,
                                                 GCancellable *cancellable,
                                                 GError **error) {
    const CodexBarHttpRequestHeader headers[] = {
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"Authorization", authorization},
        {"User-Agent", "antigravity"},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "POST",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .body = body,
        .body_length = strlen(body),
        .timeout_seconds = 10,
        .maximum_response_bytes = PROVIDER_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    return send_request(&request, transport, error);
}

static gboolean antigravity_response_success(const CodexBarHttpResponse *response,
                                             const char *operation,
                                             GError **error) {
    if (response && response->status == 200) return TRUE;
    long status = response ? response->status : 0;
    g_set_error(error,
                G_IO_ERROR,
                status == 401 || status == 403 ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_FAILED,
                "Antigravity %s request failed with HTTP %ld",
                operation,
                status);
    return FALSE;
}

static AntigravityRemoteQuota *antigravity_remote_quota(json_object *value,
                                                        const char *model_id,
                                                        const char *fallback_label) {
    if (!value || !json_object_is_type(value, json_type_object) || !model_id || !model_id[0]) return NULL;
    json_object *quota_info = object_member(value, "quotaInfo");
    if (!quota_info || !json_object_is_type(quota_info, json_type_object)) quota_info = value;
    AntigravityRemoteQuota *quota = g_new0(AntigravityRemoteQuota, 1);
    quota->model_id = g_strdup(model_id);
    quota->label = json_string(value, "displayName");
    if (!quota->label) quota->label = json_string(value, "label");
    if (!quota->label) quota->label = g_strdup(fallback_label ? fallback_label : model_id);
    quota->has_remaining = object_number(quota_info, "remainingFraction", &quota->remaining);
    parse_timestamp_ms(object_member(quota_info, "resetTime"), &quota->reset_ms);
    return quota;
}

static GPtrArray *antigravity_model_quotas(json_object *root) {
    GPtrArray *quotas = g_ptr_array_new_with_free_func(antigravity_remote_quota_free);
    json_object *models = object_member(root, "models");
    if (!models || !json_object_is_type(models, json_type_object)) return quotas;
    json_object_object_foreach(models, model_id, model) {
        AntigravityRemoteQuota *quota = antigravity_remote_quota(model, model_id, model_id);
        if (quota) g_ptr_array_add(quotas, quota);
    }
    return quotas;
}

static GPtrArray *antigravity_bucket_quotas(json_object *root) {
    GPtrArray *quotas = g_ptr_array_new_with_free_func(antigravity_remote_quota_free);
    json_object *buckets = object_member(root, "buckets");
    if (!buckets || !json_object_is_type(buckets, json_type_array)) return quotas;
    for (size_t index = 0; index < json_object_array_length(buckets); index++) {
        json_object *bucket = json_object_array_get_idx(buckets, index);
        char *model_id = json_string(bucket, "modelId");
        AntigravityRemoteQuota *quota = antigravity_remote_quota(bucket, model_id, model_id);
        g_free(model_id);
        if (!quota || !quota->has_remaining) {
            antigravity_remote_quota_free(quota);
            continue;
        }
        gboolean merged = FALSE;
        for (guint existing = 0; existing < quotas->len; existing++) {
            AntigravityRemoteQuota *candidate = g_ptr_array_index(quotas, existing);
            if (!g_ascii_strcasecmp(candidate->model_id, quota->model_id)) {
                if (!candidate->has_remaining || quota->remaining < candidate->remaining) {
                    candidate->has_remaining = TRUE;
                    candidate->remaining = quota->remaining;
                    candidate->reset_ms = quota->reset_ms;
                }
                merged = TRUE;
                break;
            }
        }
        antigravity_remote_quota_free(merged ? quota : NULL);
        if (!merged) g_ptr_array_add(quotas, quota);
    }
    return quotas;
}

static int antigravity_remote_family(const AntigravityRemoteQuota *quota) {
    char *combined = g_strdup_printf("%s %s", quota->model_id, quota->label);
    char *lower = g_ascii_strdown(combined, -1);
    g_free(combined);
    int family = strstr(lower, "claude") || strstr(lower, "gpt") || strstr(lower, "openai")
                     ? 1
                     : strstr(lower, "gemini") && !strstr(lower, "image") && !strstr(lower, "lite")
                           ? 0
                           : 2;
    g_free(lower);
    return family;
}

static char *antigravity_plan(const GeminiCodeAssist *status, const char *hosted_domain) {
    return gemini_plan(status, hosted_domain);
}

CodexBarProvider *codexbar_antigravity_parse_remote_usage(const char *models_json,
                                                           size_t models_length,
                                                           const char *quota_json,
                                                           size_t quota_length,
                                                           const char *id_token,
                                                           const char *email,
                                                           const char *code_assist_json,
                                                           size_t code_assist_length,
                                                           gint64 now_ms,
                                                           GError **error) {
    json_object *models_root = models_json ? parse_json_document(models_json, models_length) : NULL;
    json_object *quota_root = quota_json ? parse_json_document(quota_json, quota_length) : NULL;
    if (models_json && !models_root) {
        if (quota_root) json_object_put(quota_root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Antigravity model response is malformed");
        return NULL;
    }
    if (quota_json && !quota_root) {
        if (models_root) json_object_put(models_root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Antigravity quota response is malformed");
        return NULL;
    }
    GPtrArray *model_quotas = antigravity_model_quotas(models_root);
    GPtrArray *bucket_quotas = antigravity_bucket_quotas(quota_root);
    for (guint bucket_index = 0; bucket_index < bucket_quotas->len; bucket_index++) {
        AntigravityRemoteQuota *bucket = g_ptr_array_index(bucket_quotas, bucket_index);
        for (guint model_index = 0; model_index < model_quotas->len; model_index++) {
            AntigravityRemoteQuota *model = g_ptr_array_index(model_quotas, model_index);
            if (g_ascii_strcasecmp(bucket->model_id, model->model_id) != 0) continue;
            g_free(bucket->label);
            bucket->label = g_strdup(model->label);
            if (bucket->reset_ms <= 0) bucket->reset_ms = model->reset_ms;
            break;
        }
    }
    GPtrArray *quotas = bucket_quotas->len > 0 ? bucket_quotas : model_quotas;
    if (quotas == bucket_quotas) g_ptr_array_unref(model_quotas);
    else g_ptr_array_unref(bucket_quotas);
    if (models_root) json_object_put(models_root);
    if (quota_root) json_object_put(quota_root);

    GeminiCodeAssist status = {0};
    if (code_assist_json) gemini_parse_code_assist(code_assist_json, code_assist_length, &status);
    char *token_email = gemini_jwt_claim(id_token, "email");
    char *hosted_domain = gemini_jwt_claim(id_token, "hd");
    char *plan = antigravity_plan(&status, hosted_domain);
    CodexBarProvider *provider = provider_new("antigravity", now_ms);
    g_free(provider->source);
    provider->source = g_strdup("oauth");
    provider->explicit_quota_slots = TRUE;
    provider->account = g_strdup(token_email ? token_email : email);
    provider->plan = g_strdup(plan);
    if (provider->account || provider->plan) {
        provider->identity = g_new0(CodexBarProviderIdentity, 1);
        provider->identity->login_method = g_strdup(provider->plan);
    }
    AntigravityRemoteQuota *representatives[2] = {NULL, NULL};
    for (guint index = 0; index < quotas->len; index++) {
        AntigravityRemoteQuota *quota = g_ptr_array_index(quotas, index);
        int family = antigravity_remote_family(quota);
        if (family < 2 && quota->has_remaining &&
            (!representatives[family] || quota->remaining < representatives[family]->remaining)) {
            representatives[family] = quota;
        }
    }
    const char *ids[] = {"primary", "secondary"};
    const char *titles[] = {"Gemini", "Claude/GPT"};
    for (guint family = 0; family < 2; family++) {
        AntigravityRemoteQuota *quota = representatives[family];
        if (!quota) continue;
        add_window(provider, ids[family], titles[family], (1.0 - CLAMP(quota->remaining, 0.0, 1.0)) * 100.0,
                   0, quota->reset_ms, quota->label);
    }
    for (guint index = 0; index < quotas->len; index++) {
        AntigravityRemoteQuota *quota = g_ptr_array_index(quotas, index);
        int family = antigravity_remote_family(quota);
        if ((family < 2 && representatives[family] == quota) || !quota->has_remaining) continue;
        char *internal = g_strdup_printf("antigravity-extra-%u", index);
        CodexBarQuotaWindow *window = add_window(
            provider, internal, quota->label, (1.0 - CLAMP(quota->remaining, 0.0, 1.0)) * 100.0,
            0, quota->reset_ms, quota->model_id);
        g_free(internal);
        window->output_id = g_strdup(quota->model_id);
    }
    g_ptr_array_unref(quotas);
    gemini_code_assist_clear(&status);
    g_free(hosted_domain);
    g_free(token_email);
    g_free(plan);
    return provider;
}

static char *antigravity_refresh_access_token(const AntigravityCredentials *credentials,
                                              CodexBarApiProviders4Transport transport,
                                              GCancellable *cancellable,
                                              GError **error) {
    char *client_id = clean_credential(credentials->client_id);
    char *client_secret = clean_credential(credentials->client_secret);
    if (!client_id) client_id = clean_credential(g_getenv("ANTIGRAVITY_OAUTH_CLIENT_ID"));
    if (!client_secret) client_secret = clean_credential(g_getenv("ANTIGRAVITY_OAUTH_CLIENT_SECRET"));
    if (!client_id || !client_secret || !credentials->refresh_token) {
        g_free(client_id);
        g_free(client_secret);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "Antigravity OAuth client credentials are unavailable");
        return NULL;
    }
    char *escaped_id = g_uri_escape_string(client_id, NULL, TRUE);
    char *escaped_secret = g_uri_escape_string(client_secret, NULL, TRUE);
    char *escaped_refresh = g_uri_escape_string(credentials->refresh_token, NULL, TRUE);
    char *body = g_strdup_printf("client_id=%s&client_secret=%s&refresh_token=%s&grant_type=refresh_token",
                                 escaped_id, escaped_secret, escaped_refresh);
    g_free(escaped_refresh);
    g_free(escaped_secret);
    g_free(escaped_id);
    g_free(client_secret);
    g_free(client_id);
    const CodexBarHttpRequestHeader headers[] = {
        {"Accept", "application/json"},
        {"Content-Type", "application/x-www-form-urlencoded"},
    };
    const CodexBarHttpRequest request = {
        .url = ANTIGRAVITY_REFRESH_URL,
        .method = "POST",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .body = body,
        .body_length = strlen(body),
        .timeout_seconds = 10,
        .maximum_response_bytes = PROVIDER_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    g_free(body);
    if (!response) return NULL;
    if (response->status != 200) {
        codexbar_http_response_free(response);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Antigravity OAuth token refresh failed");
        return NULL;
    }
    json_object *root = parse_json_document(response->body, response->body_length);
    char *access_token = json_string(root, "access_token");
    if (root) json_object_put(root);
    codexbar_http_response_free(response);
    if (access_token) return access_token;
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                        "Antigravity OAuth refresh response is malformed");
    return NULL;
}

static char *antigravity_project_body(const char *project_id) {
    json_object *body = json_object_new_object();
    if (project_id) json_object_object_add(body, "project", json_object_new_string(project_id));
    char *result = g_strdup(json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN));
    json_object_put(body);
    return result;
}

static char *antigravity_onboard_tier(const CodexBarHttpResponse *assist) {
    if (!assist || assist->status != 200) return NULL;
    json_object *root = parse_json_document(assist->body, assist->body_length);
    json_object *tiers = object_member(root, "allowedTiers");
    char *first = NULL;
    char *selected = NULL;
    if (tiers && json_object_is_type(tiers, json_type_array)) {
        for (size_t index = 0; index < json_object_array_length(tiers); index++) {
            json_object *tier = json_object_array_get_idx(tiers, index);
            char *identifier = json_string(tier, "id");
            if (!identifier) continue;
            if (!first) first = g_strdup(identifier);
            json_object *is_default = object_member(tier, "isDefault");
            if (is_default && json_object_is_type(is_default, json_type_boolean) &&
                json_object_get_boolean(is_default)) {
                selected = identifier;
                break;
            }
            g_free(identifier);
        }
    }
    if (!selected) selected = first;
    else g_free(first);
    if (!selected) selected = json_string(object_member(root, "paidTier"), "id");
    if (!selected) selected = json_string(object_member(root, "currentTier"), "id");
    if (root) json_object_put(root);
    return selected;
}

static char *antigravity_onboard_project(const CodexBarHttpResponse *response) {
    if (!response || response->status != 200) return NULL;
    json_object *root = parse_json_document(response->body, response->body_length);
    json_object *inner = object_member(root, "response");
    json_object *project = object_member(inner, "cloudaicompanionProject");
    char *result = NULL;
    if (project && json_object_is_type(project, json_type_string)) result = json_string(inner, "cloudaicompanionProject");
    else if (project) {
        result = json_string(project, "id");
        if (!result) result = json_string(project, "projectId");
    }
    if (root) json_object_put(root);
    return result;
}

static gboolean antigravity_models_all_full(const CodexBarHttpResponse *response) {
    if (!response || response->status != 200) return FALSE;
    json_object *root = parse_json_document(response->body, response->body_length);
    GPtrArray *quotas = antigravity_model_quotas(root);
    gboolean result = quotas->len > 0;
    for (guint index = 0; result && index < quotas->len; index++) {
        AntigravityRemoteQuota *quota = g_ptr_array_index(quotas, index);
        result = quota->has_remaining && quota->remaining >= 0.999;
    }
    g_ptr_array_unref(quotas);
    if (root) json_object_put(root);
    return result;
}

CodexBarProvider *codexbar_antigravity_oauth_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    if (!transport) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Antigravity transport is missing");
        return NULL;
    }
    AntigravityCredentials credentials = {0};
    if (!antigravity_load_credentials(config, &credentials, error)) return NULL;
    char *access_token = NULL;
    if (!credentials.access_token ||
        (credentials.expiry_ms > 0 && credentials.expiry_ms <= now_ms + 60000)) {
        access_token = antigravity_refresh_access_token(&credentials, transport, cancellable, error);
    } else {
        access_token = g_strdup(credentials.access_token);
    }
    if (!access_token) {
        antigravity_credentials_clear(&credentials);
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", access_token);
    g_free(access_token);
    const char *metadata = "{\"metadata\":{\"ideType\":\"ANTIGRAVITY\",\"platform\":\"PLATFORM_UNSPECIFIED\",\"pluginType\":\"GEMINI\"}}";
    CodexBarHttpResponse *assist = antigravity_request(
        ANTIGRAVITY_CODE_ASSIST_URL, authorization, metadata, transport, cancellable, error);
    if (!assist || !antigravity_response_success(assist, "account", error)) {
        codexbar_http_response_free(assist);
        g_free(authorization);
        antigravity_credentials_clear(&credentials);
        return NULL;
    }
    GeminiCodeAssist status = {0};
    gemini_parse_code_assist(assist->body, assist->body_length, &status);
    char *onboard_project = NULL;
    if (!credentials.project_id && !status.project_id) {
        char *tier = antigravity_onboard_tier(assist);
        if (tier) {
            json_object *body = json_object_new_object();
            json_object_object_add(body, "tierId", json_object_new_string(tier));
            json_object *request_metadata = json_object_new_object();
            json_object_object_add(request_metadata, "ideType", json_object_new_string("ANTIGRAVITY"));
            json_object_object_add(request_metadata, "platform", json_object_new_string("PLATFORM_UNSPECIFIED"));
            json_object_object_add(request_metadata, "pluginType", json_object_new_string("GEMINI"));
            json_object_object_add(body, "metadata", request_metadata);
            const char *serialized = json_object_to_json_string_ext(body, JSON_C_TO_STRING_PLAIN);
            CodexBarHttpResponse *onboard = antigravity_request(
                ANTIGRAVITY_ONBOARD_URL, authorization, serialized, transport, cancellable, NULL);
            onboard_project = antigravity_onboard_project(onboard);
            codexbar_http_response_free(onboard);
            json_object_put(body);
            g_free(tier);
        }
    }
    const char *project_id = credentials.project_id ? credentials.project_id
                                                     : status.project_id ? status.project_id : onboard_project;
    char *request_body = antigravity_project_body(project_id);
    CodexBarHttpResponse *models = antigravity_request(
        ANTIGRAVITY_MODELS_URL, authorization, request_body, transport, cancellable, error);
    CodexBarHttpResponse *quota = NULL;
    if ((models && models->status == 403) || antigravity_models_all_full(models)) {
        if (error) g_clear_error(error);
        quota = antigravity_request(
            ANTIGRAVITY_QUOTA_URL, authorization, request_body, transport, cancellable, error);
        if (quota && quota->status == 403) {
            codexbar_http_response_free(quota);
            quota = NULL;
            if (error) g_clear_error(error);
        }
    }
    g_free(request_body);
    g_free(authorization);
    if ((!models || models->status != 200) && (!quota || quota->status != 200)) {
        long status_code = models ? models->status : 0;
        codexbar_http_response_free(quota);
        codexbar_http_response_free(models);
        codexbar_http_response_free(assist);
        gemini_code_assist_clear(&status);
        g_free(onboard_project);
        antigravity_credentials_clear(&credentials);
        if (!error || !*error) {
            g_set_error(error,
                        G_IO_ERROR,
                        status_code == 401 || status_code == 403 ? G_IO_ERROR_PERMISSION_DENIED
                                                                 : G_IO_ERROR_FAILED,
                        "Antigravity model request failed with HTTP %ld",
                        status_code);
        }
        return NULL;
    }
    CodexBarProvider *provider = codexbar_antigravity_parse_remote_usage(
        models && models->status == 200 ? models->body : NULL,
        models && models->status == 200 ? models->body_length : 0,
        quota && quota->status == 200 ? quota->body : NULL,
        quota && quota->status == 200 ? quota->body_length : 0,
        credentials.id_token,
        credentials.email,
        assist->body,
        assist->body_length,
        now_ms,
        error);
    codexbar_http_response_free(quota);
    codexbar_http_response_free(models);
    codexbar_http_response_free(assist);
    gemini_code_assist_clear(&status);
    g_free(onboard_project);
    antigravity_credentials_clear(&credentials);
    return provider;
}

CodexBarProvider *codexbar_antigravity_fetch_for_source_with_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    GCancellable *cancellable,
    GError **error) {
    const char *mode = source && source[0] ? source : "auto";
    if (g_str_equal(mode, "cli")) {
        return codexbar_antigravity_fetch_with_cancellable(config, cancellable, error);
    }
    if (g_str_equal(mode, "oauth")) {
        return codexbar_antigravity_oauth_fetch_with_transport_and_cancellable(
            config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
    }
    if (!g_str_equal(mode, "auto")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED,
                    "Antigravity source '%s' is unsupported", mode);
        return NULL;
    }
    GError *local_error = NULL;
    CodexBarProvider *provider = codexbar_antigravity_fetch_with_cancellable(
        config, cancellable, &local_error);
    if (provider || (local_error && g_error_matches(local_error, G_IO_ERROR, G_IO_ERROR_CANCELLED))) {
        if (local_error) g_propagate_error(error, local_error);
        return provider;
    }
    if (!codexbar_antigravity_has_oauth_credentials(config)) {
        if (local_error) g_propagate_error(error, local_error);
        else g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                                 "Antigravity is not running and OAuth credentials are unavailable");
        return NULL;
    }
    g_clear_error(&local_error);
    return codexbar_antigravity_oauth_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

static const char *const ollama_environment_keys[] = {
    "OLLAMA_API_KEY",
    "OLLAMA_KEY",
    NULL,
};

static char *ollama_api_key(const CodexBarProviderConfig *config) {
    char *key = clean_credential(config ? config->api_key : NULL);
    for (size_t index = 0; !key && ollama_environment_keys[index]; index++) {
        key = clean_credential(g_getenv(ollama_environment_keys[index]));
    }
    return key;
}

gboolean codexbar_ollama_has_api_key(const CodexBarProviderConfig *config) {
    char *key = ollama_api_key(config);
    gboolean present = key != NULL;
    g_free(key);
    return present;
}

static char *ollama_config_string(const CodexBarProviderConfig *config, const char *key) {
    json_object *value = NULL;
    if (!config || !config->raw || !json_object_object_get_ex(config->raw, key, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    return clean_credential(json_object_get_string(value));
}

static char *ollama_cookie(const CodexBarProviderConfig *config) {
    char *cookie = ollama_config_string(config, "cookieHeader");
    if (!cookie) cookie = ollama_config_string(config, "manualCookieHeader");
    if (!cookie) cookie = clean_credential(g_getenv("OLLAMA_COOKIE_HEADER"));
    if (!cookie) cookie = clean_credential(g_getenv("OLLAMA_COOKIE"));
    if (!cookie) return NULL;
    if (!strchr(cookie, '=')) {
        char *normalized = g_strdup_printf("__Secure-session=%s", cookie);
        g_free(cookie);
        return normalized;
    }
    return cookie;
}

static char *ollama_regex_capture(const char *text, const char *pattern, GRegexCompileFlags flags) {
    GRegex *regex = g_regex_new(pattern, flags, 0, NULL);
    if (!regex) return NULL;
    GMatchInfo *match = NULL;
    char *capture = g_regex_match(regex, text, 0, &match) ? g_match_info_fetch(match, 1) : NULL;
    g_match_info_free(match);
    g_regex_unref(regex);
    if (capture) g_strstrip(capture);
    if (capture && capture[0] == '\0') g_clear_pointer(&capture, g_free);
    return capture;
}

static gboolean ollama_parse_percent(const char *text, double *result) {
    char *value = ollama_regex_capture(
        text, "([0-9]+(?:\\.[0-9]+)?)\\s*%\\s*used", G_REGEX_CASELESS);
    if (!value) {
        value = ollama_regex_capture(text, "width:\\s*([0-9]+(?:\\.[0-9]+)?)%", G_REGEX_CASELESS);
    }
    if (!value) return FALSE;
    char *end = NULL;
    double percent = g_ascii_strtod(value, &end);
    gboolean valid = end && *end == '\0' && isfinite(percent);
    g_free(value);
    if (valid) *result = percent;
    return valid;
}

static gboolean ollama_parse_reset(const char *text, gint64 *result) {
    char *value = ollama_regex_capture(text, "data-time=\\\"([^\\\"]+)\\\"", 0);
    if (!value) return FALSE;
    GDateTime *date = g_date_time_new_from_iso8601(value, NULL);
    g_free(value);
    if (!date) return FALSE;
    *result = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static gboolean ollama_usage_block(const char *html,
                                   const char *label,
                                   const char *alternate_label,
                                   double *percent,
                                   gint64 *resets_at_ms,
                                   gboolean *has_reset) {
    const char *start = strstr(html, label);
    if (!start && alternate_label) start = strstr(html, alternate_label);
    if (!start) return FALSE;
    start += strlen(strstr(html, label) == start ? label : alternate_label);
    const char *end = html + strlen(html);
    static const char *const labels[] = {"Session usage", "Hourly usage", "Weekly usage", NULL};
    for (size_t index = 0; labels[index]; index++) {
        const char *candidate = strstr(start, labels[index]);
        if (candidate && candidate < end) end = candidate;
    }
    if ((size_t)(end - start) > 4000) end = start + 4000;
    char *window = g_strndup(start, (size_t)(end - start));
    gboolean found = ollama_parse_percent(window, percent);
    *has_reset = found && ollama_parse_reset(window, resets_at_ms);
    g_free(window);
    return found;
}

static gboolean ollama_looks_signed_out(const char *html) {
    char *lower = g_ascii_strdown(html, -1);
    gboolean form = strstr(lower, "<form") != NULL;
    gboolean heading = strstr(lower, "sign in to ollama") || strstr(lower, "log in to ollama");
    gboolean route = strstr(lower, "/api/auth/signin") || strstr(lower, "/auth/signin") ||
                     strstr(lower, "action=\"/login\"") || strstr(lower, "action='/login'") ||
                     strstr(lower, "href=\"/login\"") || strstr(lower, "href='/login'") ||
                     strstr(lower, "action=\"/signin\"") || strstr(lower, "action='/signin'") ||
                     strstr(lower, "href=\"/signin\"") || strstr(lower, "href='/signin'");
    gboolean password = strstr(lower, "type=\"password\"") || strstr(lower, "type='password'") ||
                        strstr(lower, "name=\"password\"") || strstr(lower, "name='password'");
    gboolean email = strstr(lower, "type=\"email\"") || strstr(lower, "type='email'") ||
                     strstr(lower, "name=\"email\"") || strstr(lower, "name='email'");
    gboolean signed_out = (heading && form && (email || password || route)) || (form && route) ||
                          (form && password && email);
    g_free(lower);
    return signed_out;
}

CodexBarProvider *codexbar_ollama_parse_settings_html(const char *html,
                                                      size_t length,
                                                      gint64 now_ms,
                                                      GError **error) {
    if (!html || length == 0 || length > PROVIDER_MAXIMUM_RESPONSE_BYTES || memchr(html, '\0', length) ||
        !g_utf8_validate(html, (gssize)length, NULL)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Ollama settings response is invalid");
        return NULL;
    }
    char *document = g_strndup(html, length);
    double primary_percent = 0, weekly_percent = 0;
    gint64 primary_reset = 0, weekly_reset = 0;
    gboolean primary_has_reset = FALSE, weekly_has_reset = FALSE;
    gboolean session = strstr(document, "Session usage") != NULL;
    gboolean has_primary = ollama_usage_block(
        document, "Session usage", "Hourly usage", &primary_percent, &primary_reset, &primary_has_reset);
    gboolean has_weekly = ollama_usage_block(
        document, "Weekly usage", NULL, &weekly_percent, &weekly_reset, &weekly_has_reset);
    if (!has_primary && !has_weekly) {
        gboolean signed_out = ollama_looks_signed_out(document);
        g_free(document);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            signed_out ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_INVALID_DATA,
                            signed_out ? "Ollama session cookie is invalid or expired"
                                       : "Ollama usage data is missing");
        return NULL;
    }
    CodexBarProvider *provider = provider_new("ollama", now_ms);
    g_free(provider->source);
    provider->source = g_strdup("web");
    provider->dashboard_url = g_strdup(OLLAMA_SETTINGS_URL);
    provider->explicit_quota_slots = TRUE;
    char *plan = ollama_regex_capture(
        document, "Cloud Usage\\s*</span>\\s*<span[^>]*>([^<]+)</span>", G_REGEX_DOTALL);
    char *email = ollama_regex_capture(document, "id=\\\"header-email\\\"[^>]*>([^<]+)<", G_REGEX_DOTALL);
    if (email && !strchr(email, '@')) g_clear_pointer(&email, g_free);
    provider->plan = plan ? g_strdup(plan) : NULL;
    provider->account = email;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = plan;
    if (has_primary) {
        add_window(provider,
                   "primary",
                   session ? "Session" : "Hourly",
                   primary_percent,
                   session ? 300 : 0,
                   primary_has_reset ? primary_reset : 0,
                   NULL);
    }
    if (has_weekly) {
        add_window(provider,
                   "secondary",
                   "Weekly",
                   weekly_percent,
                   10080,
                   weekly_has_reset ? weekly_reset : 0,
                   NULL);
    }
    g_free(document);
    return provider;
}

CodexBarProvider *codexbar_ollama_parse_api_tags(const char *json,
                                                size_t length,
                                                gint64 now_ms,
                                                GError **error) {
    json_object *root = parse_json_document(json, length);
    json_object *models = object_member(root, "models");
    if (!root || !models || !json_object_is_type(models, json_type_array)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Ollama model catalog response is malformed");
        return NULL;
    }
    CodexBarProvider *provider = provider_new("ollama", now_ms);
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = g_strdup("API key");
    provider->plan = g_strdup("API key");
    json_object_put(root);
    return provider;
}

static int uri_effective_port(GUri *uri) {
    int port = g_uri_get_port(uri);
    if (port >= 0) return port;
    const char *scheme = g_uri_get_scheme(uri);
    return scheme && g_ascii_strcasecmp(scheme, "https") == 0 ? 443 : 80;
}

static gboolean ollama_same_origin(const char *lhs, const char *rhs) {
    GUri *left = g_uri_parse(lhs, G_URI_FLAGS_NONE, NULL);
    GUri *right = g_uri_parse(rhs, G_URI_FLAGS_NONE, NULL);
    gboolean same = left && right &&
                    g_ascii_strcasecmp(g_uri_get_scheme(left), g_uri_get_scheme(right)) == 0 &&
                    g_ascii_strcasecmp(g_uri_get_host(left), g_uri_get_host(right)) == 0 &&
                    uri_effective_port(left) == uri_effective_port(right);
    if (left) g_uri_unref(left);
    if (right) g_uri_unref(right);
    return same;
}

static CodexBarHttpProtocolPolicy ollama_protocol_policy(const char *url) {
    return g_ascii_strncasecmp(url, "http://", strlen("http://")) == 0
               ? CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP
               : CODEXBAR_HTTP_HTTPS_ONLY;
}

CodexBarProvider *codexbar_ollama_fetch_endpoints_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    const char *tags_url,
    const char *validation_url,
    CodexBarApiProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    if (!transport) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Ollama transport is missing");
        return NULL;
    }
    char *key = ollama_api_key(config);
    if (!key) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "Ollama API key missing. Set OLLAMA_API_KEY or configure api_key.");
        return NULL;
    }
    char *normalized_tags = codexbar_http_normalize_endpoint(
        tags_url, CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP, error);
    char *normalized_validation = normalized_tags
                                      ? codexbar_http_normalize_endpoint(
                                            validation_url, CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP, error)
                                      : NULL;
    if (!normalized_tags || !normalized_validation ||
        !ollama_same_origin(normalized_tags, normalized_validation)) {
        if (normalized_tags && normalized_validation && error && !*error) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                                "Ollama validation and model catalog endpoints must share an origin");
        }
        g_free(normalized_validation);
        g_free(normalized_tags);
        g_free(key);
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", key);
    g_free(key);
    const CodexBarHttpRequestHeader validation_headers[] = {
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"User-Agent", "CodexBar/1.0"},
        {"Authorization", authorization},
    };
    static const char validation_body[] = "{\"query\":\"\"}";
    const CodexBarHttpRequest validation_request = {
        .url = normalized_validation,
        .method = "POST",
        .headers = validation_headers,
        .header_count = G_N_ELEMENTS(validation_headers),
        .body = validation_body,
        .body_length = sizeof(validation_body) - 1,
        .timeout_seconds = OLLAMA_TIMEOUT_SECONDS,
        .maximum_response_bytes = PROVIDER_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = ollama_protocol_policy(normalized_validation),
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *validation = send_request(&validation_request, transport, error);
    if (!validation) {
        g_free(authorization);
        g_free(normalized_validation);
        g_free(normalized_tags);
        return NULL;
    }
    if (validation->status == 401 || validation->status == 403) {
        codexbar_http_response_free(validation);
        g_free(authorization);
        g_free(normalized_validation);
        g_free(normalized_tags);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Ollama API key is invalid or revoked");
        return NULL;
    }
    if (validation->status != 200 && validation->status != 400) {
        long status = validation->status;
        codexbar_http_response_free(validation);
        g_free(authorization);
        g_free(normalized_validation);
        g_free(normalized_tags);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "Ollama API key validation failed with HTTP %ld", status);
        return NULL;
    }
    codexbar_http_response_free(validation);
    const CodexBarHttpRequestHeader tags_headers[] = {
        {"Accept", "application/json"},
        {"User-Agent", "CodexBar/1.0"},
        {"Authorization", authorization},
    };
    const CodexBarHttpRequest tags_request = {
        .url = normalized_tags,
        .method = "GET",
        .headers = tags_headers,
        .header_count = G_N_ELEMENTS(tags_headers),
        .timeout_seconds = OLLAMA_TIMEOUT_SECONDS,
        .maximum_response_bytes = PROVIDER_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = ollama_protocol_policy(normalized_tags),
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *tags = send_request(&tags_request, transport, error);
    g_free(authorization);
    g_free(normalized_validation);
    g_free(normalized_tags);
    if (!tags) return NULL;
    if (tags->status == 401 || tags->status == 403) {
        codexbar_http_response_free(tags);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Ollama API key is invalid or revoked");
        return NULL;
    }
    if (tags->status != 200) {
        long status = tags->status;
        codexbar_http_response_free(tags);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Ollama model catalog failed with HTTP %ld", status);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_ollama_parse_api_tags(
        tags->body, tags->body_length, now_ms, error);
    codexbar_http_response_free(tags);
    return provider;
}

CodexBarProvider *codexbar_ollama_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarApiProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    return codexbar_ollama_fetch_endpoints_with_transport_and_cancellable(
        config, OLLAMA_TAGS_URL, OLLAMA_VALIDATION_URL, transport, cancellable, now_ms, error);
}

static CodexBarProvider *ollama_fetch_web(const CodexBarProviderConfig *config,
                                          CodexBarApiProviders4Transport transport,
                                          GCancellable *cancellable,
                                          gint64 now_ms,
                                          GError **error) {
    char *cookie = ollama_cookie(config);
    if (!cookie) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "Ollama web session is missing; configure cookieHeader or OLLAMA_COOKIE_HEADER");
        return NULL;
    }
    const CodexBarHttpRequestHeader headers[] = {
        {"Cookie", cookie},
        {"Accept", "text/html,application/xhtml+xml,application/xml;q=0.9,*/*;q=0.8"},
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
                       "Chrome/143.0.0.0 Safari/537.36"},
        {"Accept-Language", "en-US,en;q=0.9"},
        {"Origin", "https://ollama.com"},
        {"Referer", OLLAMA_SETTINGS_URL},
    };
    const CodexBarHttpRequest request = {
        .url = OLLAMA_SETTINGS_URL,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = OLLAMA_TIMEOUT_SECONDS,
        .maximum_response_bytes = PROVIDER_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    g_free(cookie);
    if (!response) return NULL;
    if (response->status != 200) {
        g_set_error(error,
                    G_IO_ERROR,
                    response->status == 401 || response->status == 403 ? G_IO_ERROR_PERMISSION_DENIED
                                                                       : G_IO_ERROR_FAILED,
                    "Ollama settings endpoint returned HTTP %ld",
                    response->status);
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_ollama_parse_settings_html(
        response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

CodexBarProvider *codexbar_ollama_fetch_for_source_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    CodexBarApiProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    const char *mode = source && source[0] ? source : "auto";
    if (g_str_equal(mode, "api")) {
        return codexbar_ollama_fetch_with_transport_and_cancellable(
            config, transport, cancellable, now_ms, error);
    }
    if (g_str_equal(mode, "web")) return ollama_fetch_web(config, transport, cancellable, now_ms, error);
    if (!g_str_equal(mode, "auto")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Ollama source '%s' is unsupported", mode);
        return NULL;
    }
    char *cookie = ollama_cookie(config);
    gboolean has_cookie = cookie != NULL;
    g_free(cookie);
    if (has_cookie) {
        GError *web_error = NULL;
        CodexBarProvider *provider = ollama_fetch_web(config, transport, cancellable, now_ms, &web_error);
        if (provider) return provider;
        if (web_error && g_error_matches(web_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            if (error) *error = web_error;
            else g_clear_error(&web_error);
            return NULL;
        }
        if (!codexbar_ollama_has_api_key(config)) {
            if (error) *error = web_error;
            else g_clear_error(&web_error);
            return NULL;
        }
        g_clear_error(&web_error);
    }
    return codexbar_ollama_fetch_with_transport_and_cancellable(
        config, transport, cancellable, now_ms, error);
}

CodexBarProvider *codexbar_ollama_fetch_with_transport(const CodexBarProviderConfig *config,
                                                       CodexBarApiProviders4Transport transport,
                                                       gint64 now_ms,
                                                       GError **error) {
    return codexbar_ollama_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_ollama_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error) {
    return codexbar_ollama_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_ollama_fetch_for_source_with_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    GCancellable *cancellable,
    GError **error) {
    return codexbar_ollama_fetch_for_source_with_transport_and_cancellable(
        config, source, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_ollama_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_ollama_fetch_with_cancellable(config, NULL, error);
}
