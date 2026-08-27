#include "web_providers4.h"

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

static json_object *alias_member(json_object *object, const char *first, const char *second) {
    json_object *value = member(object, first);
    return value ? value : member(object, second);
}

static gboolean number(json_object *value, double *result) {
    if (!value || json_object_is_type(value, json_type_null) || json_object_is_type(value, json_type_boolean)) {
        return FALSE;
    }
    double parsed = 0;
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        parsed = json_object_get_double(value);
    } else if (json_object_is_type(value, json_type_string)) {
        const char *raw = json_object_get_string(value);
        char *end = NULL;
        parsed = g_ascii_strtod(raw, &end);
        if (!raw[0] || !end || *end) return FALSE;
    } else {
        return FALSE;
    }
    if (!isfinite(parsed)) return FALSE;
    *result = parsed;
    return TRUE;
}

static char *string_member(json_object *object, const char *key) {
    json_object *value = member(object, key);
    if (!value || !json_object_is_type(value, json_type_string)) return NULL;
    const char *raw = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!raw || memchr(raw, '\0', length) || !g_utf8_validate(raw, (gssize)length, NULL)) return NULL;
    char *copy = g_strndup(raw, length);
    g_strstrip(copy);
    if (copy[0]) return copy;
    g_free(copy);
    return NULL;
}

static gboolean timestamp(json_object *value, gint64 *result) {
    double raw = 0;
    if (number(value, &raw)) {
        if (raw < 100000000000.0) raw *= 1000;
        if (raw <= 0 || raw > (double)G_MAXINT64) return FALSE;
        *result = (gint64)llround(raw);
        return TRUE;
    }
    if (!value || !json_object_is_type(value, json_type_string)) return FALSE;
    GDateTime *date = g_date_time_new_from_iso8601(json_object_get_string(value), NULL);
    if (!date) return FALSE;
    *result = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static char *raw_config(const CodexBarProviderConfig *config, const char *key) {
    return string_member(config ? config->raw : NULL, key);
}

static char *clean_credential(const char *raw) {
    if (!raw || strlen(raw) > CREDENTIAL_LIMIT || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *copy = g_strdup(raw);
    g_strstrip(copy);
    for (const unsigned char *cursor = (const unsigned char *)copy; *cursor; cursor++) {
        if (*cursor < 32 || *cursor == 127) {
            g_free(copy);
            return NULL;
        }
    }
    if (copy[0]) return copy;
    g_free(copy);
    return NULL;
}

static char *credential(const CodexBarProviderConfig *config,
                        const char *const *environment,
                        const char *bare_cookie) {
    char *raw = raw_config(config, "cookieHeader");
    char *value = clean_credential(raw);
    g_free(raw);
    for (size_t index = 0; !value && environment[index]; index++) {
        value = clean_credential(g_getenv(environment[index]));
    }
    if (!value) return NULL;
    if (g_ascii_strncasecmp(value, "cookie:", 7) == 0) {
        memmove(value, value + 7, strlen(value + 7) + 1);
        g_strstrip(value);
    }
    if (!strchr(value, '=') && bare_cookie) {
        char *wrapped = g_strdup_printf("%s=%s", bare_cookie, value);
        g_free(value);
        value = wrapped;
    }
    return value;
}

static CodexBarProvider *new_provider(const char *id, gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup(id);
    provider->source = g_strdup("web");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));
    return provider;
}

static CodexBarQuotaWindow *add_window(CodexBarProvider *provider,
                                       const char *id,
                                       const char *title,
                                       double used,
                                       double total,
                                       gint64 reset_ms,
                                       gboolean has_reset,
                                       const char *detail) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = total > 0 ? CLAMP(used / total * 100, 0, 100) : 0;
    window->has_resets_at = has_reset;
    window->resets_at_ms = reset_ms;
    window->detail = g_strdup(detail);
    codexbar_provider_add_quota_window(provider, window);
    return window;
}

