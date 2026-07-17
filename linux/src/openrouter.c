#include "openrouter.h"

#include "http.h"

#include <json-c/json.h>

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
    window->used_percent = credits > 0.0 ? CLAMP((usage / credits) * 100.0, 0.0, 100.0) : 0.0;
    codexbar_provider_add_quota_window(provider, window);
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_openrouter_fetch(const CodexBarProviderConfig *config, GError **error) {
    const char *token = g_getenv("OPENROUTER_API_KEY");
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
    char *url = g_strdup_printf("%s%scredits", base, g_str_has_suffix(base, "/") ? "" : "/");
    g_free(base);
    char *bearer = g_strdup_printf("Bearer %s", token);
    CodexBarHttpResponse *response = codexbar_http_get(url, "Authorization", bearer, error);
    g_free(bearer);
    g_free(url);
    if (!response) {
        return NULL;
    }
    if (response->status != 200) {
        g_set_error(error, openrouter_error_quark(), 4, "OpenRouter API returned HTTP %ld", response->status);
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_openrouter_parse_credits(response->body, error);
    codexbar_http_response_free(response);
    return provider;
}
