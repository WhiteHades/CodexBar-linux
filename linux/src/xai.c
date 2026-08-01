#include "xai.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define XAI_BASE_URL "https://management-api.x.ai"
#define XAI_HISTORY_DAYS 30
#define XAI_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)

typedef struct {
    json_object *daily;
    gboolean limit_reached;
    double total_cost;
    double today_cost;
} XAIHistory;

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

static char *clean_value(const char *raw) {
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

static char *resolve_management_key(const CodexBarProviderConfig *config) {
    char *value = clean_value(config ? config->api_key : NULL);
    if (!value) value = clean_value(g_getenv("XAI_MANAGEMENT_API_KEY"));
    return value;
}

static char *resolve_team_id(const CodexBarProviderConfig *config) {
    char *value = clean_value(config ? config->workspace_id : NULL);
    if (!value) value = clean_value(g_getenv("XAI_TEAM_ID"));
    return value;
}

gboolean codexbar_xai_has_api_key(const CodexBarProviderConfig *config) {
    char *key = resolve_management_key(config);
    gboolean present = key != NULL;
    g_free(key);
    return present;
}

gboolean codexbar_xai_has_credentials(const CodexBarProviderConfig *config) {
    char *key = resolve_management_key(config);
    char *team_id = resolve_team_id(config);
    gboolean present = key != NULL && team_id != NULL;
    g_free(key);
    g_free(team_id);
    return present;
}

static gboolean valid_team_id(const char *team_id) {
    return team_id && team_id[0] != '\0' && !g_str_equal(team_id, ".") && !g_str_equal(team_id, "..") &&
           !strchr(team_id, '/') && !strchr(team_id, '\\');
}

static gboolean json_whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

static json_object *parse_json_document(const char *text, size_t length) {
    if (!text || length > G_MAXINT || !g_utf8_validate(text, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, text, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && json_whitespace(text[consumed])) consumed++;
    gboolean valid = parse_error == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static gboolean parse_balance(const CodexBarHttpResponse *response, double *balance, GError **error) {
    json_object *root = parse_json_document(response->body, response->body_length);
    json_object *total = NULL;
    json_object *value = NULL;
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "total", &total) || !json_object_is_type(total, json_type_object) ||
        !json_object_object_get_ex(total, "val", &value) || !json_object_is_type(value, json_type_string)) {
        if (root) json_object_put(root);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "Could not parse xAI billing data: balance total.val is missing");
        return FALSE;
    }
    const char *raw = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    char *clean = memchr(raw, '\0', length) ? NULL : g_strndup(raw, length);
    if (clean) g_strstrip(clean);
    char *end = NULL;
    double cents = clean ? g_ascii_strtod(clean, &end) : 0.0;
    gboolean valid = clean && clean[0] != '\0' && end && *end == '\0' && isfinite(cents);
    if (valid) {
        const char *cursor = clean;
        if (*cursor == '-') cursor++;
        gboolean digit = FALSE;
        while (g_ascii_isdigit(*cursor)) {
            digit = TRUE;
            cursor++;
        }
        if (*cursor == '.') {
            cursor++;
            gboolean fractional_digit = FALSE;
            while (g_ascii_isdigit(*cursor)) {
                fractional_digit = TRUE;
                cursor++;
            }
            valid = fractional_digit;
        }
        valid = valid && digit && *cursor == '\0';
    }
    if (!valid) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_DATA,
                    "Could not parse xAI billing data: balance total.val is not a cent amount: %s",
                    clean ? clean : "");
        g_free(clean);
        json_object_put(root);
        return FALSE;
    }
    *balance = -cents / 100.0;
    g_free(clean);
    json_object_put(root);
    return TRUE;
}

static char *utc_day_from_timestamp(json_object *value) {
    if (!json_object_is_type(value, json_type_string)) return NULL;
    const char *timestamp = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (memchr(timestamp, '\0', length) || length < 20 || timestamp[10] != 'T') return NULL;
    char *copy = g_strndup(timestamp, length);
    GDateTime *date = g_date_time_new_from_iso8601(copy, NULL);
    g_free(copy);
    if (!date) return NULL;
    GDateTime *utc = g_date_time_to_utc(date);
    char *day = g_date_time_format(utc, "%Y-%m-%d");
    g_date_time_unref(utc);
    g_date_time_unref(date);
    return day;
}