static void command_add_rolling_window(CodexBarProvider *provider,
                                       json_object *limits,
                                       const char *key,
                                       const char *id,
                                       const char *title,
                                       gint64 window_minutes) {
    json_object *limit = member(limits, key);
    double cap = 0;
    if (!limit || !number(member(limit, "cap"), &cap) || cap <= 0) return;
    double used = 0;
    if (!number(member(limit, "used"), &used)) used = 0;
    gint64 reset_ms = 0;
    gboolean has_reset = timestamp(member(limit, "resetAt"), &reset_ms);
    CodexBarQuotaWindow *window = add_window(
        provider, id, title, MAX(0, used), cap, reset_ms, has_reset, NULL);
    window->has_window_minutes = TRUE;
    window->window_minutes = window_minutes;
}

static gint64 command_month_minutes(gint64 reset_ms) {
    if (reset_ms <= 0) return 30 * 24 * 60;
    GDateTime *end = g_date_time_new_from_unix_utc(reset_ms / 1000);
    GDateTime *start = end ? g_date_time_add_months(end, -1) : NULL;
    gint64 minutes = start ? g_date_time_difference(end, start) / G_TIME_SPAN_MINUTE : 0;
    if (start) g_date_time_unref(start);
    if (end) g_date_time_unref(end);
    return minutes > 0 ? minutes : 30 * 24 * 60;
}

static CodexBarHttpResponse *request(const char *url,
                                     const char *method,
                                     const char *cookie,
                                     const char *origin,
                                     const char *referer,
                                     const CodexBarHttpRequestHeader *extra_headers,
                                     size_t extra_header_count,
                                     long timeout_seconds,
                                     CodexBarWebProviders4Transport transport,
                                     GCancellable *cancellable,
                                     GError **error) {
    if (!transport) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Provider transport is missing");
        return NULL;
    }
    CodexBarHttpRequestHeader base_headers[] = {
        {"Cookie", cookie},
        {"Accept", "application/json, text/plain, */*"},
        {"Accept-Language", "en-US,en;q=0.9"},
        {"User-Agent", "Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 Chrome/143.0 Safari/537.36"},
        {"Origin", origin},
        {"Referer", referer},
    };
    size_t header_count = G_N_ELEMENTS(base_headers) + extra_header_count;
    CodexBarHttpRequestHeader *headers = g_new(CodexBarHttpRequestHeader, header_count);
    memcpy(headers, base_headers, sizeof(base_headers));
    if (extra_header_count) {
        memcpy(headers + G_N_ELEMENTS(base_headers),
               extra_headers,
               extra_header_count * sizeof(*extra_headers));
    }
    CodexBarHttpRequest http_request = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = header_count,
        .timeout_seconds = timeout_seconds,
        .maximum_response_bytes = RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) {
        g_free(headers);
        return NULL;
    }
    CodexBarHttpResponse *response = transport(&http_request, error);
    g_free(headers);
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        codexbar_http_response_free(response);
        if (error && *error) g_clear_error(error);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        return NULL;
    }
    if (!response) return NULL;
    if (response->effective_url) {
        GUri *actual = g_uri_parse(response->effective_url, G_URI_FLAGS_NONE, NULL);
        GUri *expected = g_uri_parse(url, G_URI_FLAGS_NONE, NULL);
        const char *actual_scheme = actual ? g_uri_get_scheme(actual) : NULL;
        const char *actual_host = actual ? g_uri_get_host(actual) : NULL;
        const char *expected_host = expected ? g_uri_get_host(expected) : NULL;
        gboolean same_origin = actual_scheme && actual_host && expected_host &&
                               g_ascii_strcasecmp(actual_scheme, "https") == 0 &&
                               g_ascii_strcasecmp(actual_host, expected_host) == 0 &&
                               g_uri_get_port(actual) == g_uri_get_port(expected);
        if (actual) g_uri_unref(actual);
        if (expected) g_uri_unref(expected);
        if (!same_origin) {
            codexbar_http_response_free(response);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                                "Provider redirected outside its trusted origin");
            return NULL;
        }
    }
    if (response->status == 200) return response;
    g_set_error(error,
                G_IO_ERROR,
                response->status == 401 || response->status == 403
                    ? G_IO_ERROR_PERMISSION_DENIED
                    : G_IO_ERROR_FAILED,
                "Provider returned HTTP %ld",
                response->status);
    codexbar_http_response_free(response);
    return NULL;
}

