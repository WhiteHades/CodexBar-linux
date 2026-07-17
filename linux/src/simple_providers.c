#include "simple_providers.h"

#include "http.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>

static GQuark provider_error_quark(void) {
    return g_quark_from_static_string("codexbar-simple-provider-error");
}

static CodexBarProvider *provider_new(const char *id) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup(id);
    provider->source = g_strdup("api");
    return provider;
}

static CodexBarQuotaWindow *add_window(CodexBarProvider *provider, const char *id, const char *title) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    codexbar_provider_add_quota_window(provider, window);
    return window;
}

static void add_balance(CodexBarProvider *provider, const char *id, const char *title, double remaining,
                        const char *unit) {
    codexbar_provider_add_balance(provider, codexbar_balance_new(id, title, remaining, unit));
}

static gboolean number_member(json_object *object, const char *key, double *result) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value)) {
        return FALSE;
    }
    if (json_object_is_type(value, json_type_string) || json_object_is_type(value, json_type_double) ||
        json_object_is_type(value, json_type_int)) {
        if (json_object_is_type(value, json_type_string)) {
            char *clean = g_strdup(json_object_get_string(value));
            g_strstrip(clean);
            char *end = NULL;
            double parsed = g_ascii_strtod(clean, &end);
            gboolean valid = clean[0] != '\0' && end && *end == '\0' && isfinite(parsed);
            g_free(clean);
            if (!valid) return FALSE;
            *result = parsed;
            return TRUE;
        }
        double parsed = json_object_get_double(value);
        if (!isfinite(parsed)) return FALSE;
        *result = parsed;
        return TRUE;
    }
    return FALSE;
}

static gboolean strict_number_member(json_object *object, const char *key, double *result) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) ||
        (!json_object_is_type(value, json_type_double) && !json_object_is_type(value, json_type_int))) {
        return FALSE;
    }
    *result = json_object_get_double(value);
    return isfinite(*result);
}

static gboolean optional_number_member(json_object *object, const char *key, gboolean *present, double *result) {
    json_object *value = NULL;
    *present = FALSE;
    if (!json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) return TRUE;
    if (json_object_is_type(value, json_type_string)) {
        char *clean = g_strdup(json_object_get_string(value));
        g_strstrip(clean);
        gboolean empty = clean[0] == '\0';
        g_free(clean);
        if (empty) return TRUE;
    }
    if (!number_member(object, key, result)) return FALSE;
    *present = TRUE;
    return TRUE;
}

static const char *string_member(json_object *object, const char *key) {
    json_object *value = NULL;
    return json_object_object_get_ex(object, key, &value) && json_object_is_type(value, json_type_string)
               ? json_object_get_string(value)
               : NULL;
}

static gboolean iso_timestamp_ms(const char *raw, gint64 *result) {
    if (!raw) return FALSE;
    GDateTime *time = g_date_time_new_from_iso8601(raw, NULL);
    if (!time) return FALSE;
    *result = g_date_time_to_unix(time) * 1000 + g_date_time_get_microsecond(time) / 1000;
    g_date_time_unref(time);
    return TRUE;
}

static char *formatted_plan_expiry(const char *raw) {
    if (!raw) return NULL;
    GDateTime *time = g_date_time_new_from_iso8601(raw, NULL);
    if (!time) return NULL;
    GDateTime *local = g_date_time_to_local(time);
    char *result = g_date_time_format(local, "plan expires %b %d, %Y");
    g_date_time_unref(local);
    g_date_time_unref(time);
    return result;
}

static gint64 next_chicago_midnight_ms(void) {
    GTimeZone *zone = g_time_zone_new_identifier("America/Chicago");
    if (!zone) return 0;
    GDateTime *now = g_date_time_new_now(zone);
    GDateTime *midnight = g_date_time_new(zone,
                                          g_date_time_get_year(now),
                                          g_date_time_get_month(now),
                                          g_date_time_get_day_of_month(now),
                                          0,
                                          0,
                                          0);
    GDateTime *reset = g_date_time_add_days(midnight, 1);
    gint64 result = g_date_time_to_unix(reset) * 1000;
    g_date_time_unref(reset);
    g_date_time_unref(midnight);
    g_date_time_unref(now);
    g_time_zone_unref(zone);
    return result;
}

