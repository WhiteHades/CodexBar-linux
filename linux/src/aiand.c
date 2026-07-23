#include "aiand.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define AIAND_LOGS_URL "https://api.aiand.com/logs"
#define AIAND_PAGE_LIMIT 100
#define AIAND_MAX_PAGES 10
#define AIAND_MAXIMUM_RESPONSE_BYTES (4U * 1024U * 1024U)

typedef struct {
    int sign;
    GString *digits;
    size_t scale;
} AiAndDecimal;

typedef struct {
    char *currency;
    AiAndDecimal total;
} AiAndSpend;

typedef struct {
    gboolean has_more;
    char *next_after;
    char *next_after_id;
} AiAndPage;

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

static char *clean_api_key(const char *raw) {
    if (!raw) return NULL;
    if (!g_utf8_validate(raw, -1, NULL)) return NULL;
    char *clean = g_strdup(raw);
    strip_unicode_whitespace(clean);
    size_t length = strlen(clean);
    if (length >= 1 && ((clean[0] == '\'' && clean[length - 1] == '\'') ||
                        (clean[0] == '"' && clean[length - 1] == '"'))) {
        clean[length - 1] = '\0';
        memmove(clean, clean + 1, length - 1);
        strip_unicode_whitespace(clean);
    }
    for (const char *cursor = clean; *cursor; cursor = g_utf8_next_char(cursor)) {
        if (g_unichar_iscntrl(g_utf8_get_char(cursor))) {
            g_free(clean);
            return NULL;
        }
    }
    if (clean[0] != '\0') return clean;
    g_free(clean);
    return NULL;
}

static char *resolve_api_key(const CodexBarProviderConfig *config) {
    char *key = clean_api_key(config ? config->api_key : NULL);
    if (!key) key = clean_api_key(g_getenv("AIAND_API_KEY"));
    return key;
}

gboolean codexbar_aiand_has_api_key(const CodexBarProviderConfig *config) {
    char *key = resolve_api_key(config);
    gboolean present = key != NULL;
    g_free(key);
    return present;
}

static gboolean json_whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

