#include "web_providers3.h"

#include <json-c/json.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#define WEB3_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)
#define WEB3_MAXIMUM_CREDENTIAL_BYTES 16384U
#define WEB3_TIMEOUT_SECONDS 15
#define WEB3_OPTIONAL_TIMEOUT_SECONDS 4

#define SAKANA_BILLING_URL "https://console.sakana.ai/billing"
#define SAKANA_PAYG_URL "https://console.sakana.ai/billing?tab=payAsYouGo"
#define ABACUS_COMPUTE_URL "https://apps.abacus.ai/api/_getOrganizationComputePoints"
#define ABACUS_BILLING_URL "https://apps.abacus.ai/api/_getBillingInfo"
#define MISTRAL_USAGE_URL_FORMAT \
    "https://admin.mistral.ai/api/billing/v2/usage?month=%d&year=%d"
#define MISTRAL_CREDITS_URL "https://admin.mistral.ai/api/billing/credits"
#define MISTRAL_VIBE_URL \
    "https://console.mistral.ai/api-ui/trpc/billing.vibeUsage?batch=1&input=" \
    "%7B%220%22%3A%7B%22json%22%3Anull%2C%22meta%22%3A%7B%22values%22%3A" \
    "%5B%22undefined%22%5D%2C%22v%22%3A1%7D%7D%7D"

static gboolean json_whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

static json_object *parse_json_document(const char *json, size_t length) {
    if (!json || length == 0 || length > G_MAXINT || length > WEB3_MAXIMUM_RESPONSE_BYTES ||
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

static char *clean_header_value(const char *raw) {
    if (!raw || !g_utf8_validate(raw, -1, NULL) || strlen(raw) > WEB3_MAXIMUM_CREDENTIAL_BYTES) return NULL;
    char *value = g_strdup(raw);
    strip_unicode_whitespace(value);
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (*cursor < 32 || *cursor == 127) {
            g_free(value);
            return NULL;
        }
    }
    if (value[0] != '\0') return value;
    g_free(value);
    return NULL;
}

static char *raw_string(const CodexBarProviderConfig *config, const char *key) {
    json_object *value = object_member(config ? config->raw : NULL, key);
    if (!value || !json_object_is_type(value, json_type_string)) return NULL;
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!text || memchr(text, '\0', length) || !g_utf8_validate(text, (gssize)length, NULL)) return NULL;
    char *copy = g_strndup(text, length);
    strip_unicode_whitespace(copy);
    if (copy[0] != '\0') return copy;
    g_free(copy);
    return NULL;
}

static char *first_config_or_environment(const CodexBarProviderConfig *config,
                                         const char *const *environment_keys) {
    char *value = raw_string(config, "cookieHeader");
    for (size_t index = 0; !value && environment_keys[index]; index++) {
        value = clean_header_value(g_getenv(environment_keys[index]));
    }
    return value;
}

static gboolean valid_cookie_name(const char *name) {
    if (!name || name[0] == '\0') return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)name; *cursor; cursor++) {
        if (*cursor <= 32 || *cursor >= 127 || strchr("()<>@,;:\\\"/[]?={} ", *cursor)) return FALSE;
    }
    return TRUE;
}

static gboolean safe_cookie_value(const char *value) {
    if (!value || value[0] == '\0') return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (*cursor < 33 || *cursor == 127 || *cursor == ';' || *cursor == ',') return FALSE;
    }
    return TRUE;
}

static char *normalize_cookie_header(const char *raw) {
    char *header = clean_header_value(raw);
    if (!header) return NULL;
    if (g_ascii_strncasecmp(header, "cookie:", 7) == 0) {
        memmove(header, header + 7, strlen(header + 7) + 1);
        strip_unicode_whitespace(header);
    }
    GString *result = g_string_new(NULL);
    char **parts = g_strsplit(header, ";", -1);
    for (size_t index = 0; parts[index]; index++) {
        char *part = g_strstrip(parts[index]);
        char *separator = strchr(part, '=');
        if (!separator || separator == part) continue;
        *separator = '\0';
        char *name = g_strstrip(part);
        char *value = g_strstrip(separator + 1);
        if (!valid_cookie_name(name) || !safe_cookie_value(value)) continue;
        if (result->len > 0) g_string_append(result, "; ");
        g_string_append_printf(result, "%s=%s", name, value);
    }
    g_strfreev(parts);
    g_free(header);
    if (result->len > 0) return g_string_free(result, FALSE);
    g_string_free(result, TRUE);
    return NULL;
}

static char *provider_cookie(const CodexBarProviderConfig *config,
                             const char *const *environment_keys) {
    char *raw = first_config_or_environment(config, environment_keys);
    char *cookie = normalize_cookie_header(raw);
    g_free(raw);
    return cookie;
}

static gboolean json_number(json_object *value, double *result) {
    if (!value || json_object_is_type(value, json_type_null) || json_object_is_type(value, json_type_boolean)) {
        return FALSE;
    }
    double number = 0;
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        number = json_object_get_double(value);
    } else if (json_object_is_type(value, json_type_string)) {
        const char *text = json_object_get_string(value);
        size_t length = (size_t)json_object_get_string_len(value);
        if (!text || memchr(text, '\0', length)) return FALSE;
        char *copy = g_strndup(text, length);
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

static char *json_clean_string(json_object *object, const char *key) {
    json_object *value = object_member(object, key);
    if (!value || !json_object_is_type(value, json_type_string)) return NULL;
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!text || memchr(text, '\0', length) || !g_utf8_validate(text, (gssize)length, NULL)) return NULL;
    char *copy = g_strndup(text, length);
    strip_unicode_whitespace(copy);
    if (copy[0] != '\0') return copy;
    g_free(copy);
    return NULL;
}

