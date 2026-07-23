#include "neuralwatt.h"

#include <curl/curl.h>
#include <errno.h>
#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define NEURALWATT_DEFAULT_URL "https://api.neuralwatt.com"
#define NEURALWATT_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)

typedef struct {
    gboolean has_remaining;
    double remaining;
    gboolean has_total;
    double total;
    gboolean has_used;
    double used;
    char *accounting_method;
} NeuralWattBalance;

typedef struct {
    char *plan;
    gboolean has_start;
    gint64 start_ms;
    gboolean has_end;
    gint64 end_ms;
    gboolean has_auto_renew;
    gboolean auto_renew;
    gboolean has_included;
    double included;
    gboolean has_used;
    double used;
    gboolean has_remaining;
    double remaining;
} NeuralWattSubscription;

typedef struct {
    gboolean has_limit;
    double limit;
    char *period;
    gboolean has_spent;
    double spent;
    gboolean has_blocked;
    gboolean blocked;
} NeuralWattAllowance;

static void strip_unicode_whitespace(char *value) {
    char *start = value;
    while (*start) {
        gunichar character = g_utf8_get_char(start);
        if (!g_unichar_isspace(character)) break;
        start = g_utf8_next_char(start);
    }
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

static char *clean_value(const char *raw, gboolean reject_controls) {
    if (!raw || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *clean = g_strdup(raw);
    strip_unicode_whitespace(clean);
    size_t length = strlen(clean);
    if (length >= 1 && ((clean[0] == '\'' && clean[length - 1] == '\'') ||
                        (clean[0] == '"' && clean[length - 1] == '"'))) {
        clean[length - 1] = '\0';
        memmove(clean, clean + 1, length - 1);
        strip_unicode_whitespace(clean);
    }
    if (reject_controls) {
        for (const char *cursor = clean; *cursor; cursor = g_utf8_next_char(cursor)) {
            if (g_unichar_iscntrl(g_utf8_get_char(cursor))) {
                g_free(clean);
                return NULL;
            }
        }
    }
    if (clean[0] != '\0') return clean;
    g_free(clean);
    return NULL;
}

static char *resolve_api_key(const CodexBarProviderConfig *config) {
    char *key = clean_value(config ? config->api_key : NULL, TRUE);
    if (!key) key = clean_value(g_getenv("NEURALWATT_API_KEY"), TRUE);
    return key;
}

gboolean codexbar_neuralwatt_has_api_key(const CodexBarProviderConfig *config) {
    char *key = resolve_api_key(config);
    gboolean present = key != NULL;
    g_free(key);
    return present;
}

static char *append_quota_path(const char *normalized, GError **error) {
    GError *parse_error = NULL;
    GUri *uri = g_uri_parse(normalized, G_URI_FLAGS_NONE, &parse_error);
    if (!uri) {
        g_clear_error(&parse_error);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "Neuralwatt endpoint override NEURALWATT_API_URL must use HTTPS or a bare host.");
        return NULL;
    }
    const char *host = g_uri_get_host(uri);
    for (const char *cursor = host; cursor && *cursor; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        if (character == '%' || character == '\\' || g_unichar_isspace(character) ||
            g_unichar_iscntrl(character)) {
            g_uri_unref(uri);
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "Neuralwatt endpoint override NEURALWATT_API_URL must use HTTPS or a bare host.");
            return NULL;
        }
    }
    const char *path = g_uri_get_path(uri);
    char *base_path = g_strdup(path && path[0] != '\0' ? path : "");
    while (strlen(base_path) > 1 && g_str_has_suffix(base_path, "/")) base_path[strlen(base_path) - 1] = '\0';
    const char *last_slash = strrchr(base_path, '/');
    const char *last_component = last_slash ? last_slash + 1 : base_path;
    char *quota_path = g_str_equal(last_component, "v1")
                           ? g_strdup_printf("%s/quota", base_path)
                           : g_str_equal(base_path, "/") ? g_strdup("/v1/quota")
                                                          : g_strdup_printf("%s/v1/quota", base_path);
    if (quota_path[0] == '\0' || quota_path[0] != '/') {
        char *absolute = g_strdup_printf("/%s", quota_path);
        g_free(quota_path);
        quota_path = absolute;
    }
    GUri *quota = g_uri_build(G_URI_FLAGS_NONE,
                              g_uri_get_scheme(uri),
                              g_uri_get_userinfo(uri),
                              g_uri_get_host(uri),
                              g_uri_get_port(uri),
                              quota_path,
                              g_uri_get_query(uri),
                              g_uri_get_fragment(uri));
    char *result = g_uri_to_string(quota);
    g_uri_unref(quota);
    g_uri_unref(uri);
    g_free(base_path);
    g_free(quota_path);
    return result;
}

