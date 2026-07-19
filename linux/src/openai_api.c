#include "openai_api.h"

#include "http.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <stdckdint.h>
#include <string.h>

#define OPENAI_COSTS_URL "https://api.openai.com/v1/organization/costs"
#define OPENAI_COMPLETIONS_URL "https://api.openai.com/v1/organization/usage/completions"
#define OPENAI_HISTORY_DAYS 30
#define OPENAI_MAXIMUM_PAGES 100
#define OPENAI_MAXIMUM_RESPONSE_BYTES (4U * 1024U * 1024U)
#define OPENAI_TIMEOUT_SECONDS 20L

typedef struct {
    double total_cost;
    double today_cost;
    gint64 total_tokens;
    gint64 today_tokens;
    gint64 total_requests;
    gint64 today_requests;
} OpenAIUsageTotals;

typedef enum {
    OPENAI_PAGE_COSTS,
    OPENAI_PAGE_COMPLETIONS,
} OpenAIPageKind;

typedef struct {
    gboolean has_more;
    char *next_page;
} OpenAIPageMetadata;

static json_object *object_member(json_object *object, const char *name) {
    json_object *value = NULL;
    if (!object || json_object_get_type(object) != json_type_object ||
        !json_object_object_get_ex(object, name, &value)) {
        return NULL;
    }
    return value;
}

static gboolean json_integer(json_object *value, gint64 *result) {
    if (!value || json_object_get_type(value) != json_type_int) {
        return FALSE;
    }
    *result = json_object_get_int64(value);
    return TRUE;
}

static gboolean json_number(json_object *value, double *result) {
    if (!value) {
        return FALSE;
    }
    enum json_type type = json_object_get_type(value);
    if (type != json_type_int && type != json_type_double) {
        return FALSE;
    }
    double number = json_object_get_double(value);
    if (!isfinite(number)) {
        return FALSE;
    }
    *result = number;
    return TRUE;
}

static gboolean checked_add(gint64 *target, gint64 value, GError **error) {
    gint64 sum = 0;
    if (ckd_add(&sum, *target, value)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI usage total overflowed");
        return FALSE;
    }
    *target = sum;
    return TRUE;
}

static json_object *parse_json_object(const char *text, GError **error) {
    if (!text || text[0] == '\0') {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI returned an empty response");
        return NULL;
    }
    size_t length = strlen(text);
    if (length > OPENAI_MAXIMUM_RESPONSE_BYTES || length > G_MAXINT) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI response is too large");
        return NULL;
    }

    json_tokener *tokener = json_tokener_new_ex(64);
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, text, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t parsed = json_tokener_get_parse_end(tokener);
    while (parsed < length && g_ascii_isspace((guchar)text[parsed])) {
        parsed++;
    }
    json_tokener_free(tokener);
    if (parse_error != json_tokener_success || parsed != length || !root ||
        json_object_get_type(root) != json_type_object) {
        if (root) {
            json_object_put(root);
        }
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI returned invalid JSON");
        return NULL;
    }
    return root;
}

static gboolean add_cost_result(json_object *result, double *cost, GError **error) {
    json_object *amount = object_member(result, "amount");
    double value = 0.0;
    if (!amount || !json_number(object_member(amount, "value"), &value)) {
        return TRUE;
    }
    json_object *currency_value = object_member(amount, "currency");
    if (currency_value && json_object_get_type(currency_value) == json_type_string &&
        g_ascii_strcasecmp(json_object_get_string(currency_value), "usd") != 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI returned a non-USD cost");
        return FALSE;
    }
    double sum = *cost + value;
    if (!isfinite(sum)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI cost total overflowed");
        return FALSE;
    }
    *cost = sum;
    return TRUE;
}

static gboolean add_completion_result(json_object *result,
                                      gint64 *tokens,
                                      gint64 *requests,
                                      GError **error) {
    static const char *const token_fields[] = {
        "input_tokens",
        "output_tokens",
        "input_audio_tokens",
        "output_audio_tokens",
    };
    for (size_t index = 0; index < G_N_ELEMENTS(token_fields); index++) {
        gint64 value = 0;
        if (json_integer(object_member(result, token_fields[index]), &value)) {
            if (value < 0) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI returned a negative token count");
                return FALSE;
            }
            if (!checked_add(tokens, value, error)) {
                return FALSE;
            }
        }
    }
    gint64 request_count = 0;
    if (!json_integer(object_member(result, "num_model_requests"), &request_count)) {
        return TRUE;
    }
    if (request_count < 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI returned a negative request count");
        return FALSE;
    }
    return checked_add(requests, request_count, error);
}