static gboolean parse_timestamp_string(const char *text, gint64 *result) {
    if (!text || text[0] == '\0') return FALSE;
    GDateTime *date = g_date_time_new_from_iso8601(text, NULL);
    if (!date) return FALSE;
    *result = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static gboolean response_origin_is(const CodexBarHttpResponse *response, const char *expected_url) {
    if (!response->effective_url) return TRUE;
    GUri *actual = g_uri_parse(response->effective_url, G_URI_FLAGS_NONE, NULL);
    GUri *expected = g_uri_parse(expected_url, G_URI_FLAGS_NONE, NULL);
    const char *actual_scheme = actual ? g_uri_get_scheme(actual) : NULL;
    const char *expected_scheme = expected ? g_uri_get_scheme(expected) : NULL;
    const char *actual_host = actual ? g_uri_get_host(actual) : NULL;
    const char *expected_host = expected ? g_uri_get_host(expected) : NULL;
    gboolean matches = actual_scheme && expected_scheme && actual_host && expected_host &&
                       g_ascii_strcasecmp(g_uri_get_scheme(actual), "https") == 0 &&
                       g_ascii_strcasecmp(g_uri_get_scheme(expected), "https") == 0 &&
                       g_ascii_strcasecmp(actual_host, expected_host) == 0 &&
                       g_uri_get_port(actual) == g_uri_get_port(expected);
    if (actual) g_uri_unref(actual);
    if (expected) g_uri_unref(expected);
    return matches;
}

static CodexBarHttpResponse *send_request(const CodexBarHttpRequest *request,
                                          CodexBarWebProviders3Transport transport,
                                          GError **error) {
    if (request->cancellable && g_cancellable_set_error_if_cancelled(request->cancellable, error)) return NULL;
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

static gboolean require_response(const CodexBarHttpResponse *response,
                                 const char *provider,
                                 const char *expected_url,
                                 GError **error) {
    if (!response_origin_is(response, expected_url)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "%s response left its allowed HTTPS origin", provider);
        return FALSE;
    }
    if (response->status == 200) return TRUE;
    if (response->status == 401 || response->status == 403 ||
        (response->status >= 300 && response->status < 400)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                    "%s credentials are invalid or expired", provider);
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED,
                    "%s API returned HTTP %ld", provider, response->status);
    }
    return FALSE;
}

static CodexBarProvider *new_web_provider(const char *id, gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup(id);
    provider->source = g_strdup("web");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));
    return provider;
}

static CodexBarQuotaWindow *new_window(const char *id,
                                       const char *title,
                                       double used_percent,
                                       gint64 window_minutes,
                                       gint64 resets_at_ms,
                                       gboolean has_reset) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = CLAMP(used_percent, 0.0, 100.0);
    if (window_minutes > 0) {
        window->has_window_minutes = TRUE;
        window->window_minutes = window_minutes;
    }
    window->has_resets_at = has_reset;
    window->resets_at_ms = resets_at_ms;
    return window;
}

static char *regex_capture(const char *text, size_t length, const char *pattern) {
    if (!text || length == 0 || length > WEB3_MAXIMUM_RESPONSE_BYTES ||
        !g_utf8_validate(text, (gssize)length, NULL) || memchr(text, '\0', length)) {
        return NULL;
    }
    GError *regex_error = NULL;
    GRegex *regex = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_DOTALL, 0, &regex_error);
    if (!regex) {
        g_clear_error(&regex_error);
        return NULL;
    }
    GMatchInfo *match = NULL;
    gboolean found = g_regex_match_full(regex, text, (gssize)length, 0, 0, &match, NULL);
    char *result = found ? g_match_info_fetch(match, 1) : NULL;
    if (match) g_match_info_free(match);
    g_regex_unref(regex);
    if (result) strip_unicode_whitespace(result);
    return result;
}

/* Sakana AI */

static const char *const sakana_environment_keys[] = {"SAKANA_COOKIE", "sakana_cookie", NULL};

gboolean codexbar_sakana_has_auth(const CodexBarProviderConfig *config) {
    char *cookie = provider_cookie(config, sakana_environment_keys);
    g_free(cookie);
    return cookie != NULL;
}

static gboolean parse_sakana_reset(const char *value, gint64 *result) {
    static const char *const months[] = {"January", "February", "March", "April", "May", "June",
                                         "July", "August", "September", "October", "November", "December"};
    char month_name[16] = {0};
    char meridiem[3] = {0};
    int day = 0, year = 0, hour = 0, minute = 0;
    if (!value || sscanf(value, "%15s %d, %d at %d:%d %2s", month_name, &day, &year, &hour, &minute,
                         meridiem) != 6) {
        return FALSE;
    }
    int month = 0;
    for (size_t index = 0; index < G_N_ELEMENTS(months); index++) {
        if (g_ascii_strcasecmp(month_name, months[index]) == 0) month = (int)index + 1;
    }
    if (month == 0 || day < 1 || day > 31 || year < 1970 || hour < 1 || hour > 12 ||
        minute < 0 || minute > 59 || (g_ascii_strcasecmp(meridiem, "AM") != 0 &&
                                      g_ascii_strcasecmp(meridiem, "PM") != 0)) {
        return FALSE;
    }
    if (hour == 12) hour = 0;
    if (g_ascii_strcasecmp(meridiem, "PM") == 0) hour += 12;
    GDateTime *date = g_date_time_new_utc(year, month, day, hour, minute, 0);
    if (!date) return FALSE;
    *result = g_date_time_to_unix(date) * 1000;
    g_date_time_unref(date);
    return TRUE;
}

static gboolean sakana_window(const char *html,
                              size_t length,
                              const char *label,
                              const char *id,
                              const char *title,
                              gint64 minutes,
                              CodexBarQuotaWindow **result,
                              GError **error) {
    char *escaped = g_regex_escape_string(label, -1);
    char *pattern = g_strdup_printf(
        "<p[^>]*>\\s*%s\\s*</p>((?:(?!<p[^>]*>\\s*(?:5-hour|Weekly)\\s*</p>|"
        "<div[^>]*data-slot=[\\\"'](?:card|card-title)[\\\"'][^>]*>)[\\s\\S])*)",
        escaped);
    char *body = regex_capture(html, length, pattern);
    g_free(pattern);
    g_free(escaped);
    if (!body) {
        *result = NULL;
        return TRUE;
    }
    char *percent_text = regex_capture(body, strlen(body),
                                       "<p[^>]*>\\s*([0-9]+(?:\\.[0-9]+)?)% used\\s*</p>");
    char *end = NULL;
    double percent = percent_text ? g_ascii_strtod(percent_text, &end) : NAN;
    if (!percent_text || !end || *end != '\0' || !isfinite(percent) || percent < 0 || percent > 100) {
        g_free(percent_text);
        g_free(body);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Sakana %s usage percentage is invalid", label);
        return FALSE;
    }
    char *reset = regex_capture(body, strlen(body), "<p[^>]*>\\s*Resets on ([^<]+?)\\s*</p>");
    gint64 reset_ms = 0;
    gboolean has_reset = parse_sakana_reset(reset, &reset_ms);
    *result = new_window(id, title, percent, minutes, reset_ms, has_reset);
    g_free(reset);
    g_free(percent_text);
    g_free(body);
    return TRUE;
}