char *codexbar_neuralwatt_quota_url_for_testing(const char *override, GError **error) {
    if (override && !g_utf8_validate(override, -1, NULL)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "Neuralwatt endpoint override NEURALWATT_API_URL must use HTTPS or a bare host.");
        return NULL;
    }
    char *clean = clean_value(override, FALSE);
    for (const char *cursor = clean; cursor && *cursor; cursor = g_utf8_next_char(cursor)) {
        if (g_unichar_iscntrl(g_utf8_get_char(cursor))) {
            g_free(clean);
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_INVALID_ARGUMENT,
                                "Neuralwatt endpoint override NEURALWATT_API_URL must use HTTPS or a bare host.");
            return NULL;
        }
    }
    const char *base = clean ? clean : NEURALWATT_DEFAULT_URL;
    GError *normalize_error = NULL;
    char *normalized = codexbar_http_normalize_endpoint(base, CODEXBAR_HTTP_HTTPS_ONLY, &normalize_error);
    g_free(clean);
    if (!normalized) {
        g_clear_error(&normalize_error);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "Neuralwatt endpoint override NEURALWATT_API_URL must use HTTPS or a bare host.");
        return NULL;
    }
    char *url = append_quota_path(normalized, error);
    g_free(normalized);
    return url;
}

static json_object *parse_json_document(const char *json, size_t length) {
    if (!json || length > G_MAXINT || !g_utf8_validate(json, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, json, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length &&
           (json[consumed] == ' ' || json[consumed] == '\t' || json[consumed] == '\n' || json[consumed] == '\r')) {
        consumed++;
    }
    gboolean valid = parse_error == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static gboolean optional_object(json_object *object, const char *key, gboolean *present, json_object **value) {
    json_object *member = NULL;
    if (!json_object_object_get_ex(object, key, &member) || json_object_is_type(member, json_type_null)) {
        *present = FALSE;
        if (value) *value = NULL;
        return TRUE;
    }
    if (!json_object_is_type(member, json_type_object)) return FALSE;
    *present = TRUE;
    if (value) *value = member;
    return TRUE;
}

static gboolean optional_number(json_object *object, const char *key, gboolean *present, double *value) {
    json_object *member = NULL;
    if (!json_object_object_get_ex(object, key, &member) || json_object_is_type(member, json_type_null)) {
        *present = FALSE;
        return TRUE;
    }
    if (!json_object_is_type(member, json_type_int) && !json_object_is_type(member, json_type_double)) return FALSE;
    double number = json_object_get_double(member);
    if (!isfinite(number)) return FALSE;
    *present = TRUE;
    if (value) *value = number;
    return TRUE;
}

static gboolean optional_integer(json_object *object, const char *key) {
    json_object *member = NULL;
    if (!json_object_object_get_ex(object, key, &member) || json_object_is_type(member, json_type_null)) return TRUE;
    if (!json_object_is_type(member, json_type_int)) return FALSE;
    const char *encoded = json_object_get_string(member);
    return encoded[0] == '-' || json_object_get_uint64(member) <= G_MAXINT64;
}

static gboolean optional_boolean(json_object *object, const char *key, gboolean *present, gboolean *value) {
    json_object *member = NULL;
    if (!json_object_object_get_ex(object, key, &member) || json_object_is_type(member, json_type_null)) {
        *present = FALSE;
        return TRUE;
    }
    if (!json_object_is_type(member, json_type_boolean)) return FALSE;
    *present = TRUE;
    if (value) *value = json_object_get_boolean(member);
    return TRUE;
}

static gboolean optional_string(json_object *object, const char *key, gboolean *present, char **value) {
    json_object *member = NULL;
    if (!json_object_object_get_ex(object, key, &member) || json_object_is_type(member, json_type_null)) {
        *present = FALSE;
        if (value) *value = NULL;
        return TRUE;
    }
    if (!json_object_is_type(member, json_type_string) ||
        memchr(json_object_get_string(member), '\0', (size_t)json_object_get_string_len(member))) {
        return FALSE;
    }
    *present = TRUE;
    if (value) *value = g_strdup(json_object_get_string(member));
    return TRUE;
}

static gboolean optional_date(json_object *object, const char *key, gboolean *present, gint64 *milliseconds) {
    char *text = NULL;
    if (!optional_string(object, key, present, &text)) return FALSE;
    if (!*present) return TRUE;
    gboolean syntax_valid = g_regex_match_simple(
        "^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-5][0-9](\\.[0-9]+)?(Z|[+-][0-9]{2}:[0-9]{2})$",
        text,
        G_REGEX_DEFAULT,
        G_REGEX_MATCH_DEFAULT);
    if (!syntax_valid) {
        g_free(text);
        return FALSE;
    }
    GDateTime *date = g_date_time_new_from_iso8601(text, NULL);
    g_free(text);
    if (!date) return FALSE;
    *milliseconds = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static gboolean validate_period(json_object *period) {
    gboolean present = FALSE;
    double number = 0.0;
    return optional_number(period, "cost_usd", &present, &number) && optional_integer(period, "requests") &&
           optional_integer(period, "tokens") && optional_number(period, "energy_kwh", &present, &number);
}

static gboolean validate_usage(json_object *root) {
    gboolean present = FALSE;
    json_object *usage = NULL;
    if (!optional_object(root, "usage", &present, &usage)) return FALSE;
    if (!present) return TRUE;
    const char *keys[] = {"lifetime", "current_month"};
    for (guint index = 0; index < G_N_ELEMENTS(keys); index++) {
        json_object *period = NULL;
        if (!optional_object(usage, keys[index], &present, &period) || (present && !validate_period(period))) return FALSE;
    }
    return TRUE;
}

static gboolean parse_balance(json_object *root, NeuralWattBalance *balance) {
    gboolean present = FALSE;
    json_object *object = NULL;
    if (!optional_object(root, "balance", &present, &object) || !present) return FALSE;
    if (!optional_number(object, "credits_remaining_usd", &balance->has_remaining, &balance->remaining) ||
        !optional_number(object, "total_credits_usd", &balance->has_total, &balance->total) ||
        !optional_number(object, "credits_used_usd", &balance->has_used, &balance->used) ||
        !optional_string(object, "accounting_method", &present, &balance->accounting_method)) {
        return FALSE;
    }
    return (balance->has_remaining && balance->remaining >= 0.0) ||
           (balance->has_used && balance->used >= 0.0) || (balance->has_total && balance->total > 0.0);
}

static gboolean validate_limits(json_object *root) {
    gboolean present = FALSE;
    double number = 0.0;
    char *text = NULL;
    json_object *limits = NULL;
    if (!optional_object(root, "limits", &present, &limits)) return FALSE;
    if (!present) return TRUE;
    gboolean valid = optional_number(limits, "overage_limit_usd", &present, &number) &&
                     optional_string(limits, "rate_limit_tier", &present, &text);
    g_free(text);
    return valid;
}

static gboolean parse_subscription(json_object *root, gboolean *present, NeuralWattSubscription *subscription) {
    json_object *object = NULL;
    if (!optional_object(root, "subscription", present, &object)) return FALSE;
    if (!*present) return TRUE;
    char *discard = NULL;
    gboolean field = FALSE;
    gboolean boolean = FALSE;
    gboolean valid = optional_string(object, "plan", &field, &subscription->plan) &&
                     optional_string(object, "status", &field, &discard);
    g_free(discard);
    discard = NULL;
    valid = valid && optional_string(object, "billing_interval", &field, &discard);
    g_free(discard);
    valid = valid && optional_date(object, "current_period_start", &subscription->has_start, &subscription->start_ms) &&
            optional_date(object, "current_period_end", &subscription->has_end, &subscription->end_ms) &&
            optional_boolean(object, "auto_renew", &subscription->has_auto_renew, &subscription->auto_renew) &&
            optional_number(object, "kwh_included", &subscription->has_included, &subscription->included) &&
            optional_number(object, "kwh_used", &subscription->has_used, &subscription->used) &&
            optional_number(object, "kwh_remaining", &subscription->has_remaining, &subscription->remaining) &&
            optional_boolean(object, "in_overage", &field, &boolean);
    return valid;
}

static gboolean parse_key(json_object *root, gboolean *has_allowance, NeuralWattAllowance *allowance) {
    gboolean present = FALSE;
    json_object *key = NULL;
    if (!optional_object(root, "key", &present, &key)) return FALSE;
    if (!present) return TRUE;
    char *name = NULL;
    if (!optional_string(key, "name", &present, &name)) return FALSE;
    g_free(name);
    json_object *object = NULL;
    if (!optional_object(key, "allowance", has_allowance, &object)) return FALSE;
    if (!*has_allowance) return TRUE;
    gboolean field = FALSE;
    double remaining = 0.0;
    return optional_number(object, "limit_usd", &allowance->has_limit, &allowance->limit) &&
           optional_string(object, "period", &field, &allowance->period) &&
           optional_number(object, "spent_usd", &allowance->has_spent, &allowance->spent) &&
           optional_number(object, "remaining_usd", &field, &remaining) &&
           optional_boolean(object, "blocked", &allowance->has_blocked, &allowance->blocked);
}

static void parsed_clear(NeuralWattBalance *balance,
                         NeuralWattSubscription *subscription,
                         NeuralWattAllowance *allowance) {
    g_free(balance->accounting_method);
    g_free(subscription->plan);
    g_free(allowance->period);
}

static char *capitalized_words(const char *raw, gboolean trim, gboolean replace_underscores) {
    if (!raw || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *clean = g_strdup(raw);
    if (trim) strip_unicode_whitespace(clean);
    if (replace_underscores) {
        for (char *cursor = clean; *cursor; cursor++) {
            if (*cursor == '_') *cursor = ' ';
        }
    }
    if (clean[0] == '\0') {
        g_free(clean);
        return NULL;
    }
    char *lower = g_utf8_strdown(clean, -1);
    g_free(clean);
    GString *result = g_string_new(NULL);
    gboolean word_start = TRUE;
    for (const char *cursor = lower; *cursor;) {
        gunichar character = g_utf8_get_char(cursor);
        char encoded[6] = {0};
        if (word_start && !g_unichar_isspace(character)) character = g_unichar_toupper(character);
        gint bytes = g_unichar_to_utf8(character, encoded);
        g_string_append_len(result, encoded, bytes);
        word_start = g_unichar_isspace(character);
        cursor = g_utf8_next_char(cursor);
    }
    g_free(lower);
    return g_string_free(result, FALSE);
}

static char *format_kwh(double value) {
    char buffer[512];
    g_ascii_formatd(buffer, sizeof(buffer), value == trunc(value) ? "%.0f" : "%.2f", value);
    return g_strdup(buffer);
}

CodexBarProvider *codexbar_neuralwatt_parse_usage_bytes(
    const char *json, size_t length, gint64 now_ms, GError **error) {
    json_object *root = parse_json_document(json, length);
    NeuralWattBalance balance = {0};
    NeuralWattSubscription subscription = {0};
    NeuralWattAllowance allowance = {0};
    gboolean has_subscription = FALSE;
    gboolean has_allowance = FALSE;
    gboolean snapshot_present = FALSE;
    char *snapshot = NULL;
    gboolean valid = root && json_object_is_type(root, json_type_object) &&
                     optional_string(root, "snapshot_at", &snapshot_present, &snapshot) &&
                     parse_balance(root, &balance) && validate_usage(root) && validate_limits(root) &&
                     parse_subscription(root, &has_subscription, &subscription) &&
                     parse_key(root, &has_allowance, &allowance);
    g_free(snapshot);
    if (!valid) {
        parsed_clear(&balance, &subscription, &allowance);
        if (root) json_object_put(root);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "Failed to parse Neuralwatt response: malformed or missing balance data");
        return NULL;
    }

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("neuralwatt");
    provider->source = g_strdup("api");
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));

    gboolean has_total = balance.has_total && balance.total > 0.0;
    double total = balance.total;
    if (!has_total && balance.has_remaining && balance.remaining >= 0.0 && balance.has_used && balance.used >= 0.0) {
        total = balance.remaining + balance.used;
        has_total = isfinite(total) && total > 0.0;
    }
    gboolean has_used = balance.has_used && balance.used >= 0.0;
    double used = balance.used;
    if (!has_used && has_total && balance.has_remaining && balance.remaining >= 0.0) {
        used = MAX(0.0, total - balance.remaining);
        has_used = TRUE;
    }
    gboolean has_remaining = balance.has_remaining && balance.remaining >= 0.0;
    double remaining = balance.remaining;
    if (!has_remaining && has_total && has_used) {
        remaining = MAX(0.0, total - used);
        has_remaining = TRUE;
    }
    if (has_remaining) {
        provider->provider_cost = g_new0(CodexBarProviderCost, 1);
        provider->provider_cost->used = remaining;
        provider->provider_cost->currency = g_strdup("USD");
        provider->provider_cost->period = g_strdup("Neuralwatt prepaid balance");
        provider->provider_cost->has_updated_at = TRUE;
        provider->provider_cost->updated_at_ms = now_ms;
    }

    gboolean has_subscription_total = FALSE;
    double subscription_total = 0.0;
    if (has_subscription && subscription.has_included && subscription.included > 0.0) {
        subscription_total = subscription.included;
        has_subscription_total = TRUE;
    } else if (has_subscription && subscription.has_used && subscription.used >= 0.0 &&
               subscription.has_remaining && subscription.remaining >= 0.0) {
        subscription_total = subscription.used + subscription.remaining;
        has_subscription_total = isfinite(subscription_total) && subscription_total > 0.0;
    }
    gboolean has_subscription_used = has_subscription && subscription.has_used && subscription.used >= 0.0;
    double subscription_used = subscription.used;
    if (!has_subscription_used && has_subscription_total && subscription.has_remaining &&
        subscription.remaining >= 0.0) {
        subscription_used = MAX(0.0, subscription_total - subscription.remaining);
        has_subscription_used = TRUE;
    }
    if (has_subscription_total && has_subscription_used) {
        CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "Subscription");
        window->usage_known = TRUE;
        window->used_percent = codexbar_usage_percent_display(
            codexbar_usage_percent_from_ratio(subscription_used, subscription_total));
        char *used_text = format_kwh(subscription_used);
        char *total_text = format_kwh(subscription_total);
        window->detail = g_strdup_printf("%s / %s kWh", used_text, total_text);
        g_free(used_text);
        g_free(total_text);
        if (subscription.has_end) {
            window->has_resets_at = TRUE;
            window->resets_at_ms = subscription.end_ms;
            if (!subscription.has_auto_renew || subscription.auto_renew) {
                provider->has_subscription_renews_at = TRUE;
                provider->subscription_renews_at_ms = subscription.end_ms;
            }
        }
        if (subscription.has_start && subscription.has_end && subscription.end_ms > subscription.start_ms) {
            window->has_window_minutes = TRUE;
            window->window_minutes = MAX(1, (subscription.end_ms - subscription.start_ms) / 60000);
        }
        codexbar_provider_add_quota_window(provider, window);
    }

    if (has_allowance && ((allowance.has_blocked && allowance.blocked) ||
                          (allowance.has_spent && allowance.has_limit && allowance.limit > 0.0))) {
        char *period = capitalized_words(allowance.period ? allowance.period : "allowance", FALSE, FALSE);
        char *title = g_strdup_printf("Key %s", period ? period : "Allowance");
        CodexBarQuotaWindow *window = codexbar_quota_window_new("key-allowance", title);
        window->output_id = g_strdup("key-allowance");
        window->usage_known = TRUE;
        window->used_percent = allowance.has_blocked && allowance.blocked
                                   ? 100.0
                                   : codexbar_usage_percent_display(
                                         codexbar_usage_percent_from_ratio(allowance.spent, allowance.limit));
        codexbar_provider_add_quota_window(provider, window);
        g_free(period);
        g_free(title);
    }

    char *login_method = capitalized_words(subscription.plan, TRUE, TRUE);
    if (login_method) {
        char *with_plan = g_strdup_printf("%s plan", login_method);
        g_free(login_method);
        provider->identity->login_method = with_plan;
    } else {
        provider->identity->login_method = capitalized_words(balance.accounting_method, FALSE, FALSE);
    }

    parsed_clear(&balance, &subscription, &allowance);
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_neuralwatt_parse_usage(const char *json, gint64 now_ms, GError **error) {
    return codexbar_neuralwatt_parse_usage_bytes(json, json ? strlen(json) : 0, now_ms, error);
}

static gboolean retryable_status(long status) {
    const long statuses[] = {408, 429, 500, 502, 503, 504};
    for (guint index = 0; index < G_N_ELEMENTS(statuses); index++) {
        if (status == statuses[index]) return TRUE;
    }
    return FALSE;
}

static gboolean retryable_error(const GError *error) {
    if (!error) return FALSE;
    if (error->domain == G_IO_ERROR) {
        return error->code == G_IO_ERROR_TIMED_OUT || error->code == G_IO_ERROR_CONNECTION_CLOSED ||
               error->code == G_IO_ERROR_CONNECTION_REFUSED || error->code == G_IO_ERROR_HOST_NOT_FOUND;
    }
    if (error->domain != g_quark_from_static_string("codexbar-http-error")) return FALSE;
    return error->code == CURLE_OPERATION_TIMEDOUT || error->code == CURLE_RECV_ERROR ||
           error->code == CURLE_SEND_ERROR || error->code == CURLE_PARTIAL_FILE ||
           error->code == CURLE_COULDNT_CONNECT || error->code == CURLE_COULDNT_RESOLVE_HOST ||
           error->code == CURLE_COULDNT_RESOLVE_PROXY || error->code == CURLE_GOT_NOTHING;
}

double codexbar_neuralwatt_retry_delay_for_testing(const CodexBarHttpResponse *response) {
    if (!response || !response->headers) return 1.0;
    const char *header = codexbar_http_response_header_first(response, "Retry-After");
    if (!header) return 1.0;
    char *clean = g_strstrip(g_strdup(header));
    char *end = NULL;
    double seconds = g_ascii_strtod(clean, &end);
    gboolean valid = clean[0] != '\0' && end && *end == '\0' && isfinite(seconds) && seconds >= 0.0;
    g_free(clean);
    return valid ? MIN(seconds, 10.0) : 1.0;
}

static gboolean wait_for_retry(double delay, GCancellable *cancellable, GError **error) {
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) return FALSE;
    if (delay <= 0.0) return TRUE;
    if (!cancellable) {
        g_usleep((gulong)(delay * G_USEC_PER_SEC));
        return TRUE;
    }
    gint64 deadline = g_get_monotonic_time() + (gint64)(delay * G_USEC_PER_SEC);
    while (!g_cancellable_is_cancelled(cancellable)) {
        gint64 remaining = deadline - g_get_monotonic_time();
        if (remaining <= 0) break;
        g_usleep((gulong)MIN(remaining, 10000));
    }
    return !g_cancellable_set_error_if_cancelled(cancellable, error);
}