CodexBarProvider *codexbar_commandcode_parse(const char *credits,
                                             size_t credits_length,
                                             const char *subscription,
                                             size_t subscription_length,
                                             gint64 now_ms,
                                             GError **error) {
    json_object *root = parse_json(credits, credits_length);
    json_object *data = root ? member(root, "credits") : NULL;
    double remaining = 0;
    if (!data || !number(member(data, "monthlyCredits"), &remaining) || remaining < 0) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Command Code credits are invalid");
        return NULL;
    }
    json_object *subscription_root = subscription ? parse_json(subscription, subscription_length) : NULL;
    json_object *subscription_success = member(subscription_root, "success");
    json_object *subscription_value = NULL;
    gboolean subscription_envelope =
        subscription_root && json_object_is_type(subscription_success, json_type_boolean) &&
        json_object_get_boolean(subscription_success) &&
        json_object_object_get_ex(subscription_root, "data", &subscription_value);
    gboolean subscription_free = subscription_envelope &&
                                 json_object_is_type(subscription_value, json_type_null);
    json_object *subscription_data =
        subscription_envelope && json_object_is_type(subscription_value, json_type_object)
            ? subscription_value
            : NULL;
    char *plan_id = string_member(subscription_data, "planId");
    char *status = string_member(subscription_data, "status");
    gboolean subscription_available = subscription_free || plan_id != NULL;
    double total = 0;
    double purchased = 0;
    number(member(data, "purchasedCredits"), &purchased);
    const char *plan_name = NULL;
    if (plan_id) {
        if (g_ascii_strcasecmp(plan_id, "individual-go") == 0) total = 10, plan_name = "Go";
        else if (g_ascii_strcasecmp(plan_id, "individual-goat") == 0) total = 70, plan_name = "GOAT";
        else if (g_ascii_strcasecmp(plan_id, "individual-pro") == 0) total = 30, plan_name = "Pro";
        else if (g_ascii_strcasecmp(plan_id, "individual-pro-v1") == 0) total = 80, plan_name = "Pro";
        else if (g_ascii_strcasecmp(plan_id, "individual-max") == 0) total = 150, plan_name = "Max";
        else if (g_ascii_strcasecmp(plan_id, "individual-ultra") == 0) total = 300, plan_name = "Ultra";
    }
    if (plan_id && status && g_ascii_strcasecmp(status, "active") == 0 && total <= 0) {
        g_free(status);
        g_free(plan_id);
        if (subscription_root) json_object_put(subscription_root);
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Command Code plan is unknown");
        return NULL;
    }
    gint64 reset_ms = 0;
    gboolean has_reset = timestamp(member(subscription_data, "currentPeriodEnd"), &reset_ms);
    CodexBarProvider *provider = new_provider("commandcode", now_ms);
    provider->plan = g_strdup(plan_name);
    provider->explicit_quota_slots = TRUE;
    json_object *window_limits = member(root, "windowLimits");
    if (!json_object_is_type(window_limits, json_type_object)) window_limits = member(data, "windowLimits");
    command_add_rolling_window(provider, window_limits, "fiveHour", "primary", "5-hour", 5 * 60);
    command_add_rolling_window(provider, window_limits, "weekly", "secondary", "Weekly", 7 * 24 * 60);
    if (total > 0) {
        char *detail = g_strdup_printf("$%.2f / $%.2f remaining", remaining, total);
        CodexBarQuotaWindow *monthly = add_window(provider, "tertiary", "Monthly credits",
                                                   MAX(0, total - remaining), total,
                                                   reset_ms, has_reset, detail);
        monthly->has_window_minutes = TRUE;
        monthly->window_minutes = command_month_minutes(reset_ms);
        g_free(detail);
    } else if (remaining > 0 || purchased > 0) {
        CodexBarQuotaWindow *monthly = add_window(provider, "tertiary", "Monthly credits",
                                                   0, MAX(remaining, 1), reset_ms, has_reset, NULL);
        monthly->has_window_minutes = TRUE;
        monthly->window_minutes = command_month_minutes(reset_ms);
    }
    json_object_object_add(provider->usage_extensions,
                           "commandCodeSubscriptionEnrichmentUnavailable",
                           json_object_new_boolean(!subscription_available));
    json_object_object_add(provider->usage_extensions,
                           "commandCodeHasSubscriptionPlan",
                           json_object_new_boolean(plan_name != NULL));
    json_object_object_add(provider->usage_extensions,
                           "commandCodeMonthlyGrantDepleted",
                           json_object_new_boolean(remaining <= 0));
    codexbar_provider_add_balance(provider, codexbar_balance_new("monthly", "Monthly credits", remaining, "USD"));
    const char *credit_keys[] = {"purchasedCredits", "premiumMonthlyCredits", "opensourceMonthlyCredits"};
    const char *credit_ids[] = {"purchased", "premium", "opensource"};
    const char *credit_titles[] = {"Purchased credits", "Premium credits", "Open-source credits"};
    for (size_t index = 0; index < G_N_ELEMENTS(credit_keys); index++) {
        double value = 0;
        if (number(member(data, credit_keys[index]), &value) && value > 0) {
            codexbar_provider_add_balance(
                provider, codexbar_balance_new(credit_ids[index], credit_titles[index], value, "USD"));
        }
    }
    g_free(status);
    g_free(plan_id);
    if (subscription_root) json_object_put(subscription_root);
    json_object_put(root);
    return provider;
}

