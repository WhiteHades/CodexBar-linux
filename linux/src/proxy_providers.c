#include "proxy_providers.h"

#include "http.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>

typedef struct {
    char *name;
    gint64 requests;
    gint64 successes;
    gint64 errors;
    gint64 tokens;
    gboolean has_cost;
    double cost;
} ProviderSummary;

static void provider_summary_free(ProviderSummary *summary);

static GQuark proxy_error_quark(void) {
    return g_quark_from_static_string("codexbar-proxy-provider-error");
}

static char *clean_value(const char *raw) {
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

static gboolean required_object(json_object *object, const char *key, json_object **result) {
    return json_object_object_get_ex(object, key, result) && json_object_is_type(*result, json_type_object);
}

static gboolean required_int(json_object *object, const char *key, gint64 *result) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || !json_object_is_type(value, json_type_int)) return FALSE;
    *result = json_object_get_int64(value);
    return TRUE;
}

static gboolean optional_int(json_object *object, const char *key, gboolean *present, gint64 *result) {
    json_object *value = NULL;
    *present = FALSE;
    if (!json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) return TRUE;
    if (!json_object_is_type(value, json_type_int)) return FALSE;
    *present = TRUE;
    *result = json_object_get_int64(value);
    return TRUE;
}

static gboolean optional_double(json_object *object, const char *key, gboolean *present, double *result) {
    json_object *value = NULL;
    *present = FALSE;
    if (!json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) return TRUE;
    if (!json_object_is_type(value, json_type_int) && !json_object_is_type(value, json_type_double)) return FALSE;
    double number = json_object_get_double(value);
    if (!isfinite(number)) return FALSE;
    *present = TRUE;
    *result = number;
    return TRUE;
}

static gboolean required_string(json_object *object, const char *key, const char **result) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || !json_object_is_type(value, json_type_string)) return FALSE;
    *result = json_object_get_string(value);
    return TRUE;
}

static gboolean required_bool(json_object *object, const char *key, gboolean *result) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || !json_object_is_type(value, json_type_boolean)) return FALSE;
    *result = json_object_get_boolean(value);
    return TRUE;
}

static char *grouped_integer(gint64 value) {
    char *plain = g_strdup_printf("%" G_GINT64_FORMAT, value);
    size_t start = plain[0] == '-' ? 1 : 0;
    size_t length = strlen(plain);
    GString *grouped = g_string_new_len(plain, start);
    for (size_t index = start; index < length; index++) {
        if (index > start && (length - index) % 3 == 0) g_string_append_c(grouped, ',');
        g_string_append_c(grouped, plain[index]);
    }
    g_free(plain);
    return g_string_free(grouped, FALSE);
}

static gboolean iso_timestamp(const char *raw, gint64 *result) {
    if (!raw) return FALSE;
    GDateTime *time = g_date_time_new_from_iso8601(raw, NULL);
    if (!time) return FALSE;
    *result = g_date_time_to_unix(time) * 1000 + g_date_time_get_microsecond(time) / 1000;
    g_date_time_unref(time);
    return TRUE;
}

static json_object *timestamp_json(gint64 timestamp_ms) {
    GDateTime *time = g_date_time_new_from_unix_utc(timestamp_ms / 1000);
    if (!time) return NULL;
    char *iso = g_date_time_format_iso8601(time);
    json_object *result = json_object_new_string(iso);
    g_free(iso);
    g_date_time_unref(time);
    return result;
}

static gboolean monthly_reset(const char *window_key, gint64 *result) {
    if (!window_key) return FALSE;
    const char *suffix = strrchr(window_key, '/');
    suffix = suffix ? suffix + 1 : window_key;
    if (strlen(suffix) != 7 || suffix[4] != '-') return FALSE;
    char year_text[5] = {0};
    char month_text[3] = {0};
    memcpy(year_text, suffix, 4);
    memcpy(month_text, suffix + 5, 2);
    char *year_end = NULL;
    char *month_end = NULL;
    gint64 year = g_ascii_strtoll(year_text, &year_end, 10);
    gint64 month = g_ascii_strtoll(month_text, &month_end, 10);
    if (!year_end || *year_end != '\0' || !month_end || *month_end != '\0' || year < 1 || year > 9998 ||
        month < 1 || month > 12) {
        return FALSE;
    }
    month++;
    if (month == 13) {
        month = 1;
        year++;
    }
    GDateTime *reset = g_date_time_new_utc((int)year, (int)month, 1, 0, 0, 0);
    if (!reset) return FALSE;
    *result = g_date_time_to_unix(reset) * 1000;
    g_date_time_unref(reset);
    return TRUE;
}

