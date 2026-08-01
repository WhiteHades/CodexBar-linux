#include "simple_providers.h"

#include "http.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define SIMPLE_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)

static gboolean json_whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

static json_object *parse_json_bytes(const char *json, size_t length) {
    if (!json || length == 0 || length > G_MAXINT || length > SIMPLE_MAXIMUM_RESPONSE_BYTES ||
        memchr(json, '\0', length) || !g_utf8_validate(json, (gssize)length, NULL)) {
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

typedef struct {
    char *currency;
    double paid;
    double granted;
} DeepSeekWalletTotal;

static void deepseek_wallet_total_free(gpointer data) {
    DeepSeekWalletTotal *total = data;
    g_free(total->currency);
    g_free(total);
}

static DeepSeekWalletTotal *deepseek_wallet_total(GPtrArray *totals, const char *currency) {
    for (guint index = 0; index < totals->len; index++) {
        DeepSeekWalletTotal *total = g_ptr_array_index(totals, index);
        if (g_str_equal(total->currency, currency)) return total;
    }
    DeepSeekWalletTotal *total = g_new0(DeepSeekWalletTotal, 1);
    total->currency = g_strdup(currency);
    g_ptr_array_add(totals, total);
    return total;
}

static gboolean deepseek_add_wallets(GPtrArray *totals,
                                     json_object *wallets,
                                     gboolean bonus,
                                     GError **error) {
    if (!wallets || !json_object_is_type(wallets, json_type_array)) {
        g_set_error_literal(error, provider_error_quark(), 1, "DeepSeek platform balance response is malformed");
        return FALSE;
    }
    size_t count = json_object_array_length(wallets);
    for (size_t index = 0; index < count; index++) {
        json_object *wallet = json_object_array_get_idx(wallets, index);
        json_object *currency_value = NULL;
        const char *currency = NULL;
        double balance = 0;
        if (!wallet || !json_object_is_type(wallet, json_type_object) ||
            !json_object_object_get_ex(wallet, "currency", &currency_value) ||
            !json_object_is_type(currency_value, json_type_string) ||
            !(currency = json_object_get_string(currency_value)) || currency[0] == '\0' ||
            !number_member(wallet, "balance", &balance) || balance < 0) {
            g_set_error_literal(error, provider_error_quark(), 1, "DeepSeek platform balance response is malformed");
            return FALSE;
        }
        DeepSeekWalletTotal *total = deepseek_wallet_total(totals, currency);
        if (bonus) total->granted += balance;
        else total->paid += balance;
        if (!isfinite(total->paid) || !isfinite(total->granted)) {
            g_set_error_literal(error, provider_error_quark(), 1, "DeepSeek platform balance response is malformed");
            return FALSE;
        }
    }
    return TRUE;
}

CodexBarProvider *codexbar_deepseek_parse_platform_balance(const char *json,
                                                           size_t length,
                                                           GError **error) {
    json_object *root = parse_json_bytes(json, length);
    json_object *data = NULL, *summary = NULL, *normal = NULL, *bonus = NULL;
    json_object *code = root ? json_object_object_get(root, "code") : NULL;
    json_object *biz_code = NULL;
    if (code && json_object_get_int(code) != 0) {
        int value = json_object_get_int(code);
        json_object_put(root);
        g_set_error(error,
                    provider_error_quark(),
                    value == 40002 || value == 40003 ? 6 : 1,
                    "DeepSeek platform user summary returned code %d",
                    value);
        return NULL;
    }
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "data", &data) || !json_object_is_type(data, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, provider_error_quark(), 1, "DeepSeek platform balance response is malformed");
        return NULL;
    }
    biz_code = json_object_object_get(data, "biz_code");
    if (biz_code && json_object_get_int(biz_code) != 0) {
        int value = json_object_get_int(biz_code);
        json_object_put(root);
        g_set_error(error,
                    provider_error_quark(),
                    value == 40002 || value == 40003 ? 6 : 1,
                    "DeepSeek platform user summary returned biz_code %d",
                    value);
        return NULL;
    }
    if (!json_object_object_get_ex(data, "biz_data", &summary) ||
        !json_object_is_type(summary, json_type_object) ||
        !json_object_object_get_ex(summary, "normal_wallets", &normal) ||
        !json_object_object_get_ex(summary, "bonus_wallets", &bonus)) {
        json_object_put(root);
        g_set_error_literal(error, provider_error_quark(), 1, "DeepSeek platform balance response is malformed");
        return NULL;
    }
    GPtrArray *totals = g_ptr_array_new_with_free_func(deepseek_wallet_total_free);
    if (!deepseek_add_wallets(totals, normal, FALSE, error) ||
        !deepseek_add_wallets(totals, bonus, TRUE, error)) {
        g_ptr_array_free(totals, TRUE);
        json_object_put(root);
        return NULL;
    }
    DeepSeekWalletTotal *selected = NULL;
    for (guint index = 0; index < totals->len; index++) {
        DeepSeekWalletTotal *candidate = g_ptr_array_index(totals, index);
        if (g_str_equal(candidate->currency, "USD") && candidate->paid + candidate->granted > 0) {
            selected = candidate;
            break;
        }
    }
    for (guint index = 0; !selected && index < totals->len; index++) {
        DeepSeekWalletTotal *candidate = g_ptr_array_index(totals, index);
        if (candidate->paid + candidate->granted > 0) selected = candidate;
    }
    for (guint index = 0; !selected && index < totals->len; index++) {
        DeepSeekWalletTotal *candidate = g_ptr_array_index(totals, index);
        if (g_str_equal(candidate->currency, "USD")) selected = candidate;
    }
    if (!selected && totals->len > 0) selected = g_ptr_array_index(totals, 0);
    const char *currency = selected ? selected->currency : "USD";
    double paid = selected ? selected->paid : 0;
    double granted = selected ? selected->granted : 0;
    double total = paid + granted;
    CodexBarProvider *provider = provider_new("deepseek");
    g_free(provider->source);
    provider->source = g_strdup("web");
    add_balance(provider, "credits", "credits", total, currency);
    CodexBarQuotaWindow *window = add_window(provider, "primary", "Balance");
    window->used_percent = total > 0 ? 0 : 100;
    const char *symbol = g_str_equal(currency, "CNY") ? "¥" : "$";
    window->detail = total > 0
                         ? g_strdup_printf("%s%.2f (Paid: %s%.2f / Granted: %s%.2f)",
                                           symbol, total, symbol, paid, symbol, granted)
                         : g_strdup_printf("%s0.00 — add credits at platform.deepseek.com", symbol);
    g_ptr_array_free(totals, TRUE);
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
    window->used_percent = limit > 0.0
                               ? codexbar_usage_percent_display(codexbar_usage_percent_from_ratio(used, limit))
                               : 0.0;
    window->detail = g_strdup_printf("%.0f / %.0f credits", used, limit);
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_crof_parse(const char *json, GError **error) {
    json_object *root = json_tokener_parse(json);
    double credits = 0.0;
    double plan = 0.0;
    double usable = 0.0;
    json_object *plan_value = NULL;
    json_object *usable_value = NULL;
    gboolean has_plan = root && json_object_object_get_ex(root, "requests_plan", &plan_value) &&
                        !json_object_is_type(plan_value, json_type_null);
    gboolean has_usable = root && json_object_object_get_ex(root, "usable_requests", &usable_value) &&
                          !json_object_is_type(usable_value, json_type_null);
    if (!root || !json_object_is_type(root, json_type_object) || !strict_number_member(root, "credits", &credits) ||
        (has_plan && !strict_number_member(root, "requests_plan", &plan)) ||
        (has_usable && !strict_number_member(root, "usable_requests", &usable))) {
        g_set_error_literal(error, provider_error_quark(), 7, "Crof usage response is malformed");
        if (root) json_object_put(root);
        return NULL;
    }

    double credit_floor = floor(MAX(0.0, credits) * 100.0) / 100.0;
    CodexBarProvider *provider = provider_new("crof");
    provider->plan = g_strdup("API key");
    if (has_plan && has_usable) {
        double displayed = MAX(0.0, usable);
        double clamped = CLAMP(displayed, 0.0, MAX(0.0, plan));
        int remaining_percent = plan > 0.0 ? (int)((clamped / plan) * 100.0) : 0;
        CodexBarQuotaWindow *requests_window = add_window(provider, "requests", "requests");
        requests_window->used_percent = 100.0 - remaining_percent;
        char *requests = amount(displayed);
        requests_window->detail = g_strdup_printf("%s requests left", requests);
        g_free(requests);
        requests_window->has_resets_at = TRUE;
        requests_window->resets_at_ms = next_chicago_midnight_ms();
    }
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
        window->used_percent = codexbar_usage_percent_display(
            codexbar_usage_percent_from_ratio(allocation - diem, allocation));
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
    window->used_percent = codexbar_usage_percent_display(codexbar_usage_percent_from_raw(usage * 100.0));
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

static char *deepseek_config_string(const CodexBarProviderConfig *config, const char *key) {
    json_object *value = NULL;
    if (!config || !config->raw || !json_object_object_get_ex(config->raw, key, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    return clean_token(json_object_get_string(value));
}

static char *deepseek_api_token(const CodexBarProviderConfig *config) {
    char *token = clean_token(config ? config->api_key : NULL);
    if (!token) token = clean_token(g_getenv("DEEPSEEK_API_KEY"));
    if (!token) token = clean_token(g_getenv("DEEPSEEK_KEY"));
    return token;
}

static char *deepseek_platform_token(const CodexBarProviderConfig *config) {
    char *token = deepseek_config_string(config, "platformToken");
    if (!token) token = deepseek_config_string(config, "userToken");
    if (!token) token = clean_token(g_getenv("DEEPSEEK_PLATFORM_TOKEN"));
    if (!token) token = clean_token(g_getenv("DEEPSEEK_USER_TOKEN"));
    return token;
}

static CodexBarProvider *deepseek_request(const char *url,
                                          const char *token,
                                          gboolean platform,
                                          CodexBarSimpleProviderTransport transport,
                                          GCancellable *cancellable,
                                          GError **error) {
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) return NULL;
    char *authorization = g_strdup_printf("Bearer %s", token);
    const CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 15,
        .maximum_response_bytes = SIMPLE_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = transport(&request, error);
    g_free(authorization);
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        codexbar_http_response_free(response);
        if (error && *error) g_clear_error(error);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        return NULL;
    }
    if (!response) {
        if (error && !*error) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "DeepSeek request failed");
        return NULL;
    }
    if (response->status != 200) {
        g_set_error(error,
                    G_IO_ERROR,
                    response->status == 401 || response->status == 403 ? G_IO_ERROR_PERMISSION_DENIED
                                                                       : G_IO_ERROR_FAILED,
                    "DeepSeek %s endpoint returned HTTP %ld",
                    platform ? "platform" : "balance",
                    response->status);
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = platform
                                     ? codexbar_deepseek_parse_platform_balance(
                                           response->body, response->body_length, error)
                                     : codexbar_deepseek_parse(response->body, error);
    codexbar_http_response_free(response);
    return provider;
}

CodexBarProvider *codexbar_deepseek_fetch_for_source_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    CodexBarSimpleProviderTransport transport,
    GCancellable *cancellable,
    GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    const char *mode = source && source[0] ? source : "auto";
    if (!g_str_equal(mode, "auto") && !g_str_equal(mode, "api") && !g_str_equal(mode, "web")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "DeepSeek source '%s' is unsupported", mode);
        return NULL;
    }
    char *api_token = deepseek_api_token(config);
    gboolean use_api = g_str_equal(mode, "api") || (g_str_equal(mode, "auto") && api_token);
    if (use_api) {
        if (!api_token) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Missing DeepSeek API key");
            return NULL;
        }
        CodexBarProvider *provider = deepseek_request(
            "https://api.deepseek.com/user/balance", api_token, FALSE, transport, cancellable, error);
        g_free(api_token);
        return provider;
    }
    g_free(api_token);
    char *platform_token = deepseek_platform_token(config);
    if (!platform_token) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "DeepSeek Platform session is missing; configure platformToken or DEEPSEEK_PLATFORM_TOKEN");
        return NULL;
    }
    CodexBarProvider *provider = deepseek_request(
        "https://platform.deepseek.com/api/v0/users/get_user_summary",
        platform_token,
        TRUE,
        transport,
        cancellable,
        error);
    g_free(platform_token);
    return provider;
}

CodexBarProvider *codexbar_deepseek_fetch_for_source_with_cancellable(
    const CodexBarProviderConfig *config,
    const char *source,
    GCancellable *cancellable,
    GError **error) {
    return codexbar_deepseek_fetch_for_source_with_transport_and_cancellable(
        config, source, codexbar_http_send, cancellable, error);
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