static const char *const command_environment[] = {"COMMANDCODE_COOKIE", "COMMAND_CODE_COOKIE", NULL};

CodexBarProvider *codexbar_commandcode_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *cookie = credential(config, command_environment, "__Secure-better-auth.session_token");
    if (!cookie) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Command Code cookie is missing");
        return NULL;
    }
    CodexBarHttpResponse *credits = request("https://api.commandcode.ai/internal/billing/credits", "GET", cookie,
                                           "https://commandcode.ai", "https://commandcode.ai/", NULL, 0, 15, transport,
                                           cancellable, error);
    if (!credits) {
        g_free(cookie);
        return NULL;
    }
    GError *optional_error = NULL;
    CodexBarHttpResponse *subscription = request("https://api.commandcode.ai/internal/billing/subscriptions", "GET",
                                                cookie, "https://commandcode.ai", "https://commandcode.ai/", NULL, 0,
                                                2, transport,
                                                cancellable, &optional_error);
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        codexbar_http_response_free(subscription);
        codexbar_http_response_free(credits);
        g_free(cookie);
        g_clear_error(&optional_error);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        return NULL;
    }
    g_clear_error(&optional_error);
    CodexBarProvider *provider = codexbar_commandcode_parse(
        credits->body, credits->body_length,
        subscription ? subscription->body : NULL, subscription ? subscription->body_length : 0,
        now_ms, error);
    if (provider) {
        provider->runtime_scope = g_compute_checksum_for_string(G_CHECKSUM_SHA256, cookie, -1);
    }
    codexbar_http_response_free(subscription);
    codexbar_http_response_free(credits);
    g_free(cookie);
    return provider;
}