static gint compare_claw_summary(gconstpointer left, gconstpointer right) {
    const ProviderSummary *a = *(ProviderSummary *const *)left;
    const ProviderSummary *b = *(ProviderSummary *const *)right;
    if (a->cost != b->cost) return a->cost > b->cost ? -1 : 1;
    if (a->requests != b->requests) return a->requests > b->requests ? -1 : 1;
    return g_strcmp0(a->name, b->name);
}

static CodexBarProvider *provider_new(const char *id, gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup(id);
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    return provider;
}

char *codexbar_proxy_provider_url(const char *base_url, const char *leaf, GError **error) {
    char *normalized = codexbar_http_normalize_endpoint(base_url, CODEXBAR_HTTP_HTTPS_ONLY, error);
    if (!normalized) return NULL;
    GUri *uri = g_uri_parse(normalized, G_URI_FLAGS_NONE, NULL);
    if (!uri || g_uri_get_query(uri) || g_uri_get_fragment(uri)) {
        if (uri) g_uri_unref(uri);
        g_free(normalized);
        g_set_error_literal(error, proxy_error_quark(), 1, "Proxy base URL cannot contain a query or fragment");
        return NULL;
    }
    g_uri_unref(uri);
    while (g_str_has_suffix(normalized, "/")) normalized[strlen(normalized) - 1] = '\0';
    char **parts = g_strsplit(normalized, "/", -1);
    guint count = g_strv_length(parts);
    gboolean versioned = count > 0 && g_str_equal(parts[count - 1], "v1");
    g_strfreev(parts);
    char *url = g_strdup_printf("%s%s%s", normalized, versioned ? "/" : "/v1/", leaf);
    g_free(normalized);
    return url;
}