static CodexBarHttpResponse *send_with_retry(
    const CodexBarHttpRequest *request, CodexBarNeuralWattTransport transport, GError **error) {
    for (guint attempt = 0; attempt < 2; attempt++) {
        if (request->cancellable && g_cancellable_set_error_if_cancelled(request->cancellable, error)) return NULL;
        GError *request_error = NULL;
        CodexBarHttpResponse *response = transport(request, &request_error);
        if (request->cancellable && g_cancellable_is_cancelled(request->cancellable)) {
            codexbar_http_response_free(response);
            g_clear_error(&request_error);
            g_cancellable_set_error_if_cancelled(request->cancellable, error);
            return NULL;
        }
        gboolean retry = attempt == 0 && ((response && retryable_status(response->status)) ||
                                          (!response && retryable_error(request_error)));
        if (!retry) {
            if (response) {
                g_clear_error(&request_error);
                return response;
            }
            if (request_error) {
                g_propagate_error(error, request_error);
            } else {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Neuralwatt network request failed");
            }
            return NULL;
        }
        double delay = response ? codexbar_neuralwatt_retry_delay_for_testing(response) : 1.0;
        codexbar_http_response_free(response);
        g_clear_error(&request_error);
        if (!wait_for_retry(delay, request->cancellable, error)) return NULL;
    }
    g_assert_not_reached();
}