CodexBarProvider *codexbar_sakana_parse_billing_html(const char *html,
                                                      size_t length,
                                                      gint64 now_ms,
                                                      GError **error) {
    if (!html || length == 0 || length > WEB3_MAXIMUM_RESPONSE_BYTES ||
        !g_utf8_validate(html, (gssize)length, NULL) || memchr(html, '\0', length)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Sakana billing response is invalid text");
        return NULL;
    }
    CodexBarQuotaWindow *five_hour = NULL;
    CodexBarQuotaWindow *weekly = NULL;
    if (!sakana_window(html, length, "5-hour", "primary", "5-hour", 300, &five_hour, error) ||
        !sakana_window(html, length, "Weekly", "secondary", "Weekly", 10080, &weekly, error)) {
        codexbar_quota_window_free(five_hour);
        codexbar_quota_window_free(weekly);
        return NULL;
    }
    if (!five_hour && !weekly) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Sakana usage limit windows were not found");
        return NULL;
    }
    CodexBarProvider *provider = new_web_provider("sakana", now_ms);
    provider->dashboard_url = g_strdup(SAKANA_BILLING_URL);
    provider->explicit_quota_slots = TRUE;
    if (five_hour) codexbar_provider_add_quota_window(provider, five_hour);
    if (weekly) codexbar_provider_add_quota_window(provider, weekly);
    char *plan = regex_capture(html, length,
                               "<div[^>]*data-slot=[\\\"']card-title[\\\"'][^>]*>"
                               "[\\s\\S]*?<span>\\s*([^<]+?)\\s*</span>");
    char *price = regex_capture(html, length,
                                "<div[^>]*data-slot=[\\\"']card-title[\\\"'][^>]*>"
                                "[\\s\\S]*?<span>[^<]+</span>\\s*<span[^>]*>\\s*([^<]+?)\\s*</span>");
    if (plan || price) {
        provider->plan = plan && price ? g_strdup_printf("%s %s", plan, price) : g_strdup(plan ? plan : price);
        provider->identity = g_new0(CodexBarProviderIdentity, 1);
        provider->identity->login_method = g_strdup(provider->plan);
    }
    g_free(plan);
    g_free(price);
    return provider;
}

static char *strip_html_comments(const char *raw) {
    GRegex *comments = g_regex_new("<!--.*?-->", G_REGEX_DOTALL, 0, NULL);
    char *without_comments = g_regex_replace_literal(comments, raw, -1, 0, "", 0, NULL);
    g_regex_unref(comments);
    GRegex *whitespace = g_regex_new("\\s+", 0, 0, NULL);
    char *collapsed = g_regex_replace_literal(whitespace, without_comments, -1, 0, " ", 0, NULL);
    g_regex_unref(whitespace);
    g_free(without_comments);
    g_strstrip(collapsed);
    return collapsed;
}

gboolean codexbar_sakana_apply_payg_html(CodexBarProvider *provider,
                                         const char *html,
                                         size_t length,
                                         GError **error) {
    g_return_val_if_fail(provider != NULL, FALSE);
    char *balance_text = regex_capture(
        html, length,
        "<h2[^>]*>\\s*Credit balance\\s*</h2>[\\s\\S]{0,900}?"
        "<p[^>]*tabular-nums[^\\\"]*[\\\"][^>]*>\\$?([0-9][0-9,]*(?:\\.[0-9]+)?)</p>");
    if (!balance_text) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Sakana pay-as-you-go credit balance was not found");
        return FALSE;
    }
    GString *digits = g_string_new(NULL);
    for (const char *cursor = balance_text; *cursor; cursor++) {
        if (*cursor != ',') g_string_append_c(digits, *cursor);
    }
    char *end = NULL;
    double balance = g_ascii_strtod(digits->str, &end);
    gboolean valid = end && *end == '\0' && isfinite(balance);
    g_string_free(digits, TRUE);
    g_free(balance_text);
    if (!valid) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Sakana pay-as-you-go credit balance is invalid");
        return FALSE;
    }
    CodexBarBalance *credit = codexbar_balance_new("pay-as-you-go", "Pay as you go", balance, "USD");
    credit->has_updated_at = provider->has_updated_at;
    credit->updated_at_ms = provider->updated_at_ms;
    codexbar_provider_add_balance(provider, credit);

    char *usage_text = regex_capture(
        html, length,
        "<h2[^>]*>\\s*Usage\\s*</h2>\\s*<span[^>]*>\\s*Total(?:<!--\\s*-->)?:\\s*"
        "(?:<!--\\s*-->)?\\$?([0-9][0-9,]*(?:\\.[0-9]+)?)\\s*</span>");
    char *period_raw = regex_capture(html, length,
                                     "aria-label=[\\\"']Usage date range[\\\"'][^>]*>"
                                     "([\\s\\S]*?)</button>");
    json_object *payg = json_object_new_object();
    json_object_object_add(payg, "creditBalance", json_object_new_double(balance));
    if (usage_text) {
        GString *usage_digits = g_string_new(NULL);
        for (const char *cursor = usage_text; *cursor; cursor++) {
            if (*cursor != ',') g_string_append_c(usage_digits, *cursor);
        }
        double usage = g_ascii_strtod(usage_digits->str, &end);
        if (end && *end == '\0' && isfinite(usage)) {
            json_object_object_add(payg, "periodUsageTotal", json_object_new_double(usage));
        }
        g_string_free(usage_digits, TRUE);
    }
    if (period_raw) {
        char *period = strip_html_comments(period_raw);
        if (period[0]) json_object_object_add(payg, "periodLabel", json_object_new_string(period));
        g_free(period);
    }
    json_object_object_add(provider->usage_extensions, "sakanaPayAsYouGo", payg);
    g_free(usage_text);
    g_free(period_raw);
    return TRUE;
}

