#include "simple_providers.h"

#include "http.h"

#include <json-c/json.h>

static GQuark provider_error_quark(void) {
    return g_quark_from_static_string("codexbar-simple-provider-error");
}

static CodexBarProvider *provider_new(const char *id) {
    CodexBarProvider *provider = g_new0(CodexBarProvider, 1);
    provider->provider = g_strdup(id);
    provider->source = g_strdup("api");
    return provider;
}

static gboolean number_member(json_object *object, const char *key, double *result) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value)) {
        return FALSE;
    }
    if (json_object_is_type(value, json_type_string) || json_object_is_type(value, json_type_double) ||
        json_object_is_type(value, json_type_int)) {
        *result = json_object_get_double(value);
        return TRUE;
    }
    return FALSE;
}

CodexBarProvider *codexbar_deepseek_parse(const char *json, GError **error) {
    json_object *root = json_tokener_parse(json);
    json_object *infos = NULL;
    if (!root || !json_object_object_get_ex(root, "balance_infos", &infos) ||
        !json_object_is_type(infos, json_type_array) || json_object_array_length(infos) == 0) {
        g_set_error_literal(error, provider_error_quark(), 1, "DeepSeek balance response is malformed");
        if (root) json_object_put(root);
        return NULL;
    }
    json_object *first = json_object_array_get_idx(infos, 0);
    double balance = 0.0;
    if (!number_member(first, "total_balance", &balance)) {
        g_set_error_literal(error, provider_error_quark(), 1, "DeepSeek balance response is malformed");
        json_object_put(root);
        return NULL;
    }
    CodexBarProvider *provider = provider_new("deepseek");
    provider->has_credits = TRUE;
    provider->credits_remaining = MAX(0.0, balance);
    provider->primary.available = TRUE;
    provider->primary.used_percent = balance > 0.0 ? 0.0 : 100.0;
    provider->primary.reset_description = g_strdup_printf("balance %.2f", balance);
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_moonshot_parse(const char *json, GError **error) {
    json_object *root = json_tokener_parse(json);
    json_object *data = NULL;
    json_object *status = NULL;
    double balance = 0.0;
    if (!root || !json_object_object_get_ex(root, "status", &status) || !json_object_get_boolean(status) ||
        !json_object_object_get_ex(root, "data", &data) || !number_member(data, "available_balance", &balance)) {
        g_set_error_literal(error, provider_error_quark(), 2, "Moonshot balance response is malformed");
        if (root) json_object_put(root);
        return NULL;
    }
    CodexBarProvider *provider = provider_new("moonshot");
    provider->has_credits = TRUE;
    provider->credits_remaining = balance;
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_elevenlabs_parse(const char *json, GError **error) {
    json_object *root = json_tokener_parse(json);
    double used = 0.0;
    double limit = 0.0;
    if (!root || !number_member(root, "character_count", &used) ||
        !number_member(root, "character_limit", &limit)) {
        g_set_error_literal(error, provider_error_quark(), 3, "ElevenLabs subscription response is malformed");
        if (root) json_object_put(root);
        return NULL;
    }
    CodexBarProvider *provider = provider_new("elevenlabs");
    provider->primary.available = TRUE;
    provider->primary.used_percent = limit > 0.0 ? CLAMP((used / limit) * 100.0, 0.0, 100.0) : 0.0;
    provider->primary.reset_description = g_strdup_printf("%.0f / %.0f credits", used, limit);
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_simple_provider_fetch(const CodexBarProviderConfig *config, GError **error) {
    const char *token = config->api_key;
    const char *environment_key = NULL;
    const char *url = NULL;
    const char *header = "Authorization";
    if (g_str_equal(config->id, "deepseek")) {
        environment_key = "DEEPSEEK_API_KEY";
        url = "https://api.deepseek.com/user/balance";
    } else if (g_str_equal(config->id, "moonshot")) {
        environment_key = "MOONSHOT_API_KEY";
        url = config->region && g_str_equal(config->region, "china")
                  ? "https://api.moonshot.cn/v1/users/me/balance"
                  : "https://api.moonshot.ai/v1/users/me/balance";
    } else if (g_str_equal(config->id, "elevenlabs")) {
        environment_key = "ELEVENLABS_API_KEY";
        url = "https://api.elevenlabs.io/v1/user/subscription";
        header = "xi-api-key";
    } else {
        g_set_error_literal(error, provider_error_quark(), 4, "Unsupported simple provider");
        return NULL;
    }
    const char *environment_token = g_getenv(environment_key);
    if (environment_token && environment_token[0] != '\0') token = environment_token;
    if (!token || token[0] == '\0') {
        g_set_error(error, provider_error_quark(), 5, "%s API token is not configured", config->id);
        return NULL;
    }
    char *value = g_str_equal(header, "Authorization") ? g_strdup_printf("Bearer %s", token) : g_strdup(token);
    CodexBarHttpResponse *response = codexbar_http_get(url, header, value, error);
    g_free(value);
    if (!response) return NULL;
    if (response->status != 200) {
        g_set_error(error, provider_error_quark(), 6, "%s API returned HTTP %ld", config->id, response->status);
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = g_str_equal(config->id, "deepseek")
                                     ? codexbar_deepseek_parse(response->body, error)
                                 : g_str_equal(config->id, "moonshot")
                                     ? codexbar_moonshot_parse(response->body, error)
                                     : codexbar_elevenlabs_parse(response->body, error);
    codexbar_http_response_free(response);
    return provider;
}