static void wrap_network_error(GError **error) {
    if (!error || !*error || ((*error)->domain == G_IO_ERROR && (*error)->code == G_IO_ERROR_CANCELLED)) return;
    char *message = g_strdup((*error)->message);
    g_clear_error(error);
    if (strstr(message, "configured size limit") || strstr(message, "headers exceeded the size limit")) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_DATA,
                    "Failed to parse Neuralwatt response: %s",
                    message);
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Neuralwatt network error: %s", message);
    }
    g_free(message);
}

CodexBarProvider *codexbar_neuralwatt_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                           CodexBarNeuralWattTransport transport,
                                                                           GCancellable *cancellable,
                                                                           gint64 now_ms,
                                                                           GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    char *api_key = resolve_api_key(config);
    if (!api_key) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "Missing Neuralwatt API key. Set apiKey in the CodexBar config file or NEURALWATT_API_KEY.");
        return NULL;
    }
    const char *override = g_getenv("NEURALWATT_API_URL");
    char *url = codexbar_neuralwatt_quota_url_for_testing(override, error);
    if (!url) {
        g_free(api_key);
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", api_key);
    g_free(api_key);
    const CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
    };
    const CodexBarHttpRequest request = {
        .method = "GET",
        .url = url,
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 15,
        .maximum_response_bytes = NEURALWATT_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_with_retry(&request, transport, error);
    g_free(authorization);
    g_free(url);
    if (!response) {
        wrap_network_error(error);
        return NULL;
    }
    if (response->status == 401 || response->status == 403) {
        codexbar_http_response_free(response);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PERMISSION_DENIED,
                            "Missing Neuralwatt API key. Set apiKey in the CodexBar config file or NEURALWATT_API_KEY.");
        return NULL;
    }
    if (response->status != 200) {
        long status = response->status;
        codexbar_http_response_free(response);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Neuralwatt API error: HTTP %ld", status);
        return NULL;
    }
    CodexBarProvider *provider =
        codexbar_neuralwatt_parse_usage_bytes(response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    return provider;
}

CodexBarProvider *codexbar_neuralwatt_fetch_with_transport(const CodexBarProviderConfig *config,
                                                            CodexBarNeuralWattTransport transport,
                                                            gint64 now_ms,
                                                            GError **error) {
    return codexbar_neuralwatt_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_neuralwatt_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                              GCancellable *cancellable,
                                                              GError **error) {
    return codexbar_neuralwatt_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_neuralwatt_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_neuralwatt_fetch_with_cancellable(config, NULL, error);
}