CodexBarProvider *codexbar_clawrouter_parse(const char *json, gint64 now_ms, GError **error) {
    json_object *root = json_tokener_parse(json);
    json_object *budget = NULL;
    json_object *usage = NULL;
    json_object *summary = NULL;
    json_object *providers = NULL;
    gboolean configured = FALSE;
    const char *ledger = NULL;
    gint64 request_count = 0;
    gint64 success_count = 0;
    gint64 error_count = 0;
    gint64 input_tokens = 0;
    gint64 output_tokens = 0;
    gint64 total_tokens = 0;
    gint64 actual_cost_micros = 0;
    if (!root || !json_object_is_type(root, json_type_object) || !required_object(root, "budget", &budget) ||
        !required_bool(budget, "configured", &configured) || !required_string(budget, "ledger", &ledger) ||
        !required_object(root, "usage", &usage) || !required_object(usage, "summary", &summary) ||
        !json_object_object_get_ex(usage, "providers", &providers) || !json_object_is_type(providers, json_type_array) ||
        !required_int(summary, "requestCount", &request_count) ||
        !required_int(summary, "successCount", &success_count) || !required_int(summary, "errorCount", &error_count) ||
        !required_int(summary, "inputTokens", &input_tokens) || !required_int(summary, "outputTokens", &output_tokens) ||
        !required_int(summary, "totalTokens", &total_tokens) ||
        !required_int(summary, "actualCostMicros", &actual_cost_micros)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, proxy_error_quark(), 2, "ClawRouter usage response is malformed");
        return NULL;
    }
    GPtrArray *provider_summaries = g_ptr_array_new_with_free_func((GDestroyNotify)provider_summary_free);
    for (size_t index = 0; index < json_object_array_length(providers); index++) {
        json_object *entry = json_object_array_get_idx(providers, index);
        const char *name = NULL;
        gint64 request_value = 0;
        gint64 success_value = 0;
        gint64 error_value = 0;
        gint64 token_value = 0;
        gint64 cost_value = 0;
        if (!json_object_is_type(entry, json_type_object) || !required_string(entry, "provider", &name) ||
            !required_int(entry, "requestCount", &request_value) ||
            !required_int(entry, "successCount", &success_value) ||
            !required_int(entry, "errorCount", &error_value) ||
            !required_int(entry, "totalTokens", &token_value) ||
            !required_int(entry, "actualCostMicros", &cost_value)) {
            g_ptr_array_unref(provider_summaries);
            json_object_put(root);
            g_set_error_literal(error, proxy_error_quark(), 2, "ClawRouter usage response is malformed");
            return NULL;
        }
        ProviderSummary *item = g_new0(ProviderSummary, 1);
        item->name = g_strdup(name);
        item->requests = request_value;
        item->successes = success_value;
        item->errors = error_value;
        item->tokens = token_value;
        item->has_cost = TRUE;
        item->cost = (double)cost_value / 1000000.0;
        g_ptr_array_add(provider_summaries, item);
    }

    gboolean has_limit = FALSE;
    gboolean has_spent = FALSE;
    gboolean has_remaining = FALSE;
    gint64 limit_micros = 0;
    gint64 spent_micros = 0;
    gint64 remaining_micros = 0;
    if (!optional_int(budget, "limitMicros", &has_limit, &limit_micros) ||
        !optional_int(budget, "spentMicros", &has_spent, &spent_micros) ||
        !optional_int(budget, "remainingMicros", &has_remaining, &remaining_micros)) {
        g_ptr_array_unref(provider_summaries);
        json_object_put(root);
        g_set_error_literal(error, proxy_error_quark(), 2, "ClawRouter usage response is malformed");
        return NULL;
    }

    CodexBarProvider *provider = provider_new("clawrouter", now_ms);
    provider->identity->organization = g_strdup_printf("%zu routed providers", json_object_array_length(providers));
    provider->identity->login_method = g_strdup(configured ? "Managed monthly budget" : "Unmetered");
    char *requests_text = grouped_integer(request_count);
    char *tokens_text = grouped_integer(total_tokens);
    provider->note = g_strdup_printf(
        "%s requests · %s tokens · %" G_GINT64_FORMAT " succeeded · %" G_GINT64_FORMAT " failed",
        requests_text,
        tokens_text,
        success_count,
        error_count);
    g_free(requests_text);
    g_free(tokens_text);
    json_object *window_key = NULL;
    gint64 reset_ms = 0;
    gboolean has_reset = json_object_object_get_ex(budget, "windowKey", &window_key) &&
                         json_object_is_type(window_key, json_type_string) &&
                         monthly_reset(json_object_get_string(window_key), &reset_ms);
    if (has_spent && has_limit && limit_micros > 0) {
        CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "monthly budget");
        window->usage_known = TRUE;
        window->used_percent = CLAMP(((double)spent_micros / (double)limit_micros) * 100.0, 0.0, 100.0);
        window->has_resets_at = has_reset;
        window->resets_at_ms = reset_ms;
        codexbar_provider_add_quota_window(provider, window);
    }
    g_ptr_array_sort(provider_summaries, compare_claw_summary);
    if ((has_spent && has_limit) || actual_cost_micros > 0) {
        provider->provider_cost = g_new0(CodexBarProviderCost, 1);
        provider->provider_cost->used = has_spent && has_limit ? (double)spent_micros / 1000000.0
                                                               : (double)actual_cost_micros / 1000000.0;
        provider->provider_cost->limit = has_spent && has_limit ? (double)limit_micros / 1000000.0 : 0.0;
        provider->provider_cost->currency = g_strdup("USD");
        provider->provider_cost->period = g_strdup("This month");
        provider->provider_cost->has_resets_at = has_reset;
        provider->provider_cost->resets_at_ms = reset_ms;
        provider->provider_cost->has_updated_at = TRUE;
        provider->provider_cost->updated_at_ms = now_ms;
    }
    json_object *claw_usage = json_object_new_object();
    json_object_object_add(claw_usage, "budgetConfigured", json_object_new_boolean(configured));
    json_object_object_add(claw_usage, "budgetLedger", json_object_new_string(ledger));
    if (has_limit) {
        json_object_object_add(claw_usage, "budgetLimitUSD", json_object_new_double((double)limit_micros / 1000000.0));
    }
    if (has_spent) {
        json_object_object_add(claw_usage, "budgetSpentUSD", json_object_new_double((double)spent_micros / 1000000.0));
    }
    if (has_remaining) {
        json_object_object_add(
            claw_usage, "budgetRemainingUSD", json_object_new_double((double)remaining_micros / 1000000.0));
    }
    if (has_reset) json_object_object_add(claw_usage, "budgetResetsAt", timestamp_json(reset_ms));
    json_object_object_add(claw_usage, "requestCount", json_object_new_int64(request_count));
    json_object_object_add(claw_usage, "successCount", json_object_new_int64(success_count));
    json_object_object_add(claw_usage, "errorCount", json_object_new_int64(error_count));
    json_object_object_add(claw_usage, "inputTokens", json_object_new_int64(input_tokens));
    json_object_object_add(claw_usage, "outputTokens", json_object_new_int64(output_tokens));
    json_object_object_add(claw_usage, "totalTokens", json_object_new_int64(total_tokens));
    json_object_object_add(
        claw_usage, "actualCostUSD", json_object_new_double((double)actual_cost_micros / 1000000.0));
    json_object *provider_array = json_object_new_array_ext((int)provider_summaries->len);
    for (guint index = 0; index < provider_summaries->len; index++) {
        ProviderSummary *item = g_ptr_array_index(provider_summaries, index);
        json_object *entry = json_object_new_object();
        json_object_object_add(entry, "provider", json_object_new_string(item->name));
        json_object_object_add(entry, "requestCount", json_object_new_int64(item->requests));
        json_object_object_add(entry, "successCount", json_object_new_int64(item->successes));
        json_object_object_add(entry, "errorCount", json_object_new_int64(item->errors));
        json_object_object_add(entry, "totalTokens", json_object_new_int64(item->tokens));
        json_object_object_add(entry, "actualCostUSD", json_object_new_double(item->cost));
        json_object_array_add(provider_array, entry);
    }
    json_object_object_add(claw_usage, "providers", provider_array);
    json_object_object_add(claw_usage, "updatedAt", timestamp_json(now_ms));
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "clawRouterUsage", claw_usage);
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));
    (void)ledger;
    g_ptr_array_unref(provider_summaries);
    json_object_put(root);
    return provider;
}