CodexBarProvider *codexbar_sakana_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders3Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *cookie = provider_cookie(config, sakana_environment_keys);
    if (!cookie) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "No Sakana cookie header is configured");
        return NULL;
    }
    CodexBarHttpRequestHeader headers[] = {
        {"Accept", "text/html,application/xhtml+xml"},
        {"Accept-Language", "en-US,en;q=0.9"},
        {"Cookie", cookie},
    };
    CodexBarHttpRequest request = {
        .url = SAKANA_BILLING_URL,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = WEB3_TIMEOUT_SECONDS,
        .maximum_response_bytes = WEB3_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = send_request(&request, transport, error);
    if (!response || !require_response(response, "Sakana", SAKANA_BILLING_URL, error)) {
        codexbar_http_response_free(response);
        g_free(cookie);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_sakana_parse_billing_html(
        response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    if (!provider) {
        g_free(cookie);
        return NULL;
    }

    request.url = SAKANA_PAYG_URL;
    request.timeout_seconds = WEB3_OPTIONAL_TIMEOUT_SECONDS;
    GError *optional_error = NULL;
    response = send_request(&request, transport, &optional_error);
    if (response && require_response(response, "Sakana", SAKANA_BILLING_URL, &optional_error)) {
        GError *parse_error = NULL;
        codexbar_sakana_apply_payg_html(provider, response->body, response->body_length, &parse_error);
        g_clear_error(&parse_error);
    }
    if (optional_error && g_error_matches(optional_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_propagate_error(error, optional_error);
        codexbar_http_response_free(response);
        codexbar_provider_free(provider);
        g_free(cookie);
        return NULL;
    }
    g_clear_error(&optional_error);
    codexbar_http_response_free(response);
    g_free(cookie);
    return provider;
}

CodexBarProvider *codexbar_sakana_fetch_with_transport(const CodexBarProviderConfig *config,
                                                       CodexBarWebProviders3Transport transport,
                                                       gint64 now_ms,
                                                       GError **error) {
    return codexbar_sakana_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_sakana_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error) {
    return codexbar_sakana_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_sakana_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_sakana_fetch_with_cancellable(config, NULL, error);
}

/* Abacus AI */

static const char *const abacus_environment_keys[] = {"ABACUS_COOKIE", "abacus_cookie", NULL};

gboolean codexbar_abacus_has_auth(const CodexBarProviderConfig *config) {
    char *cookie = provider_cookie(config, abacus_environment_keys);
    g_free(cookie);
    return cookie != NULL;
}

static json_object *abacus_result(const char *json,
                                  size_t length,
                                  const char *label,
                                  gboolean optional,
                                  GError **error) {
    if (optional && (!json || length == 0)) return json_object_new_object();
    json_object *root = parse_json_document(json, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        if (optional) return json_object_new_object();
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Abacus %s response is invalid JSON", label);
        return NULL;
    }
    json_object *success = object_member(root, "success");
    json_object *result = object_member(root, "result");
    if (!success || !json_object_is_type(success, json_type_boolean) ||
        !json_object_get_boolean(success) || !result || !json_object_is_type(result, json_type_object)) {
        char *message = json_clean_string(root, "error");
        char *lower = message ? g_ascii_strdown(message, -1) : NULL;
        gboolean auth = lower && (strstr(lower, "expired") || strstr(lower, "session") ||
                                  strstr(lower, "login") || strstr(lower, "authenticate") ||
                                  strstr(lower, "unauthorized") || strstr(lower, "unauthenticated") ||
                                  strstr(lower, "forbidden"));
        g_free(lower);
        g_free(message);
        json_object_put(root);
        if (optional && !auth) return json_object_new_object();
        g_set_error(error, G_IO_ERROR,
                    auth ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_INVALID_DATA,
                    "Abacus %s response was rejected", label);
        return NULL;
    }
    json_object_get(result);
    json_object_put(root);
    return result;
}

static char *format_abacus_credits(double value) {
    char *plain = fabs(value - round(value)) < 0.0000001
                      ? g_strdup_printf("%.0f", value)
                      : g_strdup_printf("%.1f", value);
    gboolean negative = plain[0] == '-';
    char *decimal = strchr(plain, '.');
    size_t integer_length = decimal ? (size_t)(decimal - plain) : strlen(plain);
    const char *digits = plain + (negative ? 1 : 0);
    size_t digit_count = integer_length - (negative ? 1U : 0U);
    GString *result = g_string_new(negative ? "-" : "");
    for (size_t index = 0; index < digit_count; index++) {
        if (index > 0 && (digit_count - index) % 3 == 0) g_string_append_c(result, ',');
        g_string_append_c(result, digits[index]);
    }
    if (decimal) g_string_append(result, decimal);
    g_free(plain);
    return g_string_free(result, FALSE);
}

CodexBarProvider *codexbar_abacus_parse_results(const char *compute_json,
                                                size_t compute_length,
                                                const char *billing_json,
                                                size_t billing_length,
                                                gint64 now_ms,
                                                GError **error) {
    json_object *compute = abacus_result(compute_json, compute_length, "compute points", FALSE, error);
    if (!compute) return NULL;
    GError *billing_error = NULL;
    json_object *billing = abacus_result(
        billing_json, billing_length, "billing info", TRUE, &billing_error);
    g_clear_error(&billing_error);
    if (!billing) billing = json_object_new_object();
    double total = 0, left = 0;
    if (!object_number(compute, "totalComputePoints", &total) ||
        !object_number(compute, "computePointsLeft", &left) || total < 0 || left < 0) {
        json_object_put(compute);
        json_object_put(billing);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Abacus compute points response is missing valid credit fields");
        return NULL;
    }
    double used = total - left;
    if (!isfinite(used)) {
        json_object_put(compute);
        json_object_put(billing);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Abacus credit usage is invalid");
        return NULL;
    }
    char *reset = json_clean_string(billing, "nextBillingDate");
    gint64 reset_ms = 0;
    gboolean has_reset = parse_timestamp_string(reset, &reset_ms);
    char *plan = json_clean_string(billing, "currentTier");

    CodexBarProvider *provider = new_web_provider("abacus", now_ms);
    provider->dashboard_url = g_strdup("https://apps.abacus.ai/chatllm/admin/compute-points-usage");
    provider->explicit_quota_slots = TRUE;
    char *used_text = format_abacus_credits(used);
    char *total_text = format_abacus_credits(total);
    char *detail = g_strdup_printf("%s / %s credits", used_text, total_text);
    gint64 window_minutes = 43200;
    if (has_reset) {
        GDateTime *reset_date = g_date_time_new_from_unix_utc(reset_ms / 1000);
        GDateTime *cycle_start = reset_date ? g_date_time_add_months(reset_date, -1) : NULL;
        if (reset_date && cycle_start) {
            window_minutes = MAX(G_GINT64_CONSTANT(1), g_date_time_difference(reset_date, cycle_start) /
                                                         G_TIME_SPAN_MINUTE);
        }
        if (cycle_start) g_date_time_unref(cycle_start);
        if (reset_date) g_date_time_unref(reset_date);
    }
    CodexBarQuotaWindow *window = new_window(
        "primary", "Credits", total > 0 ? used / total * 100.0 : 0.0,
        window_minutes, reset_ms, has_reset);
    window->detail = detail;
    codexbar_provider_add_quota_window(provider, window);
    provider->plan = g_strdup(plan);
    if (plan) {
        provider->identity = g_new0(CodexBarProviderIdentity, 1);
        provider->identity->login_method = g_strdup(plan);
    }
    CodexBarBalance *balance = codexbar_balance_new("compute-points", "Compute points", left, "credits");
    balance->has_used = TRUE;
    balance->used = used;
    balance->has_limit = TRUE;
    balance->limit = total;
    balance->has_resets_at = has_reset;
    balance->resets_at_ms = reset_ms;
    balance->has_updated_at = TRUE;
    balance->updated_at_ms = now_ms;
    codexbar_provider_add_balance(provider, balance);
    g_free(used_text);
    g_free(total_text);
    g_free(reset);
    g_free(plan);
    json_object_put(compute);
    json_object_put(billing);
    return provider;
}

static CodexBarHttpResponse *abacus_request(const char *url,
                                            const char *method,
                                            const char *cookie,
                                            long timeout,
                                            CodexBarWebProviders3Transport transport,
                                            GCancellable *cancellable,
                                            GError **error) {
    CodexBarHttpRequestHeader headers[] = {
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"Cookie", cookie},
    };
    static const char body[] = "{}";
    CodexBarHttpRequest request = {
        .url = url,
        .method = method,
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .body = g_str_equal(method, "POST") ? body : NULL,
        .body_length = g_str_equal(method, "POST") ? sizeof(body) - 1 : 0,
        .timeout_seconds = timeout,
        .maximum_response_bytes = WEB3_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    return send_request(&request, transport, error);
}

CodexBarProvider *codexbar_abacus_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders3Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *cookie = provider_cookie(config, abacus_environment_keys);
    if (!cookie) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "No Abacus cookie header is configured");
        return NULL;
    }
    CodexBarHttpResponse *compute = abacus_request(
        ABACUS_COMPUTE_URL, "GET", cookie, WEB3_TIMEOUT_SECONDS, transport, cancellable, error);
    if (!compute || !require_response(compute, "Abacus", ABACUS_COMPUTE_URL, error)) {
        codexbar_http_response_free(compute);
        g_free(cookie);
        return NULL;
    }
    GError *optional_error = NULL;
    CodexBarHttpResponse *billing = abacus_request(
        ABACUS_BILLING_URL, "POST", cookie, WEB3_OPTIONAL_TIMEOUT_SECONDS,
        transport, cancellable, &optional_error);
    if (billing && !require_response(billing, "Abacus", ABACUS_BILLING_URL, &optional_error)) {
        codexbar_http_response_free(billing);
        billing = NULL;
    }
    if (optional_error && g_error_matches(optional_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_propagate_error(error, optional_error);
        codexbar_http_response_free(compute);
        codexbar_http_response_free(billing);
        g_free(cookie);
        return NULL;
    }
    g_clear_error(&optional_error);
    CodexBarProvider *provider = codexbar_abacus_parse_results(
        compute->body, compute->body_length,
        billing ? billing->body : NULL, billing ? billing->body_length : 0,
        now_ms, error);
    codexbar_http_response_free(compute);
    codexbar_http_response_free(billing);
    g_free(cookie);
    return provider;
}

CodexBarProvider *codexbar_abacus_fetch_with_transport(const CodexBarProviderConfig *config,
                                                       CodexBarWebProviders3Transport transport,
                                                       gint64 now_ms,
                                                       GError **error) {
    return codexbar_abacus_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_abacus_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error) {
    return codexbar_abacus_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_abacus_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_abacus_fetch_with_cancellable(config, NULL, error);
}

/* Mistral */

static const char *const mistral_environment_keys[] = {"MISTRAL_COOKIE", "mistral_cookie", NULL};

static gboolean cookie_has_prefix(const char *cookie, const char *prefix) {
    char **pairs = g_strsplit(cookie, ";", -1);
    gboolean found = FALSE;
    for (size_t index = 0; pairs[index] && !found; index++) {
        char *pair = g_strstrip(pairs[index]);
        found = g_str_has_prefix(pair, prefix) && strchr(pair, '=') != NULL;
    }
    g_strfreev(pairs);
    return found;
}

static char *mistral_cookie(const CodexBarProviderConfig *config) {
    char *cookie = provider_cookie(config, mistral_environment_keys);
    if (cookie && !cookie_has_prefix(cookie, "ory_session_")) {
        g_free(cookie);
        return NULL;
    }
    return cookie;
}

gboolean codexbar_mistral_has_auth(const CodexBarProviderConfig *config) {
    char *cookie = mistral_cookie(config);
    g_free(cookie);
    return cookie != NULL;
}

static char *mistral_csrf_token(const char *cookie) {
    char **pairs = g_strsplit(cookie, ";", -1);
    char *result = NULL;
    for (size_t index = 0; pairs[index] && !result; index++) {
        char *pair = g_strstrip(pairs[index]);
        char *separator = strchr(pair, '=');
        if (!separator) continue;
        *separator = '\0';
        char *name = g_strstrip(pair);
        char *value = g_strstrip(separator + 1);
        if (g_str_equal(name, "csrftoken") && safe_cookie_value(value)) result = g_strdup(value);
    }
    g_strfreev(pairs);
    return result;
}

char *codexbar_mistral_console_cookie_header(const char *cookie_header, GError **error) {
    char *cookie = normalize_cookie_header(cookie_header);
    if (!cookie) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Mistral cookie header is invalid");
        return NULL;
    }
    char *csrf = mistral_csrf_token(cookie);
    if (!csrf || strchr(csrf, ';') || strchr(csrf, ',') || strchr(csrf, '\r') || strchr(csrf, '\n')) {
        g_free(cookie);
        g_free(csrf);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Mistral CSRF token is missing or invalid");
        return NULL;
    }
    GString *result = g_string_new(NULL);
    g_string_append_printf(result, "csrftoken=%s", csrf);
    char **pairs = g_strsplit(cookie, ";", -1);
    for (size_t index = 0; pairs[index]; index++) {
        char *pair = g_strstrip(pairs[index]);
        if (g_str_has_prefix(pair, "ory_session_") && strchr(pair, '=')) {
            g_string_append_printf(result, "; %s", pair);
        }
    }
    g_strfreev(pairs);
    g_free(cookie);
    g_free(csrf);
    return g_string_free(result, FALSE);
}