static gboolean parse_page(const char *json,
                           OpenAIPageKind kind,
                           gint64 now_seconds,
                           OpenAIUsageTotals *totals,
                           OpenAIPageMetadata *metadata,
                           GError **error) {
    json_object *root = parse_json_object(json, error);
    if (!root) {
        return FALSE;
    }
    json_object *data = object_member(root, "data");
    if (!data || json_object_get_type(data) != json_type_array) {
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI response is missing data");
        return FALSE;
    }

    size_t bucket_count = json_object_array_length(data);
    for (size_t bucket_index = 0; bucket_index < bucket_count; bucket_index++) {
        json_object *bucket = json_object_array_get_idx(data, bucket_index);
        gint64 start = 0;
        gint64 end = 0;
        if (!bucket || json_object_get_type(bucket) != json_type_object ||
            !json_integer(object_member(bucket, "start_time"), &start) ||
            !json_integer(object_member(bucket, "end_time"), &end) || end <= start) {
            json_object_put(root);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI returned an invalid usage bucket");
            return FALSE;
        }
        gboolean today = start <= now_seconds && now_seconds < end;
        json_object *results = object_member(bucket, "results");
        if (!results || json_object_get_type(results) != json_type_array) {
            json_object_put(root);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI usage bucket is missing results");
            return FALSE;
        }
        size_t result_count = json_object_array_length(results);
        for (size_t result_index = 0; result_index < result_count; result_index++) {
            json_object *result = json_object_array_get_idx(results, result_index);
            if (!result || json_object_get_type(result) != json_type_object) {
                continue;
            }
            if (kind == OPENAI_PAGE_COSTS) {
                double value = 0.0;
                if (!add_cost_result(result, &value, error)) {
                    json_object_put(root);
                    return FALSE;
                }
                double total = totals->total_cost + value;
                double current = totals->today_cost + (today ? value : 0.0);
                if (!isfinite(total) || !isfinite(current)) {
                    json_object_put(root);
                    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI cost total overflowed");
                    return FALSE;
                }
                totals->total_cost = total;
                totals->today_cost = current;
            } else {
                gint64 tokens = 0;
                gint64 requests = 0;
                if (!add_completion_result(result, &tokens, &requests, error) ||
                    !checked_add(&totals->total_tokens, tokens, error) ||
                    !checked_add(&totals->total_requests, requests, error) ||
                    (today && (!checked_add(&totals->today_tokens, tokens, error) ||
                               !checked_add(&totals->today_requests, requests, error)))) {
                    json_object_put(root);
                    return FALSE;
                }
            }
        }
    }

    json_object *has_more = object_member(root, "has_more");
    if (!has_more || json_object_get_type(has_more) != json_type_boolean) {
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI response is missing has_more");
        return FALSE;
    }
    metadata->has_more = json_object_get_boolean(has_more);
    if (metadata->has_more) {
        json_object *next_page = object_member(root, "next_page");
        const char *raw = next_page && json_object_get_type(next_page) == json_type_string
                              ? json_object_get_string(next_page)
                              : NULL;
        if (!raw || raw[0] == '\0' || strlen(raw) > 4096 || !g_utf8_validate(raw, -1, NULL)) {
            json_object_put(root);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI pagination cursor is missing");
            return FALSE;
        }
        metadata->next_page = g_strdup(raw);
        g_strstrip(metadata->next_page);
        if (metadata->next_page[0] == '\0') {
            g_clear_pointer(&metadata->next_page, g_free);
            json_object_put(root);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI pagination cursor is missing");
            return FALSE;
        }
    }
    json_object_put(root);
    return TRUE;
}

static char *clean_value(const char *raw, size_t maximum_length) {
    if (!raw) {
        return NULL;
    }
    char *value = g_strdup(raw);
    g_strstrip(value);
    size_t length = strlen(value);
    if (length >= 2 && ((value[0] == '"' && value[length - 1] == '"') ||
                        (value[0] == '\'' && value[length - 1] == '\''))) {
        memmove(value, value + 1, length - 2);
        value[length - 2] = '\0';
        g_strstrip(value);
        length = strlen(value);
    }
    if (length == 0 || length > maximum_length || !g_utf8_validate(value, -1, NULL)) {
        g_free(value);
        return NULL;
    }
    return value;
}

static gboolean has_text(const char *raw) {
    if (!raw) {
        return FALSE;
    }
    while (*raw && g_ascii_isspace((guchar)*raw)) {
        raw++;
    }
    return *raw != '\0';
}