CodexBarProvider *codexbar_commandcode_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                              GCancellable *cancellable,
                                                              GError **error) {
    return codexbar_commandcode_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_qoder_parse(const char *json, size_t length, gint64 now_ms, GError **error) {
    json_object *root = parse_json(json, length);
    json_object *total = root ? alias_member(root, "totalQuota", "total_quota") : NULL;
    json_object *summary = alias_member(total, "quotaSummary", "quota_summary");
    double used = 0, limit = 0, remaining = 0, shared_used = 0, shared_limit = 0, shared_remaining = 0;
    if (!summary || !number(alias_member(summary, "usedValue", "used_value"), &used) ||
        !number(alias_member(summary, "limitValue", "limit_value"), &limit) || used < 0 || limit < 0) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Qoder quota is invalid");
        return NULL;
    }
    json_object *remaining_value = alias_member(summary, "remainingValue", "remaining_value");
    if (remaining_value) {
        if (!number(remaining_value, &remaining) || remaining < 0) {
            json_object_put(root);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Qoder remaining quota is invalid");
            return NULL;
        }
    } else {
        remaining = MAX(0, limit - used);
    }
    json_object *shared = alias_member(root, "sharedQuota", "shared_quota");
    json_object *shared_summary = alias_member(shared, "quotaSummary", "quota_summary");
    if (shared_summary) {
        if (!number(alias_member(shared_summary, "usedValue", "used_value"), &shared_used) ||
            !number(alias_member(shared_summary, "limitValue", "limit_value"), &shared_limit) ||
            shared_used < 0 || shared_limit < 0) {
            json_object_put(root);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Qoder shared quota is invalid");
            return NULL;
        }
        json_object *shared_remaining_value = alias_member(shared_summary, "remainingValue", "remaining_value");
        if (shared_remaining_value) {
            if (!number(shared_remaining_value, &shared_remaining) || shared_remaining < 0) {
                json_object_put(root);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                    "Qoder shared remaining quota is invalid");
                return NULL;
            }
        } else {
            shared_remaining = MAX(0, shared_limit - shared_used);
        }
    }
    used += shared_used;
    limit += shared_limit;
    remaining += shared_remaining;
    if (limit == 0 && (used != 0 || remaining != 0)) {
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Qoder zero quota has usage");
        return NULL;
    }
    gint64 reset_ms = 0;
    gboolean has_reset = timestamp(alias_member(root, "nextResetAt", "next_reset_at"), &reset_ms);
    CodexBarProvider *provider = new_provider("qoder", now_ms);
    char *detail = g_strdup_printf("%.0f / %.0f credits used · %.0f remaining", used, limit, remaining);
    add_window(provider, "primary", "Big model credits", used, limit, reset_ms, has_reset, detail);
    double percentage = 0;
    json_object *provided_percentage = alias_member(summary, "usagePercentage", "usage_percentage");
    if (!shared_summary && provided_percentage && number(provided_percentage, &percentage) &&
        percentage >= 0 && percentage <= 100) {
        codexbar_provider_quota_window(provider, 0)->used_percent = percentage;
    } else if (limit == 0) {
        codexbar_provider_quota_window(provider, 0)->used_percent = 100;
    }
    g_free(detail);
    json_object_put(root);
    return provider;
}

static const char *const qoder_environment[] = {"QODER_COOKIE", "qoder_cookie", NULL};