static char *amount(double value) {
    double integral = 0.0;
    return modf(value, &integral) == 0.0 ? g_strdup_printf("%.0f", value) : g_strdup_printf("%.2f", value);
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
    add_balance(provider, "credits", "credits", MAX(0.0, balance), "credits");
    CodexBarQuotaWindow *window = add_window(provider, "primary", "session");
    window->used_percent = balance > 0.0 ? 0.0 : 100.0;
    window->detail = g_strdup_printf("balance %.2f", balance);
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
    add_balance(provider, "credits", "credits", balance, "credits");
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
    CodexBarQuotaWindow *window = add_window(provider, "primary", "session");
    window->used_percent = limit > 0.0 ? CLAMP((used / limit) * 100.0, 0.0, 100.0) : 0.0;
    window->detail = g_strdup_printf("%.0f / %.0f credits", used, limit);
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_crof_parse(const char *json, GError **error) {
    json_object *root = json_tokener_parse(json);
    double credits = 0.0;
    double plan = 0.0;
    double usable = 0.0;
    if (!root || !json_object_is_type(root, json_type_object) || !strict_number_member(root, "credits", &credits) ||
        !strict_number_member(root, "requests_plan", &plan) ||
        !strict_number_member(root, "usable_requests", &usable)) {
        g_set_error_literal(error, provider_error_quark(), 7, "Crof usage response is malformed");
        if (root) json_object_put(root);
        return NULL;
    }

    double displayed = MAX(0.0, usable);
    double clamped = CLAMP(displayed, 0.0, MAX(0.0, plan));
    int remaining_percent = plan > 0.0 ? (int)((clamped / plan) * 100.0) : 0;
    double credit_floor = floor(MAX(0.0, credits) * 100.0) / 100.0;
    CodexBarProvider *provider = provider_new("crof");
    provider->plan = g_strdup("API key");
    CodexBarQuotaWindow *requests_window = add_window(provider, "requests", "requests");
    requests_window->used_percent = 100.0 - remaining_percent;
    char *requests = amount(displayed);
    requests_window->detail = g_strdup_printf("%s requests left", requests);
    g_free(requests);
    requests_window->has_resets_at = TRUE;
    requests_window->resets_at_ms = next_chicago_midnight_ms();
    CodexBarQuotaWindow *balance_window = add_window(provider, "balance", "balance");
    balance_window->used_percent = credits > 0.0 ? 0.0 : 100.0;
    balance_window->detail = g_strdup_printf("$%.2f", credit_floor);
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_venice_parse(const char *json, GError **error) {
    json_object *root = json_tokener_parse(json);
    json_object *can_consume_value = NULL;
    json_object *balances = NULL;
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "canConsume", &can_consume_value) ||
        !json_object_is_type(can_consume_value, json_type_boolean) ||
        !json_object_object_get_ex(root, "balances", &balances) || !json_object_is_type(balances, json_type_object)) {
        g_set_error_literal(error, provider_error_quark(), 8, "Venice balance response is malformed");
        if (root) json_object_put(root);
        return NULL;
    }

    gboolean can_consume = json_object_get_boolean(can_consume_value);
    json_object *currency_value = NULL;
    const char *currency = NULL;
    if (json_object_object_get_ex(root, "consumptionCurrency", &currency_value) &&
        !json_object_is_type(currency_value, json_type_null)) {
        if (!json_object_is_type(currency_value, json_type_string)) {
            g_set_error_literal(error, provider_error_quark(), 8, "Venice balance response is malformed");
            json_object_put(root);
            return NULL;
        }
        currency = json_object_get_string(currency_value);
    }
    double diem = 0.0;
    double usd = 0.0;
    double allocation = 0.0;
    gboolean has_diem = FALSE;
    gboolean has_usd = FALSE;
    gboolean has_allocation = FALSE;
    if (!optional_number_member(balances, "diem", &has_diem, &diem) ||
        !optional_number_member(balances, "usd", &has_usd, &usd) ||
        !optional_number_member(root, "diemEpochAllocation", &has_allocation, &allocation)) {
        g_set_error_literal(error, provider_error_quark(), 8, "Venice balance response is malformed");
        json_object_put(root);
        return NULL;
    }
    gboolean uses_usd = currency && g_ascii_strcasecmp(currency, "USD") == 0;

    CodexBarProvider *provider = provider_new("venice");
    CodexBarQuotaWindow *window = add_window(provider, "balance", "balance");
    if (!can_consume) {
        window->used_percent = 100.0;
        window->detail = g_strdup("Balance unavailable for API calls");
    } else if (uses_usd && has_usd && usd > 0.0) {
        window->detail = g_strdup_printf("$%.2f USD remaining", usd);
    } else if (!uses_usd && has_diem && has_allocation && allocation > 0.0) {
        window->used_percent = CLAMP(((allocation - diem) / allocation) * 100.0, 0.0, 100.0);
        window->detail =
            g_strdup_printf("DIEM %.2f / %.2f epoch allocation", diem, allocation);
    } else if (has_diem && diem > 0.0) {
        window->detail = g_strdup_printf("DIEM %.2f remaining", diem);
    } else if (has_usd && usd > 0.0) {
        window->detail = g_strdup_printf("$%.2f USD remaining", usd);
    } else {
        window->used_percent = 100.0;
        window->detail = g_strdup("No Venice API balance available");
    }
    json_object_put(root);
    return provider;
}