char *codexbar_openai_api_url(const char *endpoint,
                              gint64 start_time,
                              gint64 end_time,
                              const char *group_by,
                              const char *project_id,
                              const char *page) {
    char *project = project_id ? g_uri_escape_string(project_id, NULL, TRUE) : NULL;
    char *cursor = page ? g_uri_escape_string(page, NULL, TRUE) : NULL;
    GString *url = g_string_new(endpoint);
    g_string_append_printf(url,
                           "?start_time=%" G_GINT64_FORMAT "&end_time=%" G_GINT64_FORMAT
                           "&bucket_width=1d&limit=%d&group_by=%s",
                           start_time,
                           end_time,
                           OPENAI_HISTORY_DAYS,
                           group_by);
    if (project) {
        g_string_append_printf(url, "&project_ids=%s", project);
    }
    if (cursor) {
        g_string_append_printf(url, "&page=%s", cursor);
    }
    g_free(project);
    g_free(cursor);
    return g_string_free(url, FALSE);
}

static CodexBarProvider *make_provider(const OpenAIUsageTotals *totals,
                                      gint64 now_seconds,
                                      const char *project_id) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("openai");
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    ckd_mul(&provider->updated_at_ms, now_seconds, (gint64)1000);

    provider->provider_cost = g_new0(CodexBarProviderCost, 1);
    provider->provider_cost->used = totals->total_cost;
    provider->provider_cost->limit = 0.0;
    provider->provider_cost->currency = g_strdup("USD");
    provider->provider_cost->period = g_strdup("Last 30 days");
    provider->provider_cost->has_updated_at = TRUE;
    provider->provider_cost->updated_at_ms = provider->updated_at_ms;

    provider->token_cost = g_new0(CodexBarTokenCost, 1);
    provider->token_cost->has_today_tokens = TRUE;
    provider->token_cost->today_tokens = totals->today_tokens;
    provider->token_cost->has_today_cost = TRUE;
    provider->token_cost->today_cost = totals->today_cost;
    provider->token_cost->has_today_requests = TRUE;
    provider->token_cost->today_requests = totals->today_requests;
    provider->token_cost->has_last_days_tokens = TRUE;
    provider->token_cost->last_days_tokens = totals->total_tokens;
    provider->token_cost->has_last_days_cost = TRUE;
    provider->token_cost->last_days_cost = totals->total_cost;
    provider->token_cost->has_last_days_requests = TRUE;
    provider->token_cost->last_days_requests = totals->total_requests;
    provider->token_cost->currency = g_strdup("USD");
    provider->token_cost->history_label = g_strdup("Last 30 days");
    provider->token_cost->has_history_days = TRUE;
    provider->token_cost->history_days = OPENAI_HISTORY_DAYS;
    provider->token_cost->has_updated_at = TRUE;
    provider->token_cost->updated_at_ms = provider->updated_at_ms;

    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = project_id ? g_strdup_printf("Admin API: %s", project_id)
                                                   : g_strdup("Admin API");
    if (project_id) {
        provider->identity->organization = g_strdup_printf("Project: %s", project_id);
    }
    return provider;
}

CodexBarProvider *codexbar_openai_api_parse_usage(const char *costs_json,
                                                  const char *completions_json,
                                                  gint64 now_seconds,
                                                  const char *project_id,
                                                  GError **error) {
    gint64 updated_at_ms = 0;
    if (ckd_mul(&updated_at_ms, now_seconds, (gint64)1000)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "OpenAI timestamp is out of range");
        return NULL;
    }
    OpenAIUsageTotals totals = {0};
    OpenAIPageMetadata costs_page = {0};
    OpenAIPageMetadata completions_page = {0};
    if (!parse_page(costs_json, OPENAI_PAGE_COSTS, now_seconds, &totals, &costs_page, error) ||
        !parse_page(completions_json,
                    OPENAI_PAGE_COMPLETIONS,
                    now_seconds,
                    &totals,
                    &completions_page,
                    error)) {
        g_free(costs_page.next_page);
        g_free(completions_page.next_page);
        return NULL;
    }
    if (costs_page.has_more || completions_page.has_more) {
        g_free(costs_page.next_page);
        g_free(completions_page.next_page);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "OpenAI fixture requires pagination; use the live fetch path");
        return NULL;
    }
    g_free(costs_page.next_page);
    g_free(completions_page.next_page);
    return make_provider(&totals, now_seconds, project_id);
}