static void provider_summary_free(ProviderSummary *summary) {
    if (!summary) return;
    g_free(summary->name);
    g_free(summary);
}

static gint compare_provider_summary(gconstpointer left, gconstpointer right) {
    const ProviderSummary *a = *(ProviderSummary *const *)left;
    const ProviderSummary *b = *(ProviderSummary *const *)right;
    if (a->requests != b->requests) return a->requests > b->requests ? -1 : 1;
    return g_strcmp0(a->name, b->name);
}

static gint64 saturated_add(gint64 left, gint64 right) {
    if (right > 0 && left > G_MAXINT64 - right) return G_MAXINT64;
    if (right < 0 && left < G_MININT64 - right) return G_MININT64;
    return left + right;
}

static void scan_quota_groups(json_object *groups, double *minimum, gboolean *has_minimum,
                              gint64 *earliest_reset, gboolean *has_reset) {
    if (!groups) return;
    size_t count = json_object_is_type(groups, json_type_array) ? json_object_array_length(groups) : 0;
    if (json_object_is_type(groups, json_type_array)) {
        for (size_t index = 0; index < count; index++) {
            json_object *entry = json_object_array_get_idx(groups, index);
            if (!json_object_is_type(entry, json_type_object)) continue;
            gboolean present = FALSE;
            double remaining = 0.0;
            if (optional_double(entry, "remaining_percent", &present, &remaining) && present &&
                (!*has_minimum || remaining < *minimum)) {
                *minimum = remaining;
                *has_minimum = TRUE;
            }
            json_object *reset = NULL;
            gint64 parsed = 0;
            if (json_object_object_get_ex(entry, "reset_time", &reset) &&
                json_object_is_type(reset, json_type_string) && iso_timestamp(json_object_get_string(reset), &parsed) &&
                (!*has_reset || parsed < *earliest_reset)) {
                *earliest_reset = parsed;
                *has_reset = TRUE;
            }
        }
    } else if (json_object_is_type(groups, json_type_object)) {
        json_object_object_foreach(groups, key, entry) {
            (void)key;
            json_object *array = json_object_new_array();
            json_object_array_add(array, json_object_get(entry));
            scan_quota_groups(array, minimum, has_minimum, earliest_reset, has_reset);
            json_object_put(array);
        }
    }
}