static gboolean parse_zenmux_window(json_object *data, const char *key, const char *label,
                                    CodexBarProvider *provider) {
    json_object *quota = NULL;
    double usage = 0.0;
    double maximum = 0.0;
    double used = 0.0;
    double remaining = 0.0;
    if (!json_object_object_get_ex(data, key, &quota) || !json_object_is_type(quota, json_type_object) ||
        !strict_number_member(quota, "usage_percentage", &usage) ||
        !strict_number_member(quota, "max_flows", &maximum) || !strict_number_member(quota, "used_flows", &used) ||
        !strict_number_member(quota, "remaining_flows", &remaining)) {
        return FALSE;
    }
    CodexBarQuotaWindow *window = add_window(provider, key, label);
    window->used_percent = CLAMP(usage * 100.0, 0.0, 100.0);
    char *used_text = amount(used);
    char *maximum_text = amount(maximum);
    window->detail = g_strdup_printf("%s / %s flows", used_text, maximum_text);
    g_free(used_text);
    g_free(maximum_text);
    window->has_resets_at = iso_timestamp_ms(string_member(quota, "resets_at"), &window->resets_at_ms);
    return TRUE;
}

CodexBarProvider *codexbar_zenmux_parse_subscription(const char *json, GError **error) {
    json_object *root = json_tokener_parse(json);
    json_object *success = NULL;
    json_object *data = NULL;
    json_object *plan = NULL;
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "success", &success) || !json_object_is_type(success, json_type_boolean) ||
        !json_object_get_boolean(success) ||
        !json_object_object_get_ex(root, "data", &data) || !json_object_is_type(data, json_type_object) ||
        !json_object_object_get_ex(data, "plan", &plan) || !json_object_is_type(plan, json_type_object)) {
        g_set_error_literal(error, provider_error_quark(), 9, "ZenMux subscription response is malformed");
        if (root) json_object_put(root);
        return NULL;
    }
    const char *raw_tier = string_member(plan, "tier");
    const char *raw_status = string_member(data, "account_status");
    if (!raw_tier || !raw_status) {
        json_object_put(root);
        g_set_error_literal(error, provider_error_quark(), 9, "ZenMux subscription response is malformed");
        return NULL;
    }
    char *tier = g_strdup(raw_tier);
    char *status = g_strdup(raw_status);
    g_strstrip(tier);
    g_strstrip(status);
    CodexBarProvider *provider = provider_new("zenmux");
    if (tier && tier[0] != '\0') {
        char *capitalized = g_strdup(tier);
        capitalized[0] = (char)g_ascii_toupper(capitalized[0]);
        provider->plan = g_strdup_printf("%s plan", capitalized);
        g_free(capitalized);
        if (status && status[0] != '\0' && g_ascii_strcasecmp(status, "healthy") != 0) {
            char *capitalized_status = g_strdup(status);
            capitalized_status[0] = (char)g_ascii_toupper(capitalized_status[0]);
            char *with_status = g_strdup_printf("%s · %s", provider->plan, capitalized_status);
            g_free(capitalized_status);
            g_free(provider->plan);
            provider->plan = with_status;
        }
    }
    provider->note = formatted_plan_expiry(string_member(plan, "expires_at"));
    if (!parse_zenmux_window(data, "quota_5_hour", "5-hour", provider) ||
        !parse_zenmux_window(data, "quota_7_day", "weekly", provider)) {
        g_free(tier);
        g_free(status);
        codexbar_provider_free(provider);
        json_object_put(root);
        g_set_error_literal(error, provider_error_quark(), 9, "ZenMux subscription response is malformed");
        return NULL;
    }
    g_free(tier);
    g_free(status);
    json_object_put(root);
    return provider;
}

