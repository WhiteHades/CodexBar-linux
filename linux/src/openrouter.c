#include "openrouter.h"

#include "http.h"
#include "token_accounts.h"

#include <json-c/json.h>
#include <math.h>

#define OPENROUTER_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)

static GQuark openrouter_error_quark(void) {
    return g_quark_from_static_string("codexbar-openrouter-error");
}

static char *resolve_base_url(GError **error) {
    const char *raw = g_getenv("OPENROUTER_API_URL");
    if (!raw || raw[0] == '\0') {
        return g_strdup("https://openrouter.ai/api/v1");
    }
    char *candidate = strstr(raw, "://") ? g_strdup(raw) : g_strdup_printf("https://%s", raw);
    GUri *uri = g_uri_parse(candidate, G_URI_FLAGS_NONE, NULL);
    if (!uri || !g_str_equal(g_uri_get_scheme(uri), "https") || !g_uri_get_host(uri)) {
        g_set_error_literal(error, openrouter_error_quark(), 1,
                            "OPENROUTER_API_URL must use HTTPS or be a bare host");
        if (uri) {
            g_uri_unref(uri);
        }
        g_free(candidate);
        return NULL;
    }
    g_uri_unref(uri);
    return candidate;
}

CodexBarProvider *codexbar_openrouter_parse_credits(const char *json, GError **error) {
    json_object *root = json_tokener_parse(json);
    json_object *data = NULL;
    json_object *total_credits = NULL;
    json_object *total_usage = NULL;
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "data", &data) || !json_object_is_type(data, json_type_object) ||
        !json_object_object_get_ex(data, "total_credits", &total_credits) ||
        !json_object_object_get_ex(data, "total_usage", &total_usage)) {
        g_set_error_literal(error, openrouter_error_quark(), 2, "OpenRouter credits response is malformed");
        if (root) {
            json_object_put(root);
        }
        return NULL;
    }
    double credits = json_object_get_double(total_credits);
    double usage = json_object_get_double(total_usage);
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("openrouter");
    provider->source = g_strdup("api");
    CodexBarBalance *balance = codexbar_balance_new(
        "credits", "credits", MAX(0.0, credits - usage), "credits");
    balance->has_used = TRUE;
    balance->used = usage;
    balance->has_limit = TRUE;
    balance->limit = credits;
    codexbar_provider_add_balance(provider, balance);
    CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "session");
    window->usage_known = TRUE;
    window->used_percent = credits > 0.0
                               ? codexbar_usage_percent_display(codexbar_usage_percent_from_ratio(usage, credits))
                               : 0.0;
    codexbar_provider_add_quota_window(provider, window);
    json_object_put(root);
    return provider;
}

static gboolean openrouter_number(json_object *object, const char *key, double *result) {
    json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value) ||
        !(json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double))) {
        return FALSE;
    }
    *result = json_object_get_double(value);
    return isfinite(*result);
}

static void apply_key_limit(CodexBarProvider *provider, const char *json) {
    json_object *root = json_tokener_parse(json);
    json_object *data = NULL;
    double limit = 0;
    if (!root || !json_object_object_get_ex(root, "data", &data) ||
        !json_object_is_type(data, json_type_object) ||
        !openrouter_number(data, "limit", &limit) || limit <= 0) {
        if (root) json_object_put(root);
        return;
    }
    double used = 0;
    double remaining = 0;
    gboolean has_used = FALSE;
    if (openrouter_number(data, "limit_remaining", &remaining)) {
        used = limit - CLAMP(remaining, 0.0, limit);
        has_used = TRUE;
    } else {
        json_object *reset = NULL;
        const char *usage_key = NULL;
        if (json_object_object_get_ex(data, "limit_reset", &reset) &&
            json_object_is_type(reset, json_type_string)) {
            const char *name = json_object_get_string(reset);
            usage_key = g_ascii_strcasecmp(name, "daily") == 0     ? "usage_daily"
                        : g_ascii_strcasecmp(name, "weekly") == 0  ? "usage_weekly"
                        : g_ascii_strcasecmp(name, "monthly") == 0 ? "usage_monthly"
                                                                  : NULL;
        }
        has_used = usage_key && openrouter_number(data, usage_key, &used);
        if (!has_used) has_used = openrouter_number(data, "usage", &used);
    }
    if (has_used && used >= 0) {
        CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "API key");
        window->usage_known = TRUE;
        window->used_percent = codexbar_usage_percent_display(
            codexbar_usage_percent_from_ratio(used, limit));
        codexbar_provider_add_quota_window(provider, window);
    }
    json_object_put(root);
}

CodexBarProvider *codexbar_openrouter_fetch_with_transport(const CodexBarProviderConfig *config,
                                                           CodexBarOpenRouterTransport transport,
                                                           GError **error) {
    const char *token = codexbar_token_account_is_selected(config) ? config->api_key : g_getenv("OPENROUTER_API_KEY");
    if (!token || token[0] == '\0') {
        token = config->api_key;
    }
    if (!token || token[0] == '\0') {
        g_set_error_literal(error, openrouter_error_quark(), 3, "OpenRouter API token is not configured");
        return NULL;
    }
    char *base = resolve_base_url(error);
    if (!base) {
        return NULL;
    }
    char *bearer = g_strdup_printf("Bearer %s", token);
    const CodexBarHttpRequestHeader headers[] = {{"Authorization", bearer}};
    char *url = g_strdup_printf("%s%scredits", base, g_str_has_suffix(base, "/") ? "" : "/");
    CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 15,
        .maximum_response_bytes = OPENROUTER_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
    };
    CodexBarHttpResponse *response = transport(&request, error);
    g_free(url);
    if (!response) {
        g_free(bearer);
        g_free(base);
        return NULL;
    }
    if (response->status != 200) {
        g_set_error(error, openrouter_error_quark(), 4, "OpenRouter API returned HTTP %ld", response->status);
        codexbar_http_response_free(response);
        g_free(bearer);
        g_free(base);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_openrouter_parse_credits(response->body, error);
    codexbar_http_response_free(response);
    if (provider) {
        url = g_strdup_printf("%s%skey", base, g_str_has_suffix(base, "/") ? "" : "/");
        request.url = url;
        request.timeout_seconds = 1;
        response = transport(&request, NULL);
        g_free(url);
        if (response) {
            if (response->status == 200) apply_key_limit(provider, response->body);
            codexbar_http_response_free(response);
        }
    }
    g_free(bearer);
    g_free(base);
    return provider;
}

CodexBarProvider *codexbar_openrouter_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_openrouter_fetch_with_transport(config, codexbar_http_send, error);
}