static gboolean fetch_pages(const char *endpoint,
                            const char *group_by,
                            OpenAIPageKind kind,
                            const char *api_key,
                            const char *project_id,
                            gint64 start_time,
                            gint64 end_time,
                            gint64 now_seconds,
                            OpenAIUsageTotals *totals,
                            GError **error) {
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    char *page = NULL;
    for (guint page_number = 0; page_number < OPENAI_MAXIMUM_PAGES; page_number++) {
        char *url = codexbar_openai_api_url(endpoint, start_time, end_time, group_by, project_id, page);
        char *authorization = g_strdup_printf("Bearer %s", api_key);
        const CodexBarHttpRequestHeader headers[] = {
            {"Accept", "application/json"},
            {"Authorization", authorization},
        };
        const CodexBarHttpRequest request = {
            .url = url,
            .method = "GET",
            .headers = headers,
            .header_count = G_N_ELEMENTS(headers),
            .timeout_seconds = OPENAI_TIMEOUT_SECONDS,
            .maximum_response_bytes = OPENAI_MAXIMUM_RESPONSE_BYTES,
            .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
            .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        };
        CodexBarHttpResponse *response = codexbar_http_send(&request, error);
        g_free(authorization);
        g_free(url);
        if (!response) {
            g_free(page);
            g_hash_table_unref(seen);
            return FALSE;
        }
        if (response->status != 200) {
            long status = response->status;
            codexbar_http_response_free(response);
            g_free(page);
            g_hash_table_unref(seen);
            if (status == 401 || status == 403) {
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "OpenAI rejected the Admin API key");
            } else {
                g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "OpenAI usage request failed with HTTP %ld", status);
            }
            return FALSE;
        }

        OpenAIPageMetadata metadata = {0};
        gboolean parsed = parse_page(response->body, kind, now_seconds, totals, &metadata, error);
        codexbar_http_response_free(response);
        g_free(page);
        page = NULL;
        if (!parsed) {
            g_free(metadata.next_page);
            g_hash_table_unref(seen);
            return FALSE;
        }
        if (!metadata.has_more) {
            g_free(metadata.next_page);
            g_hash_table_unref(seen);
            return TRUE;
        }
        if (g_hash_table_contains(seen, metadata.next_page)) {
            g_free(metadata.next_page);
            g_hash_table_unref(seen);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI pagination cursor repeated");
            return FALSE;
        }
        g_hash_table_add(seen, g_strdup(metadata.next_page));
        page = metadata.next_page;
    }
    g_free(page);
    g_hash_table_unref(seen);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "OpenAI pagination exceeded 100 pages");
    return FALSE;
}

CodexBarProvider *codexbar_openai_api_fetch(const CodexBarProviderConfig *config, GError **error) {
    char *api_key = clean_value(g_getenv("OPENAI_ADMIN_KEY"), 16384);
    if (!api_key) {
        api_key = clean_value(g_getenv("OPENAI_API_KEY"), 16384);
    }
    if (!api_key && config) {
        api_key = clean_value(config->api_key, 16384);
    }
    if (!api_key) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "OpenAI Admin API key not found. Set OPENAI_ADMIN_KEY or configure api_key.");
        return NULL;
    }

    const char *environment_project = g_getenv("OPENAI_PROJECT_ID");
    char *project_id = clean_value(environment_project, 256);
    if (has_text(environment_project) && !project_id) {
        g_free(api_key);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "OPENAI_PROJECT_ID is invalid");
        return NULL;
    }
    if (!project_id && config) {
        project_id = clean_value(config->workspace_id, 256);
        if (has_text(config->workspace_id) && !project_id) {
            g_free(api_key);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "OpenAI workspace_id is invalid");
            return NULL;
        }
    }

    GDateTime *now = g_date_time_new_now_utc();
    GDateTime *today = g_date_time_new_utc(g_date_time_get_year(now),
                                          g_date_time_get_month(now),
                                          g_date_time_get_day_of_month(now),
                                          0,
                                          0,
                                          0.0);
    GDateTime *start = g_date_time_add_days(today, -(OPENAI_HISTORY_DAYS - 1));
    GDateTime *end = g_date_time_add_days(today, 1);
    gint64 now_seconds = g_date_time_to_unix(now);
    gint64 start_time = g_date_time_to_unix(start);
    gint64 end_time = g_date_time_to_unix(end);
    g_date_time_unref(end);
    g_date_time_unref(start);
    g_date_time_unref(today);
    g_date_time_unref(now);

    OpenAIUsageTotals totals = {0};
    gboolean ok = fetch_pages(OPENAI_COSTS_URL,
                              "line_item",
                              OPENAI_PAGE_COSTS,
                              api_key,
                              project_id,
                              start_time,
                              end_time,
                              now_seconds,
                              &totals,
                              error) &&
                  fetch_pages(OPENAI_COMPLETIONS_URL,
                              "model",
                              OPENAI_PAGE_COMPLETIONS,
                              api_key,
                              project_id,
                              start_time,
                              end_time,
                              now_seconds,
                              &totals,
                              error);
    g_free(api_key);
    if (!ok) {
        g_free(project_id);
        return NULL;
    }
    CodexBarProvider *provider = make_provider(&totals, now_seconds, project_id);
    g_free(project_id);
    return provider;
}