CodexBarProvider *codexbar_llmproxy_parse(const char *json, gint64 now_ms, GError **error) {
    json_object *root = json_tokener_parse(json);
    json_object *providers = NULL;
    if (!root || !json_object_is_type(root, json_type_object) || !required_object(root, "providers", &providers)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, proxy_error_quark(), 3, "LLM Proxy quota response is malformed");
        return NULL;
    }
    GPtrArray *summaries = g_ptr_array_new_with_free_func((GDestroyNotify)provider_summary_free);
    gint64 credential_count = 0;
    gint64 active_count = 0;
    gint64 exhausted_count = 0;
    gint64 fallback_requests = 0;
    gint64 fallback_tokens = 0;
    double fallback_cost = 0.0;
    gboolean has_fallback_cost = FALSE;
    double minimum = 0.0;
    gboolean has_minimum = FALSE;
    gint64 earliest_reset = 0;
    gboolean has_reset = FALSE;
    gboolean malformed = FALSE;
    json_object_object_foreach(providers, name, stats) {
        if (!json_object_is_type(stats, json_type_object)) {
            malformed = TRUE;
            break;
        }
        ProviderSummary *provider_summary = g_new0(ProviderSummary, 1);
        provider_summary->name = g_strdup(name);
        gboolean present = FALSE;
        gint64 value = 0;
        if (!optional_int(stats, "credential_count", &present, &value)) malformed = TRUE;
        if (present) credential_count = saturated_add(credential_count, value);
        if (!optional_int(stats, "active_count", &present, &value)) malformed = TRUE;
        if (present) active_count = saturated_add(active_count, value);
        if (!optional_int(stats, "exhausted_count", &present, &value)) malformed = TRUE;
        if (present) exhausted_count = saturated_add(exhausted_count, value);
        if (!optional_int(stats, "total_requests", &present, &value)) malformed = TRUE;
        provider_summary->requests = present ? value : 0;
        fallback_requests = saturated_add(fallback_requests, provider_summary->requests);
        json_object *tokens = NULL;
        if (json_object_object_get_ex(stats, "tokens", &tokens) && !json_object_is_type(tokens, json_type_null)) {
            if (!json_object_is_type(tokens, json_type_object)) {
                malformed = TRUE;
            } else {
                const char *keys[] = {"input_cached", "input_uncached", "output"};
                for (guint index = 0; index < G_N_ELEMENTS(keys); index++) {
                    if (!optional_int(tokens, keys[index], &present, &value)) malformed = TRUE;
                    if (present) provider_summary->tokens = saturated_add(provider_summary->tokens, value);
                }
            }
        }
        fallback_tokens = saturated_add(fallback_tokens, provider_summary->tokens);
        if (!optional_double(stats, "approx_cost", &provider_summary->has_cost, &provider_summary->cost)) malformed = TRUE;
        if (provider_summary->has_cost) {
            fallback_cost += provider_summary->cost;
            has_fallback_cost = TRUE;
        }
        json_object *groups = NULL;
        if (json_object_object_get_ex(stats, "quota_groups", &groups)) {
            scan_quota_groups(groups, &minimum, &has_minimum, &earliest_reset, &has_reset);
        }
        g_ptr_array_add(summaries, provider_summary);
        if (malformed) break;
    }
    json_object *summary = NULL;
    if (json_object_object_get_ex(root, "summary", &summary) && !json_object_is_type(summary, json_type_null) &&
        !json_object_is_type(summary, json_type_object)) {
        malformed = TRUE;
    }
    gint64 total_requests = fallback_requests;
    gint64 total_tokens = fallback_tokens;
    double total_cost = fallback_cost;
    gboolean has_total_cost = has_fallback_cost && fallback_cost > 0.0;
    if (!malformed && summary && json_object_is_type(summary, json_type_object)) {
        gboolean present = FALSE;
        gint64 value = 0;
        if (!optional_int(summary, "total_requests", &present, &value)) malformed = TRUE;
        if (present) total_requests = value;
        if (!optional_int(summary, "total_tokens", &present, &value)) malformed = TRUE;
        if (present) total_tokens = value;
        if (!optional_double(summary, "approx_cost", &present, &total_cost)) malformed = TRUE;
        if (present) has_total_cost = TRUE;
    }
    if (malformed) {
        g_ptr_array_unref(summaries);
        json_object_put(root);
        g_set_error_literal(error, proxy_error_quark(), 3, "LLM Proxy quota response is malformed");
        return NULL;
    }
    g_ptr_array_sort(summaries, compare_provider_summary);

    CodexBarProvider *provider = provider_new("llmproxy", now_ms);
    provider->identity->organization = g_strdup_printf(
        "%" G_GINT64_FORMAT "/%" G_GINT64_FORMAT " active keys", active_count, credential_count);
    provider->identity->login_method = g_strdup("quota-stats");
    if (has_minimum) {
        CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "minimum quota");
        window->usage_known = TRUE;
        window->used_percent = CLAMP(100.0 - minimum, 0.0, 100.0);
        window->has_resets_at = has_reset;
        window->resets_at_ms = earliest_reset;
        codexbar_provider_add_quota_window(provider, window);
    }
    CodexBarQuotaWindow *requests = codexbar_quota_window_new("secondary", "requests");
    requests->usage_known = TRUE;
    char *request_text = grouped_integer(total_requests);
    requests->reset_description = g_strdup_printf("%s requests", request_text);
    g_free(request_text);
    codexbar_provider_add_quota_window(provider, requests);
    CodexBarQuotaWindow *tokens = codexbar_quota_window_new("tertiary", "tokens");
    tokens->usage_known = TRUE;
    char *token_text = grouped_integer(total_tokens);
    tokens->reset_description = g_strdup_printf("%s tokens", token_text);
    g_free(token_text);
    codexbar_provider_add_quota_window(provider, tokens);
    for (guint index = 0; index < MIN(summaries->len, 3); index++) {
        ProviderSummary *item = g_ptr_array_index(summaries, index);
        char *id = g_strdup_printf("llmproxy-%s", item->name);
        CodexBarQuotaWindow *window = codexbar_quota_window_new(id, item->name);
        g_free(id);
        window->output_id = g_strdup(item->name);
        window->usage_known = TRUE;
        char *item_requests = grouped_integer(item->requests);
        char *item_tokens = grouped_integer(item->tokens);
        window->reset_description = item->has_cost
                                        ? g_strdup_printf(
                                              "%s req · %s tok · $%.2f", item_requests, item_tokens, item->cost)
                                        : g_strdup_printf("%s req · %s tok", item_requests, item_tokens);
        g_free(item_requests);
        g_free(item_tokens);
        codexbar_provider_add_quota_window(provider, window);
    }
    if (has_total_cost) {
        provider->provider_cost = g_new0(CodexBarProviderCost, 1);
        provider->provider_cost->used = total_cost;
        provider->provider_cost->currency = g_strdup("USD");
        provider->provider_cost->period = g_strdup("Approx. spend");
        provider->provider_cost->has_resets_at = has_reset;
        provider->provider_cost->resets_at_ms = earliest_reset;
        provider->provider_cost->has_updated_at = TRUE;
        provider->provider_cost->updated_at_ms = now_ms;
    }
    (void)exhausted_count;
    g_ptr_array_unref(summaries);
    json_object_put(root);
    return provider;
}