static gint compare_strings(gconstpointer left, gconstpointer right) {
    return strcmp(left, right);
}

static gboolean parse_history(const CodexBarHttpResponse *response,
                              const char *today,
                              XAIHistory *history,
                              GError **error) {
    json_object *root = parse_json_document(response->body, response->body_length);
    json_object *series_array = NULL;
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "timeSeries", &series_array) ||
        !json_object_is_type(series_array, json_type_array)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse xAI billing data: malformed usage history");
        return FALSE;
    }
    GHashTable *totals = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    gboolean valid = TRUE;
    size_t series_count = json_object_array_length(series_array);
    for (size_t series_index = 0; valid && series_index < series_count; series_index++) {
        json_object *series = json_object_array_get_idx(series_array, series_index);
        json_object *points = NULL;
        valid = series && json_object_is_type(series, json_type_object) &&
                json_object_object_get_ex(series, "dataPoints", &points) && json_object_is_type(points, json_type_array);
        size_t point_count = valid ? json_object_array_length(points) : 0;
        for (size_t point_index = 0; valid && point_index < point_count; point_index++) {
            json_object *point = json_object_array_get_idx(points, point_index);
            json_object *timestamp = NULL;
            json_object *values = NULL;
            valid = point && json_object_is_type(point, json_type_object) &&
                    json_object_object_get_ex(point, "timestamp", &timestamp) &&
                    json_object_object_get_ex(point, "values", &values) && json_object_is_type(values, json_type_array);
            char *day = valid ? utc_day_from_timestamp(timestamp) : NULL;
            valid = valid && day != NULL;
            double amount = 0.0;
            size_t value_count = valid ? json_object_array_length(values) : 0;
            for (size_t value_index = 0; valid && value_index < value_count; value_index++) {
                json_object *number = json_object_array_get_idx(values, value_index);
                valid = number && (json_object_is_type(number, json_type_int) ||
                                   json_object_is_type(number, json_type_double)) &&
                        isfinite(json_object_get_double(number));
                if (valid && value_index == 0) amount = json_object_get_double(number);
            }
            if (!valid) {
                g_free(day);
                break;
            }
            double *total = g_hash_table_lookup(totals, day);
            if (!total) {
                total = g_new0(double, 1);
                g_hash_table_insert(totals, day, total);
            } else {
                g_free(day);
            }
            *total += amount;
            valid = isfinite(*total);
        }
    }
    json_object *limit = NULL;
    if (valid && json_object_object_get_ex(root, "limitReached", &limit) &&
        !json_object_is_type(limit, json_type_null)) {
        valid = json_object_is_type(limit, json_type_boolean);
        if (valid) history->limit_reached = json_object_get_boolean(limit);
    }
    if (!valid) {
        g_hash_table_unref(totals);
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse xAI billing data: malformed usage history");
        return FALSE;
    }

    GList *days = g_hash_table_get_keys(totals);
    days = g_list_sort(days, compare_strings);
    history->daily = json_object_new_array_ext((int)g_hash_table_size(totals));
    for (GList *item = days; item; item = item->next) {
        const char *day = item->data;
        double amount = *(double *)g_hash_table_lookup(totals, day);
        json_object *bucket = json_object_new_object();
        json_object_object_add(bucket, "day", json_object_new_string(day));
        json_object_object_add(bucket, "costUSD", json_object_new_double(amount));
        json_object_array_add(history->daily, bucket);
        history->total_cost += amount;
        if (g_str_equal(day, today)) history->today_cost = amount;
    }
    g_list_free(days);
    g_hash_table_unref(totals);
    json_object_put(root);
    return TRUE;
}

static gboolean validate_response_status(long status, GError **error) {
    if (status >= 200 && status < 300) return TRUE;
    if (status == 401 || status == 403) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PERMISSION_DENIED,
                            "xAI rejected the Management API key. Create one in the xAI Console under Settings > Management Keys; inference API keys are not accepted.");
    } else if (status == 404) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "xAI returned 404 for this team. Check the team ID and Management key.");
    } else if (status == 429) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_BUSY,
                            "xAI Management API rate limit exceeded. Usage will refresh on the next cycle.");
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "xAI Management API returned HTTP %ld.", status);
    }
    return FALSE;
}