CodexBarProvider *codexbar_qoder_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *cookie = credential(config, qoder_environment, NULL);
    if (!cookie) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Qoder cookie is missing");
        return NULL;
    }
    gboolean china = config && config->region && g_ascii_strcasecmp(config->region, "cn") == 0;
    const char *origin = china ? "https://qoder.com.cn" : "https://qoder.com";
    char *url = g_strdup_printf("%s/api/v2/me/usages/big_model_credits", origin);
    char *referer = g_strdup_printf("%s/account/usage", origin);
    const CodexBarHttpRequestHeader qoder_headers[] = {
        {"X-Requested-With", "XMLHttpRequest"},
        {"Bx-V", "2.5.35"},
    };
    CodexBarHttpResponse *response = request(url, "GET", cookie, origin, referer,
                                             qoder_headers, G_N_ELEMENTS(qoder_headers),
                                             15, transport, cancellable, error);
    CodexBarProvider *provider = response
        ? codexbar_qoder_parse(response->body, response->body_length, now_ms, error)
        : NULL;
    codexbar_http_response_free(response);
    g_free(referer);
    g_free(url);
    g_free(cookie);
    return provider;
}

CodexBarProvider *codexbar_qoder_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                        GCancellable *cancellable,
                                                        GError **error) {
    return codexbar_qoder_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_perplexity_parse(const char *json, size_t length, gint64 now_ms, GError **error) {
    json_object *root = parse_json(json, length);
    json_object *grants = root ? member(root, "credit_grants") : NULL;
    double balance = 0, purchased_field = 0, total_usage = 0;
    gint64 reset_ms = 0;
    if (!root || !number(member(root, "balance_cents"), &balance) || balance < 0 ||
        !number(member(root, "current_period_purchased_cents"), &purchased_field) || purchased_field < 0 ||
        !number(member(root, "total_usage_cents"), &total_usage) || total_usage < 0 ||
        !timestamp(member(root, "renewal_date_ts"), &reset_ms) ||
        !grants || !json_object_is_type(grants, json_type_array)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Perplexity credits are invalid");
        return NULL;
    }
    double recurring = 0, promotional = 0, purchased_grants = 0;
    gint64 promo_expiry_ms = 0;
    size_t grant_count = json_object_array_length(grants);
    for (size_t index = 0; index < grant_count; index++) {
        json_object *grant = json_object_array_get_idx(grants, index);
        char *type = string_member(grant, "type");
        double amount = 0;
        if (!type || !number(member(grant, "amount_cents"), &amount) || amount < 0) {
            g_free(type);
            json_object_put(root);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Perplexity grant is invalid");
            return NULL;
        }
        if (g_str_equal(type, "recurring")) {
            recurring += amount;
        } else if (g_str_equal(type, "purchased")) {
            purchased_grants += amount;
        } else if (g_str_equal(type, "promotional")) {
            gint64 expiry_ms = 0;
            json_object *expiry = member(grant, "expires_at_ts");
            gboolean has_expiry = !expiry || json_object_is_type(expiry, json_type_null) ||
                                  timestamp(expiry, &expiry_ms);
            if (!has_expiry) {
                g_free(type);
                json_object_put(root);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                    "Perplexity grant expiry is invalid");
                return NULL;
            }
            if (!expiry || json_object_is_type(expiry, json_type_null) || expiry_ms > now_ms) {
                promotional += amount;
                if (expiry_ms > 0 && (promo_expiry_ms == 0 || expiry_ms < promo_expiry_ms)) {
                    promo_expiry_ms = expiry_ms;
                }
            }
        }
        g_free(type);
    }
    double purchased = MAX(purchased_field, purchased_grants);
    double remaining_usage = total_usage;
    double recurring_used = MIN(remaining_usage, recurring);
    remaining_usage -= recurring_used;
    double purchased_used = MIN(remaining_usage, purchased);
    remaining_usage -= purchased_used;
    double promotional_used = MIN(remaining_usage, promotional);
    CodexBarProvider *provider = new_provider("perplexity", now_ms);
    provider->plan = g_strdup(recurring >= 5000 ? "Max" : recurring > 0 ? "Pro" : NULL);
    if (recurring > 0) {
        add_window(provider, "primary", "Recurring credits", recurring_used, recurring, reset_ms, TRUE, NULL);
    } else if (promotional <= 0 && purchased <= 0) {
        add_window(provider, "primary", "Recurring credits", 0, 0, reset_ms, TRUE, "0 / 0 credits");
        codexbar_provider_quota_window(provider, 0)->used_percent = 100;
    }
    add_window(provider, "promotional", "Promotional credits", promotional_used, promotional,
               promo_expiry_ms, promo_expiry_ms > 0, NULL);
    if (promotional <= 0) {
        codexbar_provider_quota_window(provider, provider->quota_windows->len - 1)->used_percent = 100;
    }
    add_window(provider, "purchased", "Purchased credits", purchased_used, purchased, 0, FALSE, NULL);
    if (purchased <= 0) codexbar_provider_quota_window(provider, provider->quota_windows->len - 1)->used_percent = 100;
    codexbar_provider_add_balance(provider,
                                 codexbar_balance_new("credits", "Credit balance", balance / 100, "USD"));
    json_object_put(root);
    return provider;
}