typedef struct {
    gint64 input;
    gint64 output;
    gint64 cached;
    double cost;
} MistralDaily;

typedef struct {
    GHashTable *prices;
    GHashTable *daily;
    gint64 input;
    gint64 output;
    gint64 cached;
    gint64 model_count;
    double cost;
} MistralAggregate;

static gboolean checked_add_i64(gint64 *total, gint64 value) {
    if ((value > 0 && *total > G_MAXINT64 - value) || (value < 0 && *total < G_MININT64 - value)) return FALSE;
    *total += value;
    return TRUE;
}

static void finite_cost_add(double *total, double value) {
    if (!isfinite(value)) return;
    double updated = *total + value;
    if (isfinite(updated)) *total = updated;
}

static GHashTable *mistral_price_index(json_object *root) {
    GHashTable *prices = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    json_object *array = object_member(root, "prices");
    if (!array || !json_object_is_type(array, json_type_array)) return prices;
    size_t count = json_object_array_length(array);
    for (size_t index = 0; index < count; index++) {
        json_object *price = json_object_array_get_idx(array, index);
        char *metric = json_clean_string(price, "billing_metric");
        char *group = json_clean_string(price, "billing_group");
        double value = 0;
        if (metric && group && object_number(price, "price", &value)) {
            char *key = g_strdup_printf("%s::%s", metric, group);
            double *stored = g_new(double, 1);
            *stored = value;
            g_hash_table_replace(prices, key, stored);
        }
        g_free(metric);
        g_free(group);
    }
    return prices;
}