static char *team_url(const char *team_id, const char *suffix) {
    char *escaped = g_uri_escape_string(team_id, NULL, FALSE);
    char *url = g_strdup_printf(XAI_BASE_URL "/v1/billing/teams/%s/%s", escaped, suffix);
    g_free(escaped);
    return url;
}

static char *request_timestamp(GDateTime *date) {
    return g_date_time_format(date, "%Y-%m-%d %H:%M:%S");
}

static char *iso_time(gint64 now_ms) {
    GDateTime *date = g_date_time_new_from_unix_utc(now_ms / 1000);
    char *result = date ? g_date_time_format_iso8601(date) : NULL;
    if (date) g_date_time_unref(date);
    return result;
}

static char *usage_body(gint64 now_ms, char **today) {
    GDateTime *now = g_date_time_new_from_unix_utc(now_ms / 1000);
    GDateTime *window_day = g_date_time_add_days(now, -(XAI_HISTORY_DAYS - 1));
    GDateTime *start = g_date_time_new_utc(g_date_time_get_year(window_day),
                                          g_date_time_get_month(window_day),
                                          g_date_time_get_day_of_month(window_day),
                                          0,
                                          0,
                                          0);
    char *start_text = request_timestamp(start);
    char *end_text = request_timestamp(now);
    *today = g_date_time_format(now, "%Y-%m-%d");
    json_object *time_range = json_object_new_object();
    json_object_object_add(time_range, "startTime", json_object_new_string(start_text));
    json_object_object_add(time_range, "endTime", json_object_new_string(end_text));
    json_object_object_add(time_range, "timezone", json_object_new_string("Etc/GMT"));
    json_object *value = json_object_new_object();
    json_object_object_add(value, "name", json_object_new_string("usd"));
    json_object_object_add(value, "aggregation", json_object_new_string("AGGREGATION_SUM"));
    json_object *values = json_object_new_array();
    json_object_array_add(values, value);
    json_object *analytics = json_object_new_object();
    json_object_object_add(analytics, "timeRange", time_range);
    json_object_object_add(analytics, "timeUnit", json_object_new_string("TIME_UNIT_DAY"));
    json_object_object_add(analytics, "values", values);
    json_object_object_add(analytics, "groupBy", json_object_new_array());
    json_object_object_add(analytics, "filters", json_object_new_array());
    json_object *root = json_object_new_object();
    json_object_object_add(root, "analyticsRequest", analytics);
    char *body = g_strdup(json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN));
    json_object_put(root);
    g_free(start_text);
    g_free(end_text);
    g_date_time_unref(start);
    g_date_time_unref(window_day);
    g_date_time_unref(now);
    return body;
}

static CodexBarProvider *map_provider(double balance, XAIHistory *history, gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("xai");
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = g_strdup("Management API");
    provider->provider_cost = g_new0(CodexBarProviderCost, 1);
    provider->provider_cost->used = balance;
    provider->provider_cost->currency = g_strdup("USD");
    provider->provider_cost->period = g_strdup("Prepaid credits");
    provider->provider_cost->has_updated_at = TRUE;
    provider->provider_cost->updated_at_ms = now_ms;

    char *updated = iso_time(now_ms);
    json_object *xai_usage = json_object_new_object();
    json_object_object_add(xai_usage, "balanceUSD", json_object_new_double(balance));
    json_object_object_add(xai_usage,
                           "daily",
                           history->daily ? history->daily : json_object_new_array());
    history->daily = NULL;
    json_object_object_add(xai_usage, "historyDays", json_object_new_int(XAI_HISTORY_DAYS));
    json_object_object_add(xai_usage, "limitReached", json_object_new_boolean(history->limit_reached));
    if (updated) json_object_object_add(xai_usage, "updatedAt", json_object_new_string(updated));
    g_free(updated);
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "xaiUsage", xai_usage);
    json_object_object_add(provider->usage_extensions,
                           "dataConfidence",
                           json_object_new_string(history->limit_reached ? "estimated" : "exact"));

    json_object *daily = json_object_object_get(xai_usage, "daily");
    if (daily && json_object_array_length(daily) > 0) {
        provider->token_cost = g_new0(CodexBarTokenCost, 1);
        provider->token_cost->has_today_cost = TRUE;
        provider->token_cost->today_cost = history->today_cost;
        provider->token_cost->has_last_days_cost = TRUE;
        provider->token_cost->last_days_cost = history->total_cost;
        provider->token_cost->currency = g_strdup("USD");
        provider->token_cost->has_history_days = TRUE;
        provider->token_cost->history_days = XAI_HISTORY_DAYS;
        if (history->limit_reached) provider->token_cost->history_label = g_strdup("Last 30 days (partial)");
        provider->token_cost->has_updated_at = TRUE;
        provider->token_cost->updated_at_ms = now_ms;
    }
    return provider;
}