static const char *const perplexity_environment[] = {
    "PERPLEXITY_SESSION_TOKEN", "perplexity_session_token", "PERPLEXITY_COOKIE", NULL};

CodexBarProvider *codexbar_perplexity_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *cookie = credential(config, perplexity_environment, "__Secure-authjs.session-token");
    if (!cookie) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Perplexity session is missing");
        return NULL;
    }
    CodexBarHttpResponse *response = request(
        "https://www.perplexity.ai/rest/billing/credits?version=2.18&source=default", "GET", cookie,
        "https://www.perplexity.ai", "https://www.perplexity.ai/account/usage", NULL, 0,
        15, transport, cancellable, error);
    CodexBarProvider *provider = response
        ? codexbar_perplexity_parse(response->body, response->body_length, now_ms, error)
        : NULL;
    codexbar_http_response_free(response);
    g_free(cookie);
    return provider;
}

CodexBarProvider *codexbar_perplexity_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                             GCancellable *cancellable,
                                                             GError **error) {
    return codexbar_perplexity_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

static json_object *longcat_data(json_object *root) {
    double code = 0;
    if (!root || !number(member(root, "code"), &code) || (code != 0 && code != 200)) return NULL;
    return member(root, "data");
}

CodexBarProvider *codexbar_longcat_parse(const char *user_json,
                                         size_t user_length,
                                         const char *usage_json,
                                         size_t usage_length,
                                         const char *fuel_json,
                                         size_t fuel_length,
                                         gint64 now_ms,
                                         GError **error) {
    json_object *user_root = parse_json(user_json, user_length);
    json_object *usage_root = parse_json(usage_json, usage_length);
    json_object *user = longcat_data(user_root);
    json_object *usage = longcat_data(usage_root);
    if (usage && member(usage, "usage")) usage = member(usage, "usage");
    double total = 0, used = 0, available = 0;
    if (!user || !usage || !number(member(usage, "totalToken"), &total) ||
        !number(member(usage, "usedToken"), &used) || !number(member(usage, "availableToken"), &available) ||
        total < 0 || used < 0 || available < 0) {
        if (user_root) json_object_put(user_root);
        if (usage_root) json_object_put(usage_root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "LongCat usage is invalid");
        return NULL;
    }
    CodexBarProvider *provider = new_provider("longcat", now_ms);
    char *detail = g_strdup_printf("%.0f / %.0f tokens", used, total);
    add_window(provider, "primary", "Token plan", used, total, 0, FALSE, detail);
    g_free(detail);
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->account = string_member(user, "name");
    if (!provider->account) provider->account = string_member(user, "nickName");
    provider->identity->account_id = g_strdup(provider->account);
    codexbar_provider_add_balance(provider,
                                 codexbar_balance_new("tokens", "Available tokens", available, "tokens"));
    json_object *fuel_root = fuel_json ? parse_json(fuel_json, fuel_length) : NULL;
    json_object *fuel = longcat_data(fuel_root);
    double fuel_total = 0;
    json_object *packages = fuel ? member(fuel, "list") : NULL;
    if (fuel && number(member(fuel, "totalQuota"), &fuel_total) && fuel_total > 0 &&
        packages && json_object_is_type(packages, json_type_array)) {
        double fuel_remaining = 0;
        gint64 nearest_expiry_ms = 0;
        size_t package_count = json_object_array_length(packages);
        for (size_t index = 0; index < package_count; index++) {
            json_object *package = json_object_array_get_idx(packages, index);
            double package_remaining = 0;
            if (number(member(package, "availableToken"), &package_remaining) && package_remaining >= 0) {
                fuel_remaining += package_remaining;
            }
            gint64 expiry_ms = 0;
            if (timestamp(member(package, "expireTime"), &expiry_ms) &&
                (nearest_expiry_ms == 0 || expiry_ms < nearest_expiry_ms)) {
                nearest_expiry_ms = expiry_ms;
            }
        }
        char *fuel_detail = g_strdup_printf("%.0f / %.0f tokens remaining", fuel_remaining, fuel_total);
        add_window(provider, "fuel", "Fuel packages", MAX(0, fuel_total - fuel_remaining), fuel_total,
                   nearest_expiry_ms, nearest_expiry_ms > 0, fuel_detail);
        g_free(fuel_detail);
        CodexBarBalance *fuel_balance = codexbar_balance_new(
            "fuel", "Fuel packages", fuel_remaining, "tokens");
        fuel_balance->has_limit = TRUE;
        fuel_balance->limit = fuel_total;
        fuel_balance->has_used = TRUE;
        fuel_balance->used = MAX(0, fuel_total - fuel_remaining);
        fuel_balance->has_expiry = nearest_expiry_ms > 0;
        fuel_balance->expiry_ms = nearest_expiry_ms;
        codexbar_provider_add_balance(provider, fuel_balance);
    }
    if (fuel_root) json_object_put(fuel_root);
    json_object_put(usage_root);
    json_object_put(user_root);
    return provider;
}

static const char *const longcat_environment[] = {"LONGCAT_MANUAL_COOKIE", "longcat_manual_cookie", NULL};

CodexBarProvider *codexbar_longcat_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders4Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *cookie = credential(config, longcat_environment, NULL);
    if (!cookie) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "LongCat cookie is missing");
        return NULL;
    }
    const char *origin = "https://longcat.chat";
    const char *referer = "https://longcat.chat/platform/usage";
    CodexBarHttpResponse *user = request("https://longcat.chat/api/v1/user-current", "GET", cookie,
                                        origin, referer, NULL, 0, 15, transport, cancellable, error);
    CodexBarHttpResponse *usage = user
        ? request("https://longcat.chat/api/lc-platform/v1/tokenUsage", "GET", cookie,
                  origin, referer, NULL, 0, 15, transport, cancellable, error)
        : NULL;
    GError *optional_error = NULL;
    CodexBarHttpResponse *fuel = usage
        ? request("https://longcat.chat/api/lc-platform/v1/pending-fuel-packages", "GET", cookie,
                  origin, referer, NULL, 0, 15, transport, cancellable, &optional_error)
        : NULL;
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        codexbar_http_response_free(fuel);
        codexbar_http_response_free(usage);
        codexbar_http_response_free(user);
        g_free(cookie);
        g_clear_error(&optional_error);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        return NULL;
    }
    g_clear_error(&optional_error);
    CodexBarProvider *provider = user && usage
        ? codexbar_longcat_parse(user->body, user->body_length, usage->body, usage->body_length,
                                fuel ? fuel->body : NULL, fuel ? fuel->body_length : 0, now_ms, error)
        : NULL;
    codexbar_http_response_free(fuel);
    codexbar_http_response_free(usage);
    codexbar_http_response_free(user);
    g_free(cookie);
    return provider;
}

CodexBarProvider *codexbar_longcat_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error) {
    return codexbar_longcat_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}