CodexBarProvider *codexbar_proxy_provider_fetch(const CodexBarProviderConfig *config, GError **error) {
    gboolean clawrouter = g_str_equal(config->id, "clawrouter");
    const char *key_name = clawrouter ? "CLAWROUTER_API_KEY" : "LLM_PROXY_API_KEY";
    const char *base_name = clawrouter ? "CLAWROUTER_BASE_URL" : "LLM_PROXY_BASE_URL";
    char *token = clean_value(config->api_key);
    if (!token) token = clean_value(g_getenv(key_name));
    if (!token) {
        g_set_error(error, proxy_error_quark(), 4, "Missing %s API key.", clawrouter ? "ClawRouter" : "LLM Proxy");
        return NULL;
    }
    char *base = clean_value(config->enterprise_host);
    if (!base) base = clean_value(g_getenv(base_name));
    if (!base && clawrouter) base = g_strdup("https://clawrouter.openclaw.ai");
    if (!base) {
        g_free(token);
        g_set_error_literal(error, proxy_error_quark(), 4, "Missing LLM Proxy base URL.");
        return NULL;
    }
    char *url = codexbar_proxy_provider_url(base, clawrouter ? "usage" : "quota-stats", error);
    g_free(base);
    if (!url) {
        g_free(token);
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", token);
    g_free(token);
    CodexBarHttpResponse *response = codexbar_http_get(url, "Authorization", authorization, error);
    g_free(authorization);
    g_free(url);
    if (!response) return NULL;
    if ((clawrouter && (response->status == 401 || response->status == 403)) || response->status < 200 ||
        response->status >= 300) {
        if (clawrouter && (response->status == 401 || response->status == 403)) {
            g_set_error_literal(error, proxy_error_quark(), 5, "ClawRouter rejected the API key.");
        } else {
            g_set_error(error,
                        proxy_error_quark(),
                        5,
                        "%s API returned HTTP %ld",
                        clawrouter ? "ClawRouter" : "LLM Proxy",
                        response->status);
        }
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = clawrouter
                                     ? codexbar_clawrouter_parse(response->body, g_get_real_time() / 1000, error)
                                     : codexbar_llmproxy_parse(response->body, g_get_real_time() / 1000, error);
    codexbar_http_response_free(response);
    return provider;
}