CodexBarProvider *codexbar_xai_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                    CodexBarXAITransport transport,
                                                                    GCancellable *cancellable,
                                                                    gint64 now_ms,
                                                                    GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    char *key = resolve_management_key(config);
    if (!key) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "Missing xAI Management API key. Add one in Settings or set XAI_MANAGEMENT_API_KEY. Inference API keys are not accepted by the Management API.");
        return NULL;
    }
    char *team_id = resolve_team_id(config);
    if (!team_id) {
        g_free(key);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "Missing xAI team ID. Add it in Settings or set XAI_TEAM_ID.");
        return NULL;
    }
    if (!valid_team_id(team_id)) {
        g_free(key);
        g_free(team_id);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_ARGUMENT,
                            "The xAI team ID must be a single identifier without path separators.");
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", key);
    g_free(key);
    const CodexBarHttpRequestHeader balance_headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
    };
    CodexBarHttpRequest request = {
        .method = "GET",
        .headers = balance_headers,
        .header_count = G_N_ELEMENTS(balance_headers),
        .timeout_seconds = 15,
        .maximum_response_bytes = XAI_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    request.url = team_url(team_id, "prepaid/balance");
    CodexBarHttpResponse *response = transport(&request, error);
    g_free((char *)request.url);
    if (!response) goto failed;
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        codexbar_http_response_free(response);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        goto failed;
    }
    if (!validate_response_status(response->status, error)) {
        codexbar_http_response_free(response);
        goto failed;
    }
    double balance = 0.0;
    gboolean parsed = parse_balance(response, &balance, error);
    codexbar_http_response_free(response);
    if (!parsed) goto failed;

    char *today = NULL;
    char *body = usage_body(now_ms, &today);
    const CodexBarHttpRequestHeader usage_headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
    };
    request.method = "POST";
    request.headers = usage_headers;
    request.header_count = G_N_ELEMENTS(usage_headers);
    request.body = body;
    request.body_length = strlen(body);
    request.url = team_url(team_id, "usage");
    GError *history_error = NULL;
    response = transport(&request, &history_error);
    g_free((char *)request.url);
    g_free(body);
    XAIHistory history = {0};
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        codexbar_http_response_free(response);
        g_clear_error(&history_error);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        g_free(today);
        goto failed;
    }
    if (!response && history_error && history_error->domain == G_IO_ERROR &&
        history_error->code == G_IO_ERROR_CANCELLED) {
        g_propagate_error(error, history_error);
        history_error = NULL;
        g_free(today);
        goto failed;
    }
    if (response && (response->status == 401 || response->status == 403)) {
        validate_response_status(response->status, error);
        codexbar_http_response_free(response);
        g_clear_error(&history_error);
        g_free(today);
        goto failed;
    }
    if (response && response->status >= 200 && response->status < 300) {
        parse_history(response, today, &history, &history_error);
    }
    codexbar_http_response_free(response);
    g_clear_error(&history_error);
    g_free(today);
    CodexBarProvider *provider = map_provider(balance, &history, now_ms);
    if (history.daily) json_object_put(history.daily);
    g_free(authorization);
    g_free(team_id);
    return provider;

failed:
    if (!error || !*error) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "xAI network request failed");
    g_free(authorization);
    g_free(team_id);
    return NULL;
}

CodexBarProvider *codexbar_xai_fetch_with_transport(const CodexBarProviderConfig *config,
                                                    CodexBarXAITransport transport,
                                                    gint64 now_ms,
                                                    GError **error) {
    return codexbar_xai_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_xai_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                      GCancellable *cancellable,
                                                      GError **error) {
    return codexbar_xai_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_xai_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_xai_fetch_with_cancellable(config, NULL, error);
}