static gint64 mistral_entry_units(json_object *entry, gboolean *valid) {
    double number = 0;
    *valid = object_number(entry, "value_paid", &number) || object_number(entry, "value", &number);
    if (!*valid || number < 0 || number > (double)G_MAXINT64 || trunc(number) != number) {
        *valid = FALSE;
        return 0;
    }
    return (gint64)number;
}

static double mistral_entry_cost(json_object *entry, gint64 units, GHashTable *prices) {
    char *metric = json_clean_string(entry, "billing_metric");
    char *group = json_clean_string(entry, "billing_group");
    double result = 0;
    if (metric && group) {
        char *key = g_strdup_printf("%s::%s", metric, group);
        double *price = g_hash_table_lookup(prices, key);
        if (price) {
            double cost = (double)units * *price;
            if (isfinite(cost)) result = cost;
        }
        g_free(key);
    }
    g_free(metric);
    g_free(group);
    return result;
}

static MistralDaily *mistral_daily_bucket(MistralAggregate *aggregate, const char *timestamp) {
    if (!timestamp || strlen(timestamp) < 10) return NULL;
    for (size_t index = 0; index < 10; index++) {
        if ((index == 4 || index == 7) ? timestamp[index] != '-' : !g_ascii_isdigit(timestamp[index])) return NULL;
    }
    char *day = g_strndup(timestamp, 10);
    MistralDaily *bucket = g_hash_table_lookup(aggregate->daily, day);
    if (!bucket) {
        bucket = g_new0(MistralDaily, 1);
        g_hash_table_insert(aggregate->daily, day, bucket);
    } else {
        g_free(day);
    }
    return bucket;
}

typedef enum {
    MISTRAL_INPUT,
    MISTRAL_OUTPUT,
    MISTRAL_CACHED,
} MistralTokenKind;

static gboolean mistral_entries(MistralAggregate *aggregate,
                                json_object *entries,
                                MistralTokenKind kind,
                                gboolean counts_tokens,
                                GError **error) {
    if (!entries || json_object_is_type(entries, json_type_null)) return TRUE;
    if (!json_object_is_type(entries, json_type_array)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Mistral usage entries must be arrays");
        return FALSE;
    }
    size_t count = json_object_array_length(entries);
    for (size_t index = 0; index < count; index++) {
        json_object *entry = json_object_array_get_idx(entries, index);
        if (!entry || !json_object_is_type(entry, json_type_object)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Mistral usage entry is invalid");
            return FALSE;
        }
        gboolean valid_units = FALSE;
        gint64 units = mistral_entry_units(entry, &valid_units);
        if (!valid_units) {
            if (object_member(entry, "value_paid") || object_member(entry, "value")) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                    "Mistral usage entry has an invalid unit count");
                return FALSE;
            }
            continue;
        }
        double cost = mistral_entry_cost(entry, units, aggregate->prices);
        finite_cost_add(&aggregate->cost, cost);
        char *timestamp = json_clean_string(entry, "timestamp");
        MistralDaily *daily = mistral_daily_bucket(aggregate, timestamp);
        if (daily) finite_cost_add(&daily->cost, cost);
        g_free(timestamp);
        if (!counts_tokens) continue;
        gint64 *total = kind == MISTRAL_INPUT ? &aggregate->input
                       : kind == MISTRAL_OUTPUT ? &aggregate->output
                                                : &aggregate->cached;
        gint64 *daily_total = !daily ? NULL
                            : kind == MISTRAL_INPUT ? &daily->input
                            : kind == MISTRAL_OUTPUT ? &daily->output
                                                     : &daily->cached;
        if (!checked_add_i64(total, units) || (daily_total && !checked_add_i64(daily_total, units))) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Mistral token total exceeds the supported range");
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean mistral_model_data(MistralAggregate *aggregate,
                                   json_object *data,
                                   gboolean counts_tokens,
                                   GError **error) {
    if (!data || !json_object_is_type(data, json_type_object)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Mistral model usage is invalid");
        return FALSE;
    }
    return mistral_entries(aggregate, object_member(data, "input"), MISTRAL_INPUT, counts_tokens, error) &&
           mistral_entries(aggregate, object_member(data, "output"), MISTRAL_OUTPUT, counts_tokens, error) &&
           mistral_entries(aggregate, object_member(data, "cached"), MISTRAL_CACHED, counts_tokens, error);
}

static gboolean mistral_models(MistralAggregate *aggregate,
                               json_object *models,
                               gboolean counts_tokens,
                               gboolean count_models,
                               GError **error) {
    if (!models || json_object_is_type(models, json_type_null)) return TRUE;
    if (!json_object_is_type(models, json_type_object)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Mistral models payload is invalid");
        return FALSE;
    }
    json_object_object_foreach(models, name, data) {
        (void)name;
        if (count_models && !checked_add_i64(&aggregate->model_count, 1)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Mistral model count exceeds the supported range");
            return FALSE;
        }
        if (!mistral_model_data(aggregate, data, counts_tokens, error)) return FALSE;
    }
    return TRUE;
}

static gboolean mistral_category(MistralAggregate *aggregate,
                                 json_object *root,
                                 const char *key,
                                 gboolean counts_tokens,
                                 gboolean count_models,
                                 GError **error) {
    json_object *category = object_member(root, key);
    if (!category || json_object_is_type(category, json_type_null)) return TRUE;
    if (!json_object_is_type(category, json_type_object)) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                    "Mistral %s category is invalid", key);
        return FALSE;
    }
    return mistral_models(aggregate, object_member(category, "models"),
                          counts_tokens, count_models, error);
}