gboolean codexbar_zenmux_apply_payg(CodexBarProvider *provider, const char *json, GError **error) {
    json_object *root = json_tokener_parse(json);
    json_object *success = NULL;
    json_object *data = NULL;
    double balance = 0.0;
    if (!root || !json_object_object_get_ex(root, "success", &success) ||
        !json_object_is_type(success, json_type_boolean) || !json_object_get_boolean(success) ||
        !json_object_object_get_ex(root, "data", &data) || !json_object_is_type(data, json_type_object) ||
        !string_member(data, "currency") || g_ascii_strcasecmp(string_member(data, "currency"), "usd") != 0 ||
        !strict_number_member(data, "total_credits", &balance)) {
        g_set_error_literal(error, provider_error_quark(), 10, "ZenMux PAYG response is malformed");
        if (root) json_object_put(root);
        return FALSE;
    }
    CodexBarBalance *payg = NULL;
    for (guint index = 0; index < provider->balances->len; index++) {
        CodexBarBalance *candidate = codexbar_provider_balance(provider, index);
        if (g_str_equal(candidate->id, "payg")) {
            payg = candidate;
            break;
        }
    }
    if (payg) {
        payg->remaining = balance;
    } else {
        add_balance(provider, "payg", "pay as you go", balance, "USD");
    }
    json_object_put(root);
    return TRUE;
}

static char *clean_token(const char *raw) {
    if (!raw) return NULL;
    char *clean = g_strdup(raw);
    g_strstrip(clean);
    size_t length = strlen(clean);
    if (length >= 2 && ((clean[0] == '\'' && clean[length - 1] == '\'') ||
                        (clean[0] == '"' && clean[length - 1] == '"'))) {
        clean[length - 1] = '\0';
        memmove(clean, clean + 1, length - 1);
        g_strstrip(clean);
    }
    if (clean[0] != '\0') return clean;
    g_free(clean);
    return NULL;
}

static char *resolved_token(const char *primary, const char *secondary, const char *fallback) {
    char *value = clean_token(g_getenv(primary));
    if (!value && secondary) value = clean_token(g_getenv(secondary));
    if (!value) value = clean_token(fallback);
    return value;
}