static json_object *parse_json_document(const char *json, size_t length) {
    if (!json || length > G_MAXINT || !g_utf8_validate(json, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
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

static void decimal_clear(AiAndDecimal *decimal) {
    if (decimal->digits) g_string_free(decimal->digits, TRUE);
    decimal->digits = NULL;
    decimal->sign = 0;
    decimal->scale = 0;
}

static void decimal_normalize(AiAndDecimal *decimal) {
    size_t leading = 0;
    while (leading + 1 < decimal->digits->len && decimal->digits->str[leading] == '0') leading++;
    if (leading > 0) g_string_erase(decimal->digits, 0, (gssize)leading);
    while (decimal->scale > 0 && decimal->digits->len > 1 &&
           decimal->digits->str[decimal->digits->len - 1] == '0') {
        g_string_truncate(decimal->digits, decimal->digits->len - 1);
        decimal->scale--;
    }
    gboolean zero = TRUE;
    for (size_t index = 0; index < decimal->digits->len; index++) {
        if (decimal->digits->str[index] != '0') zero = FALSE;
    }
    if (zero) {
        g_string_assign(decimal->digits, "0");
        decimal->sign = 0;
        decimal->scale = 0;
    }
}

static gboolean parse_decimal(const char *raw, size_t length, AiAndDecimal *result) {
    if (!raw || length == 0 || length > 128) return FALSE;
    size_t offset = 0;
    int sign = 1;
    if (raw[offset] == '+' || raw[offset] == '-') {
        sign = raw[offset] == '-' ? -1 : 1;
        if (++offset == length) return FALSE;
    }
    GString *digits = g_string_sized_new(length);
    gboolean has_digit = FALSE;
    while (offset < length && g_ascii_isdigit(raw[offset])) {
        has_digit = TRUE;
        g_string_append_c(digits, raw[offset++]);
    }
    size_t scale = 0;
    if (offset < length && raw[offset] == '.') {
        offset++;
        while (offset < length && g_ascii_isdigit(raw[offset])) {
            has_digit = TRUE;
            g_string_append_c(digits, raw[offset++]);
            scale++;
        }
    }
    if (!has_digit || offset != length) {
        g_string_free(digits, TRUE);
        return FALSE;
    }
    result->sign = sign;
    result->digits = digits;
    result->scale = scale;
    decimal_normalize(result);
    return TRUE;
}

static GString *decimal_add_magnitude(const char *left, const char *right) {
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    size_t length = MAX(left_length, right_length);
    GString *reversed = g_string_sized_new(length + 1);
    int carry = 0;
    for (size_t index = 0; index < length; index++) {
        int left_digit = index < left_length ? left[left_length - index - 1] - '0' : 0;
        int right_digit = index < right_length ? right[right_length - index - 1] - '0' : 0;
        int sum = left_digit + right_digit + carry;
        g_string_append_c(reversed, (char)('0' + sum % 10));
        carry = sum / 10;
    }
    if (carry) g_string_append_c(reversed, (char)('0' + carry));
    g_strreverse(reversed->str);
    return reversed;
}

static int decimal_compare_magnitude(const char *left, const char *right) {
    size_t left_length = strlen(left);
    size_t right_length = strlen(right);
    if (left_length != right_length) return left_length > right_length ? 1 : -1;
    int comparison = strcmp(left, right);
    return comparison > 0 ? 1 : comparison < 0 ? -1 : 0;
}

static GString *decimal_subtract_magnitude(const char *larger, const char *smaller) {
    size_t larger_length = strlen(larger);
    size_t smaller_length = strlen(smaller);
    GString *reversed = g_string_sized_new(larger_length);
    int borrow = 0;
    for (size_t index = 0; index < larger_length; index++) {
        int left_digit = larger[larger_length - index - 1] - '0' - borrow;
        int right_digit = index < smaller_length ? smaller[smaller_length - index - 1] - '0' : 0;
        if (left_digit < right_digit) {
            left_digit += 10;
            borrow = 1;
        } else {
            borrow = 0;
        }
        g_string_append_c(reversed, (char)('0' + left_digit - right_digit));
    }
    g_strreverse(reversed->str);
    return reversed;
}

static void decimal_add(AiAndDecimal *total, const AiAndDecimal *value) {
    if (value->sign == 0) return;
    if (total->sign == 0) {
        total->sign = value->sign;
        total->digits = g_string_new(value->digits->str);
        total->scale = value->scale;
        return;
    }
    size_t scale = MAX(total->scale, value->scale);
    GString *left = g_string_new(total->digits->str);
    GString *right = g_string_new(value->digits->str);
    for (size_t index = total->scale; index < scale; index++) g_string_append_c(left, '0');
    for (size_t index = value->scale; index < scale; index++) g_string_append_c(right, '0');

    GString *digits = NULL;
    int sign = total->sign;
    if (total->sign == value->sign) {
        digits = decimal_add_magnitude(left->str, right->str);
    } else {
        int comparison = decimal_compare_magnitude(left->str, right->str);
        if (comparison == 0) {
            digits = g_string_new("0");
            sign = 0;
        } else if (comparison > 0) {
            digits = decimal_subtract_magnitude(left->str, right->str);
        } else {
            digits = decimal_subtract_magnitude(right->str, left->str);
            sign = value->sign;
        }
    }
    g_string_free(left, TRUE);
    g_string_free(right, TRUE);
    g_string_free(total->digits, TRUE);
    total->digits = digits;
    total->scale = scale;
    total->sign = sign;
    decimal_normalize(total);
}

static gboolean decimal_to_double(const AiAndDecimal *decimal, double *result) {
    if (decimal->sign == 0) {
        *result = 0.0;
        return TRUE;
    }
    GString *text = g_string_new(decimal->sign < 0 ? "-" : "");
    if (decimal->scale == 0) {
        g_string_append(text, decimal->digits->str);
    } else if (decimal->digits->len <= decimal->scale) {
        g_string_append(text, "0.");
        for (size_t index = decimal->digits->len; index < decimal->scale; index++) g_string_append_c(text, '0');
        g_string_append(text, decimal->digits->str);
    } else {
        size_t whole = decimal->digits->len - decimal->scale;
        g_string_append_len(text, decimal->digits->str, (gssize)whole);
        g_string_append_c(text, '.');
        g_string_append(text, decimal->digits->str + whole);
    }
    char *end = NULL;
    double value = g_ascii_strtod(text->str, &end);
    gboolean valid = end && *end == '\0' && isfinite(value);
    g_string_free(text, TRUE);
    if (valid) *result = value == 0.0 ? 0.0 : value;
    return valid;
}

static gboolean optional_string(json_object *object, const char *key, char **result) {
    json_object *value = NULL;
    *result = NULL;
    if (!json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) return TRUE;
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    const char *raw = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (memchr(raw, '\0', length)) return FALSE;
    *result = g_strndup(raw, length);
    return TRUE;
}

static gboolean add_row(json_object *row, AiAndSpend *spend) {
    if (!row || !json_object_is_type(row, json_type_object)) return FALSE;
    json_object *cost_value = NULL;
    json_object *currency_value = NULL;
    gboolean has_cost = json_object_object_get_ex(row, "cost", &cost_value) &&
                        !json_object_is_type(cost_value, json_type_null);
    gboolean has_currency = json_object_object_get_ex(row, "currency", &currency_value) &&
                            !json_object_is_type(currency_value, json_type_null);
    if ((has_cost && !json_object_is_type(cost_value, json_type_string)) ||
        (has_currency && !json_object_is_type(currency_value, json_type_string))) {
        return FALSE;
    }
    if (!has_cost || !has_currency) return TRUE;

    const char *cost_raw = json_object_get_string(cost_value);
    size_t cost_length = (size_t)json_object_get_string_len(cost_value);
    AiAndDecimal cost = {0};
    if (!parse_decimal(cost_raw, cost_length, &cost)) return TRUE;

    const char *currency_raw = json_object_get_string(currency_value);
    size_t currency_length = (size_t)json_object_get_string_len(currency_value);
    if (memchr(currency_raw, '\0', currency_length)) {
        decimal_clear(&cost);
        return TRUE;
    }
    char *currency = g_strndup(currency_raw, currency_length);
    strip_unicode_whitespace(currency);
    if (currency[0] == '\0') {
        decimal_clear(&cost);
        g_free(currency);
        return TRUE;
    }
    char *normalized = g_utf8_strdown(currency, -1);
    g_free(currency);
    if (!spend->currency) spend->currency = g_strdup(normalized);
    if (g_str_equal(normalized, spend->currency)) decimal_add(&spend->total, &cost);
    decimal_clear(&cost);
    g_free(normalized);
    return TRUE;
}

static gboolean parse_page(const CodexBarHttpResponse *response,
                           AiAndSpend *spend,
                           AiAndPage *page,
                           GError **error) {
    json_object *root = parse_json_document(response->body, response->body_length);
    json_object *data = NULL;
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "data", &data) || !json_object_is_type(data, json_type_array)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse ai& usage: malformed logs response");
        return FALSE;
    }

    size_t count = json_object_array_length(data);
    for (size_t index = 0; index < count; index++) {
        if (!add_row(json_object_array_get_idx(data, index), spend)) goto malformed;
    }
    json_object *has_more = NULL;
    if (json_object_object_get_ex(root, "has_more", &has_more) && !json_object_is_type(has_more, json_type_null)) {
        if (!json_object_is_type(has_more, json_type_boolean)) goto malformed;
        page->has_more = json_object_get_boolean(has_more);
    }
    if (!optional_string(root, "next_after", &page->next_after) ||
        !optional_string(root, "next_after_id", &page->next_after_id)) {
        goto malformed;
    }
    json_object_put(root);
    return TRUE;

malformed:
    json_object_put(root);
    g_clear_pointer(&page->next_after, g_free);
    g_clear_pointer(&page->next_after_id, g_free);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse ai& usage: malformed logs response");
    return FALSE;
}

static char *logs_url(const char *after, const char *after_id) {
    if (!after || !after_id) return g_strdup(AIAND_LOGS_URL "?range=30days&limit=100");
    char *escaped_after = g_uri_escape_string(after, ":.", FALSE);
    char *escaped_id = g_uri_escape_string(after_id, NULL, FALSE);
    char *url = g_strdup_printf(AIAND_LOGS_URL "?range=30days&limit=100&after=%s&after_id=%s",
                                escaped_after,
                                escaped_id);
    g_free(escaped_after);
    g_free(escaped_id);
    return url;
}

static gboolean validate_response(const CodexBarHttpResponse *response, GError **error) {
    if (response->status >= 200 && response->status < 300) return TRUE;
    if (response->status == 401) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PERMISSION_DENIED,
                            "ai& rejected the API key. Create a new key at console.aiand.com and update Settings.");
    } else if (response->status == 402) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NO_SPACE,
                            "ai& reports the organization is out of credits. Top up at console.aiand.com.");
    } else if (response->status == 429) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_BUSY,
                            "ai& rate limit exceeded. Usage will refresh on the next cycle.");
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ai& logs API returned HTTP %ld.", response->status);
    }
    return FALSE;
}

