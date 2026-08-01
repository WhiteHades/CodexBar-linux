#include "zed.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define RESPONSE_LIMIT (1024U * 1024U)
#define CREDENTIAL_LIMIT 16384U

static json_object *parse_json(const char *text, size_t length) {
    if (!text || !length || length > RESPONSE_LIMIT || length > G_MAXINT ||
        memchr(text, '\0', length) || !g_utf8_validate(text, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, text, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && strchr(" \t\r\n", text[consumed])) consumed++;
    gboolean valid = parse_error == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static json_object *member(json_object *object, const char *key) {
    json_object *value = NULL;
    return object && json_object_is_type(object, json_type_object) &&
                   json_object_object_get_ex(object, key, &value)
               ? value
               : NULL;
}

static char *string_member(json_object *object, const char *key) {
    json_object *value = member(object, key);
    if (!value || !json_object_is_type(value, json_type_string)) return NULL;
    const char *raw = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!raw || length > CREDENTIAL_LIMIT || memchr(raw, '\0', length) ||
        !g_utf8_validate(raw, (gssize)length, NULL)) return NULL;
    char *copy = g_strndup(raw, length);
    g_strstrip(copy);
    if (copy[0]) return copy;
    g_free(copy);
    return NULL;
}

static gboolean integer_member(json_object *object, const char *key, gint64 *result) {
    json_object *value = member(object, key);
    if (!value || !json_object_is_type(value, json_type_int)) return FALSE;
    *result = json_object_get_int64(value);
    return TRUE;
}

static gboolean boolean_member(json_object *object, const char *key, gboolean *result) {
    json_object *value = member(object, key);
    if (!value || !json_object_is_type(value, json_type_boolean)) return FALSE;
    *result = json_object_get_boolean(value);
    return TRUE;
}

static gboolean timestamp_member(json_object *object, const char *key, gint64 *result) {
    char *text = string_member(object, key);
    if (!text) return FALSE;
    GDateTime *date = g_date_time_new_from_iso8601(text, NULL);
    g_free(text);
    if (!date) return FALSE;
    *result = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static char *plan_name(const char *raw) {
    if (g_ascii_strcasecmp(raw, "zed_free") == 0) return g_strdup("Zed Free");
    if (g_ascii_strcasecmp(raw, "zed_pro") == 0) return g_strdup("Zed Pro");
    if (g_ascii_strcasecmp(raw, "zed_pro_trial") == 0) return g_strdup("Zed Pro Trial");
    if (g_ascii_strcasecmp(raw, "zed_student") == 0) return g_strdup("Zed Student");
    if (g_ascii_strcasecmp(raw, "zed_business") == 0) return g_strdup("Zed Business");
    char *copy = g_ascii_strdown(raw, -1);
    gboolean capitalize = TRUE;
    for (char *cursor = copy; *cursor; cursor++) {
        if (*cursor == '_') {
            *cursor = ' ';
            capitalize = TRUE;
        } else if (capitalize) {
            *cursor = (char)g_ascii_toupper(*cursor);
            capitalize = FALSE;
        }
    }
    return copy;
}

static char *cycle_description(gint64 end_ms, gint64 now_ms) {
    gint64 seconds = (end_ms - now_ms) / 1000;
    if (seconds <= 0) return g_strdup("Cycle ended");
    gint64 hours = seconds / 3600;
    gint64 minutes = (seconds % 3600) / 60;
    if (hours >= 24) return g_strdup_printf("Cycle ends in %" G_GINT64_FORMAT "d %" G_GINT64_FORMAT "h",
                                             hours / 24, hours % 24);
    if (hours > 0) return g_strdup_printf("Cycle ends in %" G_GINT64_FORMAT "h %" G_GINT64_FORMAT "m",
                                           hours, minutes);
    return g_strdup_printf("Cycle ends in %" G_GINT64_FORMAT "m", minutes);
}

static gboolean add_predictions(CodexBarProvider *provider, json_object *usage, GError **error) {
    json_object *predictions = member(usage, "edit_predictions");
    gint64 used = 0;
    if (!predictions || !integer_member(predictions, "used", &used)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Zed response is missing edit prediction usage");
        return FALSE;
    }
    json_object *limit = member(predictions, "limit");
    gboolean unlimited = limit && json_object_is_type(limit, json_type_string) &&
                         g_str_equal(json_object_get_string(limit), "unlimited");
    gint64 total = 0;
    if (!unlimited) {
        if (limit && json_object_is_type(limit, json_type_int)) {
            total = json_object_get_int64(limit);
        } else if (!integer_member(limit, "limited", &total)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Zed response has an unrecognized prediction limit");
            return FALSE;
        }
    }
    CodexBarQuotaWindow *window = codexbar_quota_window_new("zed.edit-predictions", "Edit predictions");
    window->usage_known = TRUE;
    if (unlimited) {
        window->used_percent = 0;
        window->detail = g_strdup("Unlimited");
    } else if (total > 0) {
        gint64 clamped = CLAMP(used, 0, total);
        window->used_percent = (double)clamped / (double)total * 100;
        window->detail = g_strdup_printf("%" G_GINT64_FORMAT " / %" G_GINT64_FORMAT " predictions",
                                         clamped, total);
    } else {
        codexbar_quota_window_free(window);
        return TRUE;
    }
    codexbar_provider_add_quota_window(provider, window);
    return TRUE;
}

CodexBarProvider *codexbar_zed_parse(const char *json,
                                     size_t length,
                                     gint64 now_ms,
                                     GError **error) {
    json_object *root = parse_json(json, length);
    json_object *user = member(root, "user");
    json_object *plan = member(root, "plan");
    json_object *usage = member(plan, "usage");
    char *github_login = string_member(user, "github_login");
    char *name = string_member(user, "name");
    char *raw_plan = string_member(plan, "plan_v3");
    gint64 user_id = 0;
    gboolean overdue = FALSE;
    if (!root || !user || !plan || !usage || !github_login || !raw_plan ||
        !integer_member(user, "id", &user_id) ||
        !boolean_member(plan, "has_overdue_invoices", &overdue)) {
        g_free(github_login);
        g_free(name);
        g_free(raw_plan);
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse Zed account response");
        return NULL;
    }

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("zed");
    provider->source = g_strdup("api");
    provider->account = github_login;
    provider->plan = plan_name(raw_plan);
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->organization = name;
    provider->identity->account_id = g_strdup_printf("%" G_GINT64_FORMAT, user_id);
    provider->identity->login_method = g_strdup(provider->plan);
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    if (!add_predictions(provider, usage, error)) {
        codexbar_provider_free(provider);
        g_free(raw_plan);
        json_object_put(root);
        return NULL;
    }

    json_object *period = member(plan, "subscription_period");
    gint64 started_ms = 0;
    gint64 ended_ms = 0;
    gboolean has_period = period && !json_object_is_type(period, json_type_null);
    if (has_period && (!json_object_is_type(period, json_type_object) ||
                       !timestamp_member(period, "started_at", &started_ms) ||
                       !timestamp_member(period, "ended_at", &ended_ms))) {
        codexbar_provider_free(provider);
        g_free(raw_plan);
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Could not parse Zed subscription period");
        return NULL;
    }
    if (has_period && ended_ms > started_ms) {
        CodexBarQuotaWindow *window = codexbar_quota_window_new("zed.billing-cycle", "Billing cycle");
        window->usage_known = TRUE;
        window->used_percent = CLAMP((double)(now_ms - started_ms) / (double)(ended_ms - started_ms) * 100, 0, 100);
        window->has_resets_at = TRUE;
        window->resets_at_ms = ended_ms;
        window->reset_description = cycle_description(ended_ms, now_ms);
        codexbar_provider_add_quota_window(provider, window);
        provider->has_subscription_renews_at = TRUE;
        provider->subscription_renews_at_ms = ended_ms;
    }
    if (overdue) {
        CodexBarQuotaWindow *window = codexbar_quota_window_new("zed.overdue-invoices", "Billing");
        window->usage_known = FALSE;
        window->used_percent = 100;
        window->reset_description = g_strdup("Overdue invoices");
        codexbar_provider_add_quota_window(provider, window);
    }
    g_free(raw_plan);
    json_object_put(root);
    return provider;
}

static char *clean_credential(const char *raw) {
    if (!raw || strlen(raw) > CREDENTIAL_LIMIT || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *copy = g_strstrip(g_strdup(raw));
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

static char *config_string(const CodexBarProviderConfig *config, const char *key) {
    return string_member(config ? config->raw : NULL, key);
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

static char *endpoint(const CodexBarProviderConfig *config, GError **error) {
    char *server = config_string(config, "serverURL");
    char *credentials = config_string(config, "credentialsURL");
    if (!server) server = g_strdup("https://zed.dev");
    gboolean trusted = g_str_equal(server, "https://zed.dev") ||
                       g_str_equal(server, "https://staging.zed.dev");
    if (!trusted && credentials && !g_str_equal(credentials, server)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Zed custom credentials and server URLs must have the same origin");
        g_free(server);
        g_free(credentials);
        return NULL;
    }
    const char *base = trusted ? "https://cloud.zed.dev" : server;
    char *normalized = codexbar_http_normalize_endpoint(base, CODEXBAR_HTTP_HTTPS_ONLY, error);
    g_free(server);
    g_free(credentials);
    if (!normalized) return NULL;
    while (g_str_has_suffix(normalized, "/")) normalized[strlen(normalized) - 1] = '\0';
    char *url = g_strdup_printf("%s/client/users/me", normalized);
    g_free(normalized);
    return url;
}

CodexBarProvider *codexbar_zed_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarZedTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *user_raw = config_string(config, "userId");
    char *token_raw = config_string(config, "accessToken");
    char *user_id = clean_credential(user_raw ? user_raw : g_getenv("ZED_USER_ID"));
    char *access_token = clean_credential(token_raw ? token_raw :
                                          config && config->api_key ? config->api_key : g_getenv("ZED_ACCESS_TOKEN"));
    g_free(user_raw);
    g_free(token_raw);
    if (!user_id || !access_token) {
        g_free(user_id);
        g_free(access_token);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Zed requires userId/accessToken or ZED_USER_ID/ZED_ACCESS_TOKEN");
        return NULL;
    }
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) {
        g_free(user_id);
        g_free(access_token);
        return NULL;
    }
    char *url = endpoint(config, error);
    if (!url) {
        g_free(user_id);
        g_free(access_token);
        return NULL;
    }
    char *authorization = g_strdup_printf("%s %s", user_id, access_token);
    CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
    };
    CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 15,
        .maximum_response_bytes = RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = transport ? transport(&request, error) : NULL;
    g_free(authorization);
    g_free(user_id);
    g_free(access_token);
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        codexbar_http_response_free(response);
        if (error && *error) g_clear_error(error);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        g_free(url);
        return NULL;
    }
    if (!transport && (!error || !*error)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Zed transport is missing");
    }
    if (!response) {
        g_free(url);
        return NULL;
    }
    if (!same_origin(response, url)) {
        codexbar_http_response_free(response);
        g_free(url);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Zed redirected outside its trusted origin");
        return NULL;
    }
    g_free(url);
    if (response->status != 200) {
        long status = response->status;
        codexbar_http_response_free(response);
        if (status == 401 || status == 403) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                "Zed credentials are invalid or expired");
        } else {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Zed cloud API returned HTTP %ld", status);
        }
        return NULL;
    }
    CodexBarProvider *provider = codexbar_zed_parse(response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

CodexBarProvider *codexbar_zed_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                      GCancellable *cancellable,
                                                      GError **error) {
    return codexbar_zed_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}