static gint compare_strings(gconstpointer left, gconstpointer right) {
    return g_strcmp0(left, right);
}

static json_object *mistral_daily_json(GHashTable *daily) {
    GList *keys = g_hash_table_get_keys(daily);
    keys = g_list_sort(keys, compare_strings);
    json_object *array = json_object_new_array();
    for (GList *node = keys; node; node = node->next) {
        const char *day = node->data;
        MistralDaily *bucket = g_hash_table_lookup(daily, day);
        json_object *item = json_object_new_object();
        json_object_object_add(item, "day", json_object_new_string(day));
        json_object_object_add(item, "cost", json_object_new_double(bucket->cost));
        json_object_object_add(item, "inputTokens", json_object_new_int64(bucket->input));
        json_object_object_add(item, "cachedTokens", json_object_new_int64(bucket->cached));
        json_object_object_add(item, "outputTokens", json_object_new_int64(bucket->output));
        json_object_object_add(item, "models", json_object_new_array());
        json_object_array_add(array, item);
    }
    g_list_free(keys);
    return array;
}

CodexBarProvider *codexbar_mistral_parse_usage(const char *json,
                                               size_t length,
                                               gint64 now_ms,
                                               GError **error) {
    json_object *root = parse_json_document(json, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Mistral usage response is invalid JSON");
        return NULL;
    }
    MistralAggregate aggregate = {
        .prices = mistral_price_index(root),
        .daily = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free),
    };
    gboolean valid = mistral_category(&aggregate, root, "completion", TRUE, TRUE, error) &&
                     mistral_category(&aggregate, root, "ocr", FALSE, FALSE, error) &&
                     mistral_category(&aggregate, root, "connectors", FALSE, FALSE, error) &&
                     mistral_category(&aggregate, root, "audio", FALSE, FALSE, error);
    json_object *libraries = object_member(root, "libraries_api");
    if (valid && libraries && !json_object_is_type(libraries, json_type_object)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Mistral libraries API category is invalid");
        valid = FALSE;
    }
    if (valid && libraries) {
        valid = mistral_category(&aggregate, libraries, "pages", FALSE, FALSE, error) &&
                mistral_category(&aggregate, libraries, "tokens", TRUE, FALSE, error);
    }
    json_object *fine_tuning = object_member(root, "fine_tuning");
    if (valid && fine_tuning && !json_object_is_type(fine_tuning, json_type_object)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Mistral fine tuning category is invalid");
        valid = FALSE;
    }
    if (valid && fine_tuning) {
        valid = mistral_models(&aggregate, object_member(fine_tuning, "training"), FALSE, FALSE, error) &&
                mistral_models(&aggregate, object_member(fine_tuning, "storage"), FALSE, FALSE, error);
    }
    if (!valid) {
        g_hash_table_unref(aggregate.prices);
        g_hash_table_unref(aggregate.daily);
        json_object_put(root);
        return NULL;
    }

    char *currency = json_clean_string(root, "currency");
    char *symbol = json_clean_string(root, "currency_symbol");
    if (!currency) currency = g_strdup("XXX");
    if (!symbol) symbol = g_strdup("¤");
    CodexBarProvider *provider = new_web_provider("mistral", now_ms);
    provider->dashboard_url = g_strdup("https://admin.mistral.ai/organization/usage");
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = g_strdup_printf(
        "API spend: %s%.4f this month", symbol, MAX(aggregate.cost, 0.0));
    provider->token_cost = g_new0(CodexBarTokenCost, 1);
    provider->token_cost->has_last_days_tokens = TRUE;
    provider->token_cost->last_days_tokens = aggregate.input + aggregate.cached + aggregate.output;
    provider->token_cost->has_last_days_cost = TRUE;
    provider->token_cost->last_days_cost = MAX(aggregate.cost, 0.0);
    provider->token_cost->currency = g_strdup(currency);
    provider->token_cost->history_label = g_strdup("This month");
    provider->token_cost->has_history_days = TRUE;
    provider->token_cost->history_days = CLAMP((gint64)g_hash_table_size(aggregate.daily), 1, 365);
    provider->token_cost->has_updated_at = TRUE;
    provider->token_cost->updated_at_ms = now_ms;

    json_object *snapshot = json_object_new_object();
    json_object_object_add(snapshot, "totalCost", json_object_new_double(aggregate.cost));
    json_object_object_add(snapshot, "currency", json_object_new_string(currency));
    json_object_object_add(snapshot, "currencySymbol", json_object_new_string(symbol));
    json_object_object_add(snapshot, "totalInputTokens", json_object_new_int64(aggregate.input));
    json_object_object_add(snapshot, "totalOutputTokens", json_object_new_int64(aggregate.output));
    json_object_object_add(snapshot, "totalCachedTokens", json_object_new_int64(aggregate.cached));
    json_object_object_add(snapshot, "modelCount", json_object_new_int64(aggregate.model_count));
    json_object_object_add(snapshot, "daily", mistral_daily_json(aggregate.daily));
    char *start_date = json_clean_string(root, "start_date");
    char *end_date = json_clean_string(root, "end_date");
    if (start_date) json_object_object_add(snapshot, "startDate", json_object_new_string(start_date));
    if (end_date) json_object_object_add(snapshot, "endDate", json_object_new_string(end_date));
    json_object_object_add(provider->usage_extensions, "mistralUsage", snapshot);
    g_free(start_date);
    g_free(end_date);
    g_free(currency);
    g_free(symbol);
    g_hash_table_unref(aggregate.prices);
    g_hash_table_unref(aggregate.daily);
    json_object_put(root);
    return provider;
}