CodexBarProvider *codexbar_aiand_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                      CodexBarAiAndTransport transport,
                                                                      GCancellable *cancellable,
                                                                      gint64 now_ms,
                                                                      GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    char *api_key = resolve_api_key(config);
    if (!api_key) {
        g_set_error_literal(
            error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Missing ai& API key. Add one in Settings or set AIAND_API_KEY.");
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", api_key);
    g_free(api_key);
    const CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
    };
    CodexBarHttpRequest request = {
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 15,
        .maximum_response_bytes = AIAND_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    AiAndSpend spend = {0};
    char *after = NULL;
    char *after_id = NULL;
    gboolean complete = FALSE;
    for (guint page_index = 0; page_index < AIAND_MAX_PAGES; page_index++) {
        if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) goto failed;
        request.url = logs_url(after, after_id);
        CodexBarHttpResponse *response = transport(&request, error);
        g_free((char *)request.url);
        request.url = NULL;
        if (!response) {
            if (!error || !*error) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "ai& network request failed");
            }
            goto failed;
        }
        if (cancellable && g_cancellable_is_cancelled(cancellable)) {
            codexbar_http_response_free(response);
            g_cancellable_set_error_if_cancelled(cancellable, error);
            goto failed;
        }
        if (!validate_response(response, error)) {
            codexbar_http_response_free(response);
            goto failed;
        }
        AiAndPage page = {0};
        gboolean parsed = parse_page(response, &spend, &page, error);
        codexbar_http_response_free(response);
        if (!parsed) goto failed;
        g_free(after);
        g_free(after_id);
        after = page.next_after;
        after_id = page.next_after_id;
        if (!page.has_more) {
            complete = TRUE;
            break;
        }
        if (!after || !after_id) break;
    }

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("aiand");
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions,
                           "dataConfidence",
                           json_object_new_string(complete ? "exact" : "estimated"));
    if (spend.currency) {
        double total = 0.0;
        if (!decimal_to_double(&spend.total, &total)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse ai& usage: spend is out of range");
            codexbar_provider_free(provider);
            goto failed;
        }
        provider->provider_cost = g_new0(CodexBarProviderCost, 1);
        provider->provider_cost->used = total;
        provider->provider_cost->limit = 0.0;
        provider->provider_cost->currency = g_utf8_strup(spend.currency, -1);
        provider->provider_cost->period =
            g_strdup(complete ? "Last 30 days" : "Last 30 days (partial)");
        provider->provider_cost->has_updated_at = TRUE;
        provider->provider_cost->updated_at_ms = now_ms;
    }
    g_free(after);
    g_free(after_id);
    g_free(spend.currency);
    decimal_clear(&spend.total);
    g_free(authorization);
    return provider;

failed:
    g_free(after);
    g_free(after_id);
    g_free(spend.currency);
    decimal_clear(&spend.total);
    g_free(authorization);
    return NULL;
}

CodexBarProvider *codexbar_aiand_fetch_with_transport(const CodexBarProviderConfig *config,
                                                      CodexBarAiAndTransport transport,
                                                      gint64 now_ms,
                                                      GError **error) {
    return codexbar_aiand_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_aiand_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                        GCancellable *cancellable,
                                                        GError **error) {
    return codexbar_aiand_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_aiand_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_aiand_fetch_with_cancellable(config, NULL, error);
}