static CodexBarProvider *fetch_zenmux(const CodexBarProviderConfig *config, const char *token, GError **error) {
    char *header_value = g_strdup_printf("Bearer %s", token);
    CodexBarHttpResponse *response = codexbar_http_get(
        "https://zenmux.ai/api/v1/management/subscription/detail", "Authorization", header_value, error);
    if (!response) {
        g_free(header_value);
        return NULL;
    }
    if (response->status == 401 || response->status == 403) {
        g_set_error_literal(error,
                            provider_error_quark(),
                            11,
                            "ZenMux rejected the Management API key; inference API keys are not supported");
        codexbar_http_response_free(response);
        g_free(header_value);
        return NULL;
    }
    if (response->status < 200 || response->status >= 300) {
        g_set_error(error, provider_error_quark(), 11, "%s API returned HTTP %ld", config->id, response->status);
        codexbar_http_response_free(response);
        g_free(header_value);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_zenmux_parse_subscription(response->body, error);
    codexbar_http_response_free(response);
    if (!provider) {
        g_free(header_value);
        return NULL;
    }

    GError *balance_error = NULL;
    response = codexbar_http_get(
        "https://zenmux.ai/api/v1/management/payg/balance", "Authorization", header_value, &balance_error);
    g_free(header_value);
    if (response && (response->status == 401 || response->status == 403)) {
        g_set_error_literal(error,
                            provider_error_quark(),
                            11,
                            "ZenMux rejected the Management API key; inference API keys are not supported");
        codexbar_http_response_free(response);
        codexbar_provider_free(provider);
        g_clear_error(&balance_error);
        return NULL;
    }
    if (response && response->status >= 200 && response->status < 300) {
        codexbar_zenmux_apply_payg(provider, response->body, &balance_error);
    }
    if (response) codexbar_http_response_free(response);
    g_clear_error(&balance_error);
    return provider;
}

CodexBarProvider *codexbar_simple_provider_fetch(const CodexBarProviderConfig *config, GError **error) {
    const char *token = config->api_key;
    char *owned_token = NULL;
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
    } else if (g_str_equal(config->id, "crof")) {
        owned_token = resolved_token("CROF_API_KEY", "CROFAI_API_KEY", token);
        token = owned_token;
        url = "https://crof.ai/usage_api/";
    } else if (g_str_equal(config->id, "venice")) {
        owned_token = resolved_token("VENICE_API_KEY", "VENICE_KEY", token);
        token = owned_token;
        url = "https://api.venice.ai/api/v1/billing/balance";
    } else if (g_str_equal(config->id, "zenmux")) {
        owned_token = resolved_token("ZENMUX_MANAGEMENT_API_KEY", NULL, token);
        token = owned_token;
        if (!token || token[0] == '\0') {
            g_set_error(error, provider_error_quark(), 5, "%s API token is not configured", config->id);
            g_free(owned_token);
            return NULL;
        }
        CodexBarProvider *provider = fetch_zenmux(config, token, error);
        g_free(owned_token);
        return provider;
    } else {
        g_set_error_literal(error, provider_error_quark(), 4, "Unsupported simple provider");
        return NULL;
    }
    if (environment_key) {
        owned_token = resolved_token(environment_key, NULL, token);
        token = owned_token;
    }
    if (!token || token[0] == '\0') {
        g_set_error(error, provider_error_quark(), 5, "%s API token is not configured", config->id);
        g_free(owned_token);
        return NULL;
    }
    char *value = g_str_equal(header, "Authorization") ? g_strdup_printf("Bearer %s", token) : g_strdup(token);
    CodexBarHttpResponse *response = codexbar_http_get(url, header, value, error);
    g_free(value);
    g_free(owned_token);
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
                                  : g_str_equal(config->id, "elevenlabs")
                                      ? codexbar_elevenlabs_parse(response->body, error)
                                  : g_str_equal(config->id, "crof") ? codexbar_crof_parse(response->body, error)
                                                                     : codexbar_venice_parse(response->body, error);
    codexbar_http_response_free(response);
    return provider;
}