gboolean codexbar_mistral_apply_credits(CodexBarProvider *provider,
                                       const char *json,
                                       size_t length,
                                       GError **error) {
    g_return_val_if_fail(provider != NULL, FALSE);
    json_object *root = parse_json_document(json, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Mistral credits response is invalid JSON");
        return FALSE;
    }
    double wallet = 0, notes = 0, ongoing = 0;
    gboolean valid = object_number(root, "wallet_amount", &wallet);
    if (object_member(root, "credit_notes_amount") &&
        !object_number(root, "credit_notes_amount", &notes)) {
        valid = FALSE;
    }
    if (object_member(root, "ongoing_usage_balance") &&
        !object_number(root, "ongoing_usage_balance", &ongoing)) {
        valid = FALSE;
    }
    char *currency = json_clean_string(root, "currency");
    double available = wallet + notes - ongoing;
    if (!valid || !currency || !isfinite(available)) {
        g_free(currency);
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Mistral credits response contains invalid amounts");
        return FALSE;
    }
    CodexBarBalance *balance = codexbar_balance_new(
        "credits", "Credits", MAX(available, 0.0), currency);
    balance->has_used = TRUE;
    balance->used = ongoing;
    balance->has_limit = TRUE;
    balance->limit = wallet + notes;
    balance->has_updated_at = provider->has_updated_at;
    balance->updated_at_ms = provider->updated_at_ms;
    codexbar_provider_add_balance(provider, balance);
    json_object *snapshot = object_member(provider->usage_extensions, "mistralUsage");
    if (snapshot) {
        json_object *credits = json_object_new_object();
        json_object_object_add(credits, "walletAmount", json_object_new_double(wallet));
        json_object_object_add(credits, "creditNotesAmount", json_object_new_double(notes));
        json_object_object_add(credits, "ongoingUsageBalance", json_object_new_double(ongoing));
        json_object_object_add(credits, "currency", json_object_new_string(currency));
        json_object_object_add(snapshot, "credits", credits);
    }
    g_free(currency);
    json_object_put(root);
    return TRUE;
}

gboolean codexbar_mistral_apply_vibe_usage(CodexBarProvider *provider,
                                          const char *json,
                                          size_t length,
                                          GError **error) {
    g_return_val_if_fail(provider != NULL, FALSE);
    json_object *root = parse_json_document(json, length);
    json_object *first = root && json_object_is_type(root, json_type_array) && json_object_array_length(root) > 0
                             ? json_object_array_get_idx(root, 0)
                             : NULL;
    json_object *payload = object_member(object_member(object_member(first, "result"), "data"), "json");
    double percent = 0;
    if (!payload || !json_object_is_type(payload, json_type_object) ||
        !object_number(payload, "usage_percentage", &percent) || percent < 0 || percent > 100) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Mistral Vibe usage response is invalid");
        return FALSE;
    }
    char *reset = json_clean_string(payload, "reset_at");
    gint64 reset_ms = 0;
    gboolean has_reset = parse_timestamp_string(reset, &reset_ms);
    codexbar_provider_add_quota_window(
        provider, new_window("mistral-monthly-plan", "Monthly Plan", percent, 0, reset_ms, has_reset));
    g_free(reset);
    json_object_put(root);
    return TRUE;
}

static CodexBarHttpResponse *mistral_request(const char *url,
                                             const char *cookie,
                                             const char *csrf,
                                             const char *referer,
                                             const char *origin,
                                             long timeout,
                                             CodexBarWebProviders3Transport transport,
                                             GCancellable *cancellable,
                                             GError **error) {
    CodexBarHttpRequestHeader headers[5] = {
        {"Accept", "*/*"},
        {"Cookie", cookie},
        {"Referer", referer},
        {"Origin", origin},
        {"X-CSRFTOKEN", csrf},
    };
    CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = csrf ? G_N_ELEMENTS(headers) : G_N_ELEMENTS(headers) - 1,
        .timeout_seconds = timeout,
        .maximum_response_bytes = WEB3_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    return send_request(&request, transport, error);
}

CodexBarProvider *codexbar_mistral_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarWebProviders3Transport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    char *cookie = mistral_cookie(config);
    if (!cookie) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND,
                            "No valid Mistral session cookie is configured");
        return NULL;
    }
    char *csrf = mistral_csrf_token(cookie);
    GDateTime *now = g_date_time_new_from_unix_utc(now_ms / 1000);
    if (!now) now = g_date_time_new_now_utc();
    int month = g_date_time_get_month(now);
    int year = g_date_time_get_year(now);
    g_date_time_unref(now);
    char *usage_url = g_strdup_printf(MISTRAL_USAGE_URL_FORMAT, month, year);
    CodexBarHttpResponse *response = mistral_request(
        usage_url, cookie, csrf, "https://admin.mistral.ai/organization/usage",
        "https://admin.mistral.ai", WEB3_TIMEOUT_SECONDS, transport, cancellable, error);
    if (!response || !require_response(response, "Mistral", MISTRAL_CREDITS_URL, error)) {
        codexbar_http_response_free(response);
        g_free(usage_url);
        g_free(csrf);
        g_free(cookie);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_mistral_parse_usage(
        response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    g_free(usage_url);
    if (!provider) {
        g_free(csrf);
        g_free(cookie);
        return NULL;
    }

    if (csrf) {
        GError *console_error = NULL;
        char *console_cookie = codexbar_mistral_console_cookie_header(cookie, &console_error);
        if (console_cookie) {
            response = mistral_request(
                MISTRAL_VIBE_URL, console_cookie, csrf, "https://console.mistral.ai/",
                "https://console.mistral.ai", WEB3_OPTIONAL_TIMEOUT_SECONDS,
                transport, cancellable, &console_error);
            if (response && require_response(response, "Mistral", MISTRAL_VIBE_URL, &console_error)) {
                GError *parse_error = NULL;
                codexbar_mistral_apply_vibe_usage(
                    provider, response->body, response->body_length, &parse_error);
                g_clear_error(&parse_error);
            }
            codexbar_http_response_free(response);
            g_free(console_cookie);
        }
        if (console_error && g_error_matches(console_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            g_propagate_error(error, console_error);
            codexbar_provider_free(provider);
            g_free(csrf);
            g_free(cookie);
            return NULL;
        }
        g_clear_error(&console_error);
    }

    GError *credits_error = NULL;
    response = mistral_request(
        MISTRAL_CREDITS_URL, cookie, csrf, "https://admin.mistral.ai/organization/billing",
        "https://admin.mistral.ai", WEB3_OPTIONAL_TIMEOUT_SECONDS,
        transport, cancellable, &credits_error);
    if (response && require_response(response, "Mistral", MISTRAL_CREDITS_URL, &credits_error)) {
        GError *parse_error = NULL;
        codexbar_mistral_apply_credits(provider, response->body, response->body_length, &parse_error);
        g_clear_error(&parse_error);
    }
    codexbar_http_response_free(response);
    if (credits_error && g_error_matches(credits_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_propagate_error(error, credits_error);
        codexbar_provider_free(provider);
        g_free(csrf);
        g_free(cookie);
        return NULL;
    }
    g_clear_error(&credits_error);
    g_free(csrf);
    g_free(cookie);
    return provider;
}

CodexBarProvider *codexbar_mistral_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarWebProviders3Transport transport,
                                                        gint64 now_ms,
                                                        GError **error) {
    return codexbar_mistral_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_mistral_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error) {
    return codexbar_mistral_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_mistral_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_mistral_fetch_with_cancellable(config, NULL, error);
}
