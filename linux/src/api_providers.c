#include "api_providers.h"

#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define API_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)
#define API_TIMEOUT_SECONDS 15
#define MAX_CREDENTIAL_BYTES 16384U
#define MAX_IDENTIFIER_BYTES 1024U
#define POE_HISTORY_PAGES 5U
#define POE_HISTORY_DAYS 30

#define DEEPGRAM_DEFAULT_BASE_URL "https://api.deepgram.com/v1"
#define POE_BALANCE_URL "https://api.poe.com/usage/current_balance"
#define POE_HISTORY_URL "https://api.poe.com/usage/points_history"
#define CHUTES_DEFAULT_BASE_URL "https://api.chutes.ai"

typedef struct {
    char *id;
    char *name;
} DeepgramProject;

typedef struct {
    char *project_id;
    char *project_name;
    guint project_count;
    char *start;
    char *end;
    double hours;
    double total_hours;
    double agent_hours;
    gint64 tokens_in;
    gint64 tokens_out;
    gint64 tts_characters;
    gint64 requests;
} DeepgramUsage;

typedef struct {
    json_object *entries;
    char *next_cursor;
    gint64 last_created_at_ms;
} PoeHistoryPage;

typedef enum {
    CHUTES_SUBSCRIPTION_UNKNOWN,
    CHUTES_SUBSCRIPTION_ACTIVE,
    CHUTES_SUBSCRIPTION_INACTIVE,
} ChutesSubscriptionState;

typedef struct {
    char *label;
    char *unit;
    gboolean has_used;
    double used;
    gboolean has_limit;
    double limit;
    gboolean has_remaining;
    double remaining;
    gboolean has_percent;
    double percent;
    gboolean has_window_minutes;
    gint64 window_minutes;
    gboolean has_resets_at;
    gint64 resets_at_ms;
} ChutesQuota;

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

static gboolean header_safe(const char *value) {
    if (!value || !g_utf8_validate(value, -1, NULL)) return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)value; *cursor; cursor++) {
        if (*cursor < 32 || *cursor == 127) return FALSE;
    }
    return TRUE;
}

static char *clean_value(const char *raw, size_t maximum_bytes) {
    if (!raw || !g_utf8_validate(raw, -1, NULL) || strlen(raw) > maximum_bytes) return NULL;
    char *value = g_strdup(raw);
    strip_unicode_whitespace(value);
    size_t length = strlen(value);
    if (length >= 2 && ((value[0] == '\'' && value[length - 1] == '\'') ||
                        (value[0] == '"' && value[length - 1] == '"'))) {
        value[length - 1] = '\0';
        memmove(value, value + 1, length - 1);
        strip_unicode_whitespace(value);
    }
    if (value[0] != '\0' && header_safe(value)) return value;
    g_free(value);
    return NULL;
}

static char *resolve_key(const CodexBarProviderConfig *config, const char *environment_key) {
    char *value = clean_value(config ? config->api_key : NULL, MAX_CREDENTIAL_BYTES);
    if (!value) value = clean_value(g_getenv(environment_key), MAX_CREDENTIAL_BYTES);
    return value;
}

static gboolean has_key(const CodexBarProviderConfig *config, const char *environment_key) {
    char *value = resolve_key(config, environment_key);
    gboolean present = value != NULL;
    g_free(value);
    return present;
}

gboolean codexbar_deepgram_has_api_key(const CodexBarProviderConfig *config) {
    return has_key(config, "DEEPGRAM_API_KEY");
}

gboolean codexbar_poe_has_api_key(const CodexBarProviderConfig *config) {
    return has_key(config, "POE_API_KEY");
}

gboolean codexbar_chutes_has_api_key(const CodexBarProviderConfig *config) {
    return has_key(config, "CHUTES_API_KEY");
}

static gboolean json_whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

static json_object *parse_json_document(const char *text, size_t length) {
    if (!text || length == 0 || memchr(text, '\0', length)) return NULL;
    json_tokener *tokener = json_tokener_new();
    if (!tokener) return NULL;
    json_object *root = json_tokener_parse_ex(tokener, text, (int)MIN(length, (size_t)G_MAXINT));
    enum json_tokener_error result = json_tokener_get_error(tokener);
    size_t offset = json_tokener_get_parse_end(tokener);
    gboolean valid = length <= G_MAXINT && result == json_tokener_success && root != NULL;
    while (valid && offset < length && json_whitespace(text[offset])) offset++;
    valid = valid && offset == length;
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static gboolean optional_string(json_object *object, const char *key, char **destination) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) return TRUE;
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    const char *text = json_object_get_string(value);
    if (!text || !g_utf8_validate(text, -1, NULL)) return FALSE;
    *destination = g_strdup(text);
    return TRUE;
}

static gboolean optional_double(json_object *object, const char *key, double *destination) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) return TRUE;
    if (!json_object_is_type(value, json_type_int) && !json_object_is_type(value, json_type_double)) return FALSE;
    double number = json_object_get_double(value);
    if (!isfinite(number)) return FALSE;
    *destination = number;
    return TRUE;
}

static gboolean optional_add_double(json_object *object, const char *key, double *destination) {
    double value = 0;
    if (!optional_double(object, key, &value)) return FALSE;
    double total = *destination + value;
    if (!isfinite(total)) return FALSE;
    *destination = total;
    return TRUE;
}

static gboolean checked_add_int64(gint64 *total, gint64 value) {
    if ((value > 0 && *total > G_MAXINT64 - value) || (value < 0 && *total < G_MININT64 - value)) return FALSE;
    *total += value;
    return TRUE;
}

static gboolean optional_int64(json_object *object, const char *key, gint64 *destination) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) return TRUE;
    if (!json_object_is_type(value, json_type_int)) return FALSE;
    return checked_add_int64(destination, json_object_get_int64(value));
}

static char *timestamp_iso8601(gint64 timestamp_ms) {
    GDateTime *date = g_date_time_new_from_unix_utc(timestamp_ms / 1000);
    if (!date) return NULL;
    char *value = g_date_time_format_iso8601(date);
    g_date_time_unref(date);
    return value;
}

static json_object *timestamp_json(gint64 timestamp_ms) {
    char *text = timestamp_iso8601(timestamp_ms);
    json_object *value = text ? json_object_new_string(text) : NULL;
    g_free(text);
    return value;
}

static void add_confidence(CodexBarProvider *provider, const char *value) {
    if (!provider->usage_extensions) provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string(value));
}

static gboolean cancellation_requested(GCancellable *cancellable, GError **error) {
    return cancellable && g_cancellable_set_error_if_cancelled(cancellable, error);
}

static CodexBarHttpResponse *perform_request(const CodexBarHttpRequest *request,
                                              CodexBarAPIProviderTransport transport,
                                              GError **error) {
    if (cancellation_requested(request->cancellable, error)) return NULL;
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

static char *validated_base_url(const char *environment_key, const char *fallback, GError **error) {
    const char *raw = g_getenv(environment_key);
    char *clean = clean_value(raw, 8192);
    if (raw && !clean) {
        char *empty_check = g_utf8_validate(raw, -1, NULL) ? g_strdup(raw) : NULL;
        if (empty_check) strip_unicode_whitespace(empty_check);
        gboolean empty = empty_check && empty_check[0] == '\0';
        g_free(empty_check);
        if (!empty) {
            g_set_error(
                error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "%s endpoint override is invalid", environment_key);
            return NULL;
        }
    }
    GError *normalize_error = NULL;
    char *normalized =
        codexbar_http_normalize_endpoint(clean ? clean : fallback, CODEXBAR_HTTP_HTTPS_ONLY, &normalize_error);
    g_free(clean);
    if (!normalized) {
        g_clear_error(&normalize_error);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "%s endpoint override is invalid", environment_key);
        return NULL;
    }
    GUri *uri = g_uri_parse(normalized, G_URI_FLAGS_ENCODED, NULL);
    gboolean valid = uri && g_uri_get_query(uri) == NULL && g_uri_get_fragment(uri) == NULL;
    if (uri) g_uri_unref(uri);
    if (!valid) {
        g_free(normalized);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "%s endpoint override is invalid", environment_key);
        return NULL;
    }
    size_t length = strlen(normalized);
    while (length > strlen("https://") && normalized[length - 1] == '/') normalized[--length] = '\0';
    return normalized;
}

static char *append_path(const char *base, const char *path) {
    return g_strdup_printf("%s/%s", base, path[0] == '/' ? path + 1 : path);
}

static void deepgram_project_free(gpointer data) {
    DeepgramProject *project = data;
    if (!project) return;
    g_free(project->id);
    g_free(project->name);
    g_free(project);
}

static void deepgram_usage_clear(DeepgramUsage *usage) {
    g_free(usage->project_id);
    g_free(usage->project_name);
    g_free(usage->start);
    g_free(usage->end);
    memset(usage, 0, sizeof(*usage));
}

static gboolean parse_deepgram_usage(const char *json,
                                     size_t length,
                                     const char *project_id,
                                     const char *project_name,
                                     DeepgramUsage *usage,
                                     GError **error) {
    json_object *root = parse_json_document(json, length);
    json_object *results = NULL;
    gboolean valid = root && json_object_is_type(root, json_type_object) &&
                     json_object_object_get_ex(root, "results", &results) &&
                     json_object_is_type(results, json_type_array) && optional_string(root, "start", &usage->start) &&
                     optional_string(root, "end", &usage->end);
    usage->project_id = g_strdup(project_id);
    usage->project_name = project_name ? g_strdup(project_name) : NULL;
    usage->project_count = 1;
    size_t count = valid ? json_object_array_length(results) : 0;
    for (size_t index = 0; valid && index < count; index++) {
        json_object *row = json_object_array_get_idx(results, index);
        valid = row && json_object_is_type(row, json_type_object) &&
                optional_add_double(row, "hours", &usage->hours) &&
                optional_add_double(row, "total_hours", &usage->total_hours) &&
                optional_add_double(row, "agent_hours", &usage->agent_hours) &&
                optional_int64(row, "tokens_in", &usage->tokens_in) &&
                optional_int64(row, "tokens_out", &usage->tokens_out) &&
                optional_int64(row, "tts_characters", &usage->tts_characters) &&
                optional_int64(row, "requests", &usage->requests) && isfinite(usage->hours) &&
                isfinite(usage->total_hours) && isfinite(usage->agent_hours);
    }
    if (root) json_object_put(root);
    if (valid) return TRUE;
    deepgram_usage_clear(usage);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse Deepgram usage response");
    return FALSE;
}

static CodexBarProvider *deepgram_provider(const DeepgramUsage *usage, gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("deepgram");
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    if (usage->project_count > 1) {
        provider->identity->login_method = g_strdup_printf("%u projects", usage->project_count);
    } else if (usage->project_name && *usage->project_name) {
        provider->identity->login_method = g_strdup_printf("Project: %s", usage->project_name);
    } else {
        provider->identity->login_method = g_strdup_printf("Project: %s", usage->project_id);
    }
    json_object *snapshot = json_object_new_object();
    json_object_object_add(snapshot, "projectID", json_object_new_string(usage->project_id));
    if (usage->project_name) json_object_object_add(snapshot, "projectName", json_object_new_string(usage->project_name));
    json_object_object_add(snapshot, "projectCount", json_object_new_int64(usage->project_count));
    if (usage->start) json_object_object_add(snapshot, "start", json_object_new_string(usage->start));
    if (usage->end) json_object_object_add(snapshot, "end", json_object_new_string(usage->end));
    json_object_object_add(snapshot, "hours", json_object_new_double(usage->hours));
    json_object_object_add(snapshot, "totalHours", json_object_new_double(usage->total_hours));
    json_object_object_add(snapshot, "agentHours", json_object_new_double(usage->agent_hours));
    json_object_object_add(snapshot, "tokensIn", json_object_new_int64(usage->tokens_in));
    json_object_object_add(snapshot, "tokensOut", json_object_new_int64(usage->tokens_out));
    json_object_object_add(snapshot, "ttsCharacters", json_object_new_int64(usage->tts_characters));
    json_object_object_add(snapshot, "requests", json_object_new_int64(usage->requests));
    json_object_object_add(snapshot, "updatedAt", timestamp_json(now_ms));
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "deepgramUsage", snapshot);
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));
    return provider;
}

CodexBarProvider *codexbar_deepgram_parse_usage_bytes(const char *json,
                                                       size_t length,
                                                       const char *project_id,
                                                       const char *project_name,
                                                       guint project_count,
                                                       gint64 now_ms,
                                                       GError **error) {
    char *clean_id = clean_value(project_id, MAX_IDENTIFIER_BYTES);
    char *clean_name = clean_value(project_name, MAX_IDENTIFIER_BYTES);
    if (!clean_id) {
        g_free(clean_name);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Deepgram project ID is invalid");
        return NULL;
    }
    DeepgramUsage usage = {0};
    gboolean parsed = parse_deepgram_usage(json, length, clean_id, clean_name, &usage, error);
    g_free(clean_id);
    g_free(clean_name);
    if (!parsed) return NULL;
    usage.project_count = MAX(project_count, 1U);
    CodexBarProvider *provider = deepgram_provider(&usage, now_ms);
    deepgram_usage_clear(&usage);
    return provider;
}

static GPtrArray *parse_deepgram_projects(const CodexBarHttpResponse *response, GError **error) {
    json_object *root = parse_json_document(response->body, response->body_length);
    json_object *projects = NULL;
    gboolean valid = root && json_object_is_type(root, json_type_object) &&
                     json_object_object_get_ex(root, "projects", &projects) &&
                     json_object_is_type(projects, json_type_array);
    GPtrArray *result = g_ptr_array_new_with_free_func(deepgram_project_free);
    size_t count = valid ? json_object_array_length(projects) : 0;
    for (size_t index = 0; valid && index < count; index++) {
        json_object *row = json_object_array_get_idx(projects, index);
        json_object *id = NULL;
        valid = row && json_object_is_type(row, json_type_object) &&
                json_object_object_get_ex(row, "project_id", &id) && json_object_is_type(id, json_type_string);
        char *clean_id = valid ? clean_value(json_object_get_string(id), MAX_IDENTIFIER_BYTES) : NULL;
        valid = valid && clean_id != NULL;
        if (!valid) {
            g_free(clean_id);
            break;
        }
        DeepgramProject *project = g_new0(DeepgramProject, 1);
        project->id = clean_id;
        if (!optional_string(row, "name", &project->name)) {
            deepgram_project_free(project);
            valid = FALSE;
            break;
        }
        g_ptr_array_add(result, project);
    }
    if (root) json_object_put(root);
    if (valid && result->len > 0) return result;
    g_ptr_array_unref(result);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Deepgram project list is invalid or empty");
    return NULL;
}

static gboolean validate_deepgram_response(CodexBarHttpResponse *response, GError **error) {
    if (response->status == 200) return TRUE;
    if (response->status == 401) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Deepgram API key is invalid or expired");
    } else if (response->status == 403) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_PERMISSION_DENIED,
                            "Deepgram API key cannot access the project or Management API");
    } else if (response->status == 400) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Deepgram API rejected the request (HTTP 400)");
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Deepgram API returned HTTP %ld", response->status);
    }
    return FALSE;
}

static gboolean deepgram_request(const char *url,
                                 const char *authorization,
                                 CodexBarAPIProviderTransport transport,
                                 GCancellable *cancellable,
                                 CodexBarHttpResponse **result,
                                 GError **error) {
    const CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
    };
    CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = API_TIMEOUT_SECONDS,
        .maximum_response_bytes = API_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    *result = perform_request(&request, transport, error);
    if (!*result) return FALSE;
    if (validate_deepgram_response(*result, error)) return TRUE;
    codexbar_http_response_free(*result);
    *result = NULL;
    return FALSE;
}

static gboolean aggregate_deepgram(DeepgramUsage *total, DeepgramUsage *next) {
    double hours = total->hours + next->hours;
    double total_hours = total->total_hours + next->total_hours;
    double agent_hours = total->agent_hours + next->agent_hours;
    if (!isfinite(hours) || !isfinite(total_hours) || !isfinite(agent_hours) ||
        !checked_add_int64(&total->tokens_in, next->tokens_in) ||
        !checked_add_int64(&total->tokens_out, next->tokens_out) ||
        !checked_add_int64(&total->tts_characters, next->tts_characters) ||
        !checked_add_int64(&total->requests, next->requests)) {
        return FALSE;
    }
    total->hours = hours;
    total->total_hours = total_hours;
    total->agent_hours = agent_hours;
    if (next->start && (!total->start || strcmp(next->start, total->start) < 0)) {
        g_free(total->start);
        total->start = g_strdup(next->start);
    }
    if (next->end && (!total->end || strcmp(next->end, total->end) > 0)) {
        g_free(total->end);
        total->end = g_strdup(next->end);
    }
    return TRUE;
}

CodexBarProvider *codexbar_deepgram_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarAPIProviderTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    char *key = resolve_key(config, "DEEPGRAM_API_KEY");
    if (!key) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Missing Deepgram API key");
        return NULL;
    }
    char *project_id = clean_value(config ? config->workspace_id : NULL, MAX_IDENTIFIER_BYTES);
    if (!project_id) project_id = clean_value(g_getenv("DEEPGRAM_PROJECT_ID"), MAX_IDENTIFIER_BYTES);
    char *base = validated_base_url("DEEPGRAM_API_URL", DEEPGRAM_DEFAULT_BASE_URL, error);
    if (!base) {
        g_free(project_id);
        g_free(key);
        return NULL;
    }
    char *authorization = g_strdup_printf("Token %s", key);
    g_free(key);
    GPtrArray *projects = g_ptr_array_new_with_free_func(deepgram_project_free);
    if (project_id) {
        DeepgramProject *project = g_new0(DeepgramProject, 1);
        project->id = project_id;
        g_ptr_array_add(projects, project);
    } else {
        char *url = append_path(base, "projects");
        CodexBarHttpResponse *response = NULL;
        gboolean sent = deepgram_request(url, authorization, transport, cancellable, &response, error);
        g_free(url);
        if (!sent) goto failure;
        g_ptr_array_unref(projects);
        projects = parse_deepgram_projects(response, error);
        codexbar_http_response_free(response);
        if (!projects) goto failure_without_projects;
    }

    DeepgramUsage total = {
        .project_id = g_strdup(projects->len == 1 ? ((DeepgramProject *)g_ptr_array_index(projects, 0))->id : "all"),
        .project_name = projects->len == 1 ? g_strdup(((DeepgramProject *)g_ptr_array_index(projects, 0))->name) : NULL,
        .project_count = projects->len,
    };
    for (guint index = 0; index < projects->len; index++) {
        if (cancellation_requested(cancellable, error)) goto usage_failure;
        DeepgramProject *project = g_ptr_array_index(projects, index);
        char *escaped = g_uri_escape_string(project->id, NULL, FALSE);
        char *path = g_strdup_printf("projects/%s/usage/breakdown", escaped);
        char *url = append_path(base, path);
        g_free(path);
        g_free(escaped);
        CodexBarHttpResponse *response = NULL;
        gboolean sent = deepgram_request(url, authorization, transport, cancellable, &response, error);
        g_free(url);
        if (!sent) goto usage_failure;
        DeepgramUsage next = {0};
        gboolean parsed = parse_deepgram_usage(
            response->body, response->body_length, project->id, project->name, &next, error);
        codexbar_http_response_free(response);
        if (!parsed) goto usage_failure;
        gboolean aggregated = aggregate_deepgram(&total, &next);
        deepgram_usage_clear(&next);
        if (!aggregated) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Deepgram usage totals overflowed");
            goto usage_failure;
        }
    }
    CodexBarProvider *provider = deepgram_provider(&total, now_ms);
    deepgram_usage_clear(&total);
    g_ptr_array_unref(projects);
    g_free(authorization);
    g_free(base);
    return provider;

usage_failure:
    deepgram_usage_clear(&total);
failure:
    g_ptr_array_unref(projects);
failure_without_projects:
    g_free(authorization);
    g_free(base);
    return NULL;
}

CodexBarProvider *codexbar_deepgram_fetch_with_transport(const CodexBarProviderConfig *config,
                                                          CodexBarAPIProviderTransport transport,
                                                          gint64 now_ms,
                                                          GError **error) {
    return codexbar_deepgram_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_deepgram_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                            GCancellable *cancellable,
                                                            GError **error) {
    return codexbar_deepgram_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_deepgram_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_deepgram_fetch_with_cancellable(config, NULL, error);
}

static gboolean json_double_value(json_object *value, double *result) {
    if (!value || json_object_is_type(value, json_type_null)) return FALSE;
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        *result = json_object_get_double(value);
        return isfinite(*result);
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    char *text = g_strdup(json_object_get_string(value));
    g_strstrip(text);
    char *end = NULL;
    double number = g_ascii_strtod(text, &end);
    gboolean valid = text[0] != '\0' && end && *end == '\0' && isfinite(number);
    g_free(text);
    if (valid) *result = number;
    return valid;
}

static gboolean object_double(json_object *object, const char *const *keys, size_t count, double *result) {
    for (size_t index = 0; index < count; index++) {
        json_object *value = NULL;
        if (json_object_object_get_ex(object, keys[index], &value) && json_double_value(value, result)) return TRUE;
    }
    return FALSE;
}

static char *object_clean_string(json_object *object, const char *const *keys, size_t count) {
    for (size_t index = 0; index < count; index++) {
        json_object *value = NULL;
        if (json_object_object_get_ex(object, keys[index], &value) && json_object_is_type(value, json_type_string)) {
            char *clean = clean_value(json_object_get_string(value), MAX_IDENTIFIER_BYTES);
            if (clean) return clean;
        }
    }
    return NULL;
}

static gboolean timestamp_from_json(json_object *value, gboolean microseconds, gint64 *result_ms) {
    if (!value || json_object_is_type(value, json_type_null)) return FALSE;
    double raw = 0;
    if (json_double_value(value, &raw)) {
        if (raw <= 0) return FALSE;
        double seconds = microseconds && raw > 1000000000000.0 ? raw / 1000000.0
                                                                 : (!microseconds && raw > 10000000000.0 ? raw / 1000.0 : raw);
        if (!isfinite(seconds) || seconds > (double)G_MAXINT64 / 1000.0) return FALSE;
        *result_ms = (gint64)(seconds * 1000.0);
        return TRUE;
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    char *text = clean_value(json_object_get_string(value), 256);
    if (!text) return FALSE;
    GDateTime *date = g_date_time_new_from_iso8601(text, NULL);
    g_free(text);
    if (!date) return FALSE;
    *result_ms = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static char *poe_balance_label(double balance) {
    if (!isfinite(balance)) return NULL;
    char *number = NULL;
    if (balance >= 1000) {
        char *raw = g_strdup_printf("%.0f", balance);
        size_t length = strlen(raw);
        GString *grouped = g_string_sized_new(length + length / 3);
        for (size_t index = 0; index < length; index++) {
            if (index > 0 && (length - index) % 3 == 0) g_string_append_c(grouped, ',');
            g_string_append_c(grouped, raw[index]);
        }
        g_free(raw);
        number = g_string_free(grouped, FALSE);
    } else {
        number = g_strdup_printf("%.1f", balance);
        if (g_str_has_suffix(number, ".0")) number[strlen(number) - 2] = '\0';
    }
    char *label = g_strdup_printf("Balance: %s points", number);
    g_free(number);
    return label;
}

static json_object *parse_poe_balance(const char *json,
                                      size_t length,
                                      gboolean *has_balance,
                                      double *balance,
                                      GError **error) {
    json_object *root = parse_json_document(json, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse Poe balance response");
        return NULL;
    }
    json_object *value = NULL;
    if (json_object_object_get_ex(root, "current_point_balance", &value) &&
        !json_object_is_type(value, json_type_null)) {
        if (!json_double_value(value, balance)) {
            json_object_put(root);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Poe balance is invalid");
            return NULL;
        }
        *has_balance = TRUE;
    }
    return root;
}

static json_object *poe_raw_entries(json_object *root) {
    static const char *const keys[] = {"data", "items", "results"};
    for (size_t index = 0; index < G_N_ELEMENTS(keys); index++) {
        json_object *value = NULL;
        if (json_object_object_get_ex(root, keys[index], &value) && json_object_is_type(value, json_type_array)) {
            return value;
        }
    }
    return NULL;
}

static void poe_history_page_clear(PoeHistoryPage *page) {
    if (page->entries) json_object_put(page->entries);
    g_free(page->next_cursor);
    memset(page, 0, sizeof(*page));
}

static gboolean parse_poe_history_page(const char *json,
                                       size_t length,
                                       PoeHistoryPage *page,
                                       GError **error) {
    json_object *root = parse_json_document(json, length);
    if (!root || !json_object_is_type(root, json_type_object)) goto malformed;
    json_object *raw_entries = poe_raw_entries(root);
    if (!raw_entries) raw_entries = json_object_new_array();
    else json_object_get(raw_entries);
    page->entries = json_object_new_array();
    size_t count = json_object_array_length(raw_entries);
    for (size_t index = 0; index < count; index++) {
        json_object *row = json_object_array_get_idx(raw_entries, index);
        if (!row || !json_object_is_type(row, json_type_object)) continue;
        static const char *const timestamp_keys[] = {"creation_time", "timestamp", "created_at"};
        json_object *timestamp = NULL;
        for (size_t key_index = 0; key_index < G_N_ELEMENTS(timestamp_keys); key_index++) {
            if (json_object_object_get_ex(row, timestamp_keys[key_index], &timestamp)) break;
        }
        gint64 created_at_ms = 0;
        if (!timestamp_from_json(timestamp, TRUE, &created_at_ms)) continue;
        static const char *const model_keys[] = {"bot_name"};
        static const char *const type_keys[] = {"usage_type"};
        static const char *const id_keys[] = {"query_id", "message_id", "id"};
        static const char *const point_keys[] = {"cost_points", "points", "point_cost"};
        static const char *const cost_keys[] = {"cost_usd", "usd"};
        char *model = object_clean_string(row, model_keys, G_N_ELEMENTS(model_keys));
        char *type = object_clean_string(row, type_keys, G_N_ELEMENTS(type_keys));
        char *id = object_clean_string(row, id_keys, G_N_ELEMENTS(id_keys));
        if (!model) model = g_strdup("unknown");
        if (!type) type = g_strdup("unknown");
        if (!id) id = g_strdup_printf("%" G_GINT64_FORMAT "-%s", created_at_ms / 1000, model);
        double points = 0;
        (void)object_double(row, point_keys, G_N_ELEMENTS(point_keys), &points);
        double cost = 0;
        gboolean has_cost = object_double(row, cost_keys, G_N_ELEMENTS(cost_keys), &cost);
        json_object *entry = json_object_new_object();
        json_object_object_add(entry, "id", json_object_new_string(id));
        json_object_object_add(entry, "createdAt", timestamp_json(created_at_ms));
        json_object_object_add(entry, "model", json_object_new_string(model));
        json_object_object_add(entry, "usageType", json_object_new_string(type));
        json_object_object_add(entry, "points", json_object_new_double(MAX(0.0, points)));
        if (has_cost) json_object_object_add(entry, "costUSD", json_object_new_double(cost));
        json_object_object_add(entry, "_createdAtMs", json_object_new_int64(created_at_ms));
        json_object_array_add(page->entries, entry);
        page->last_created_at_ms = created_at_ms;
        g_free(model);
        g_free(type);
        g_free(id);
    }
    json_object *cursor = NULL;
    if (json_object_object_get_ex(root, "next_cursor", &cursor) && json_object_is_type(cursor, json_type_string)) {
        page->next_cursor = clean_value(json_object_get_string(cursor), MAX_IDENTIFIER_BYTES);
    }
    json_object *has_more = NULL;
    if (!page->next_cursor && json_object_object_get_ex(root, "has_more", &has_more) &&
        json_object_is_type(has_more, json_type_boolean) && json_object_get_boolean(has_more) && count > 0) {
        json_object *last = json_object_array_get_idx(raw_entries, count - 1);
        static const char *const keys[] = {"query_id"};
        if (last && json_object_is_type(last, json_type_object)) {
            page->next_cursor = object_clean_string(last, keys, G_N_ELEMENTS(keys));
        }
    }
    json_object_put(raw_entries);
    json_object_put(root);
    return TRUE;

malformed:
    if (root) json_object_put(root);
    poe_history_page_clear(page);
    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse Poe history response");
    return FALSE;
}

static gint compare_json_entries(gconstpointer left, gconstpointer right) {
    json_object *a = *(json_object *const *)left;
    json_object *b = *(json_object *const *)right;
    return (json_object_get_int64(json_object_object_get(a, "_createdAtMs")) >
            json_object_get_int64(json_object_object_get(b, "_createdAtMs"))) -
           (json_object_get_int64(json_object_object_get(a, "_createdAtMs")) <
            json_object_get_int64(json_object_object_get(b, "_createdAtMs")));
}

static void json_object_destroy(gpointer data) {
    if (data) json_object_put(data);
}

static json_object *poe_history_snapshot(json_object *entries, gint64 now_ms) {
    size_t count = json_object_array_length(entries);
    if (count == 0) return NULL;
    GPtrArray *sorted = g_ptr_array_sized_new(count);
    for (size_t index = 0; index < count; index++) g_ptr_array_add(sorted, json_object_array_get_idx(entries, index));
    g_ptr_array_sort(sorted, compare_json_entries);
    json_object *output_entries = json_object_new_array_ext((int)count);
    GHashTable *daily = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, json_object_destroy);
    for (guint index = 0; index < sorted->len; index++) {
        json_object *entry = g_ptr_array_index(sorted, index);
        gint64 created_at_ms = json_object_get_int64(json_object_object_get(entry, "_createdAtMs"));
        GDateTime *date = g_date_time_new_from_unix_utc(created_at_ms / 1000);
        char *day = date ? g_date_time_format(date, "%Y-%m-%d") : NULL;
        if (date) g_date_time_unref(date);
        if (day) {
            json_object *bucket = g_hash_table_lookup(daily, day);
            if (!bucket) {
                bucket = json_object_new_object();
                json_object_object_add(bucket, "day", json_object_new_string(day));
                json_object_object_add(bucket, "points", json_object_new_double(0));
                json_object_object_add(bucket, "requests", json_object_new_int(0));
                g_hash_table_insert(daily, g_strdup(day), bucket);
            }
            double points = json_object_get_double(json_object_object_get(bucket, "points")) +
                            json_object_get_double(json_object_object_get(entry, "points"));
            int requests = json_object_get_int(json_object_object_get(bucket, "requests")) + 1;
            json_object_object_add(bucket, "points", json_object_new_double(points));
            json_object_object_add(bucket, "requests", json_object_new_int(requests));
            json_object *cost = NULL;
            if (json_object_object_get_ex(entry, "costUSD", &cost)) {
                json_object *existing = NULL;
                double total = json_object_object_get_ex(bucket, "costUSD", &existing)
                                   ? json_object_get_double(existing)
                                   : 0;
                json_object_object_add(bucket, "costUSD", json_object_new_double(total + MAX(0, json_object_get_double(cost))));
            }
            g_free(day);
        }
        json_object_object_del(entry, "_createdAtMs");
        json_object_array_add(output_entries, json_object_get(entry));
    }
    GList *days = g_hash_table_get_keys(daily);
    days = g_list_sort(days, (GCompareFunc)g_strcmp0);
    json_object *daily_array = json_object_new_array_ext((int)g_hash_table_size(daily));
    for (GList *item = days; item; item = item->next) {
        json_object_array_add(daily_array, json_object_get(g_hash_table_lookup(daily, item->data)));
    }
    g_list_free(days);
    g_hash_table_unref(daily);
    g_ptr_array_unref(sorted);
    json_object *history = json_object_new_object();
    json_object_object_add(history, "entries", output_entries);
    json_object_object_add(history, "daily", daily_array);
    json_object_object_add(history, "updatedAt", timestamp_json(now_ms));
    return history;
}

static CodexBarProvider *poe_provider(gboolean has_balance,
                                     double balance,
                                     json_object *entries,
                                     gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("poe");
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    if (has_balance) provider->identity->login_method = poe_balance_label(balance);
    provider->usage_extensions = json_object_new_object();
    json_object *history = entries ? poe_history_snapshot(entries, now_ms) : NULL;
    if (history) json_object_object_add(provider->usage_extensions, "poeUsage", history);
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));
    return provider;
}

CodexBarProvider *codexbar_poe_parse_usage_bytes(const char *balance_json,
                                                  size_t balance_length,
                                                  const char *history_json,
                                                  size_t history_length,
                                                  gint64 now_ms,
                                                  GError **error) {
    gboolean has_balance = FALSE;
    double balance = 0;
    json_object *balance_root = parse_poe_balance(balance_json, balance_length, &has_balance, &balance, error);
    if (!balance_root) return NULL;
    json_object_put(balance_root);
    PoeHistoryPage page = {0};
    if (history_json && !parse_poe_history_page(history_json, history_length, &page, error)) return NULL;
    CodexBarProvider *provider = poe_provider(has_balance, balance, page.entries, now_ms);
    poe_history_page_clear(&page);
    return provider;
}

static gboolean validate_poe_response(CodexBarHttpResponse *response, GError **error) {
    if (response->status == 200) return TRUE;
    if (response->status == 401 || response->status == 403) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Invalid or expired Poe API token");
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Poe API returned HTTP %ld", response->status);
    }
    return FALSE;
}

static CodexBarHttpResponse *poe_request(const char *url,
                                         const char *authorization,
                                         CodexBarAPIProviderTransport transport,
                                         GCancellable *cancellable,
                                         GError **error) {
    const CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
    };
    CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = API_TIMEOUT_SECONDS,
        .maximum_response_bytes = API_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = perform_request(&request, transport, error);
    if (!response) return NULL;
    if (validate_poe_response(response, error)) return response;
    codexbar_http_response_free(response);
    return NULL;
}

CodexBarProvider *codexbar_poe_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarAPIProviderTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    char *key = resolve_key(config, "POE_API_KEY");
    if (!key) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Missing Poe API token");
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", key);
    g_free(key);
    CodexBarHttpResponse *balance_response = poe_request(
        POE_BALANCE_URL, authorization, transport, cancellable, error);
    if (!balance_response) {
        g_free(authorization);
        return NULL;
    }
    gboolean has_balance = FALSE;
    double balance = 0;
    json_object *balance_root = parse_poe_balance(balance_response->body,
                                                  balance_response->body_length,
                                                  &has_balance,
                                                  &balance,
                                                  error);
    codexbar_http_response_free(balance_response);
    if (!balance_root) {
        g_free(authorization);
        return NULL;
    }
    json_object_put(balance_root);

    json_object *entries = json_object_new_array();
    char *cursor = NULL;
    gint64 cutoff_ms = now_ms - (gint64)POE_HISTORY_DAYS * 24 * 60 * 60 * 1000;
    for (guint page_index = 0; page_index < POE_HISTORY_PAGES; page_index++) {
        char *url = NULL;
        if (cursor) {
            char *escaped = g_uri_escape_string(cursor, NULL, FALSE);
            url = g_strdup_printf(POE_HISTORY_URL "?limit=100&starting_after=%s", escaped);
            g_free(escaped);
        } else {
            url = g_strdup(POE_HISTORY_URL "?limit=100");
        }
        GError *history_error = NULL;
        CodexBarHttpResponse *response = poe_request(url, authorization, transport, cancellable, &history_error);
        g_free(url);
        if (!response) {
            if (history_error && history_error->domain == G_IO_ERROR && history_error->code == G_IO_ERROR_CANCELLED) {
                g_propagate_error(error, history_error);
                json_object_put(entries);
                g_free(cursor);
                g_free(authorization);
                return NULL;
            }
            g_clear_error(&history_error);
            json_object_put(entries);
            entries = json_object_new_array();
            break;
        }
        PoeHistoryPage page = {0};
        gboolean parsed = parse_poe_history_page(response->body, response->body_length, &page, &history_error);
        codexbar_http_response_free(response);
        if (!parsed) {
            g_clear_error(&history_error);
            json_object_put(entries);
            entries = json_object_new_array();
            break;
        }
        size_t count = json_object_array_length(page.entries);
        for (size_t index = 0; index < count; index++) {
            json_object *entry = json_object_array_get_idx(page.entries, index);
            gint64 created_at_ms = json_object_get_int64(json_object_object_get(entry, "_createdAtMs"));
            if (created_at_ms >= cutoff_ms) json_object_array_add(entries, json_object_get(entry));
        }
        gboolean stop = page.last_created_at_ms < cutoff_ms || !page.next_cursor;
        g_free(cursor);
        cursor = g_steal_pointer(&page.next_cursor);
        poe_history_page_clear(&page);
        if (stop) break;
    }
    g_free(cursor);
    g_free(authorization);
    CodexBarProvider *provider = poe_provider(has_balance, balance, entries, now_ms);
    json_object_put(entries);
    return provider;
}

CodexBarProvider *codexbar_poe_fetch_with_transport(const CodexBarProviderConfig *config,
                                                     CodexBarAPIProviderTransport transport,
                                                     gint64 now_ms,
                                                     GError **error) {
    return codexbar_poe_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_poe_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                       GCancellable *cancellable,
                                                       GError **error) {
    return codexbar_poe_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_poe_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_poe_fetch_with_cancellable(config, NULL, error);
}

static char *normalized_key(const char *key) {
    GString *result = g_string_new(NULL);
    for (const char *cursor = key; cursor && *cursor; cursor = g_utf8_next_char(cursor)) {
        gunichar character = g_utf8_get_char(cursor);
        if (g_unichar_isalnum(character)) g_string_append_unichar(result, g_unichar_tolower(character));
    }
    return g_string_free(result, FALSE);
}

static json_object *chutes_value(json_object *object, const char *const *keys, size_t count) {
    if (!object || !json_object_is_type(object, json_type_object)) return NULL;
    for (size_t key_index = 0; key_index < count; key_index++) {
        char *wanted = normalized_key(keys[key_index]);
        json_object_object_foreach(object, candidate, value) {
            char *normalized = normalized_key(candidate);
            gboolean matches = strcmp(wanted, normalized) == 0;
            g_free(normalized);
            if (matches) {
                g_free(wanted);
                return value;
            }
        }
        g_free(wanted);
    }
    return NULL;
}

static char *chutes_string(json_object *object, const char *const *keys, size_t count) {
    json_object *value = chutes_value(object, keys, count);
    if (!value) return NULL;
    if (json_object_is_type(value, json_type_string)) return clean_value(json_object_get_string(value), 1024);
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        return g_strdup(json_object_get_string(value));
    }
    return NULL;
}

static gboolean chutes_number(json_object *object, const char *const *keys, size_t count, double *result) {
    json_object *value = chutes_value(object, keys, count);
    if (!value) return FALSE;
    if (json_object_is_type(value, json_type_string)) {
        char *text = g_strdup(json_object_get_string(value));
        g_strdelimit(text, ",$%", ' ');
        GString *compact = g_string_new(NULL);
        for (const char *cursor = text; *cursor; cursor++) {
            if (!g_ascii_isspace(*cursor)) g_string_append_c(compact, *cursor);
        }
        g_free(text);
        char *end = NULL;
        double number = g_ascii_strtod(compact->str, &end);
        gboolean valid = compact->str[0] != '\0' && end && *end == '\0' && isfinite(number);
        g_string_free(compact, TRUE);
        if (valid) *result = number;
        return valid;
    }
    return json_double_value(value, result);
}

static gboolean chutes_bool(json_object *object, const char *const *keys, size_t count, gboolean *result) {
    json_object *value = chutes_value(object, keys, count);
    if (!value) return FALSE;
    if (json_object_is_type(value, json_type_boolean) || json_object_is_type(value, json_type_int)) {
        *result = json_object_get_boolean(value);
        return TRUE;
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    char *text = clean_value(json_object_get_string(value), 32);
    if (!text) return FALSE;
    gboolean valid = TRUE;
    if (g_ascii_strcasecmp(text, "true") == 0 || strcmp(text, "1") == 0 ||
        g_ascii_strcasecmp(text, "yes") == 0 || g_ascii_strcasecmp(text, "active") == 0) {
        *result = TRUE;
    } else if (g_ascii_strcasecmp(text, "false") == 0 || strcmp(text, "0") == 0 ||
               g_ascii_strcasecmp(text, "no") == 0 || g_ascii_strcasecmp(text, "inactive") == 0 ||
               g_ascii_strcasecmp(text, "none") == 0) {
        *result = FALSE;
    } else {
        valid = FALSE;
    }
    g_free(text);
    return valid;
}

static const char *const chutes_limit_keys[] = {
    "limit", "cap", "max", "maximum", "quota", "quota_limit", "monthly_cap", "monthly_limit",
    "request_limit", "token_limit", "hard_limit", "total",
};
static const char *const chutes_used_keys[] = {
    "used", "usage", "used_amount", "consumed", "consumed_amount", "current", "current_usage",
    "requests", "request_count", "tokens", "token_usage", "monthly_usage",
};
static const char *const chutes_remaining_keys[] = {
    "remaining", "available", "balance", "left", "remaining_amount", "available_amount",
};
static const char *const chutes_used_percent_keys[] = {
    "percent_used", "usage_percent", "used_percent", "utilization", "utilization_percent",
};
static const char *const chutes_remaining_percent_keys[] = {
    "percent_remaining", "remaining_percent",
};
static const char *const chutes_label_keys[] = {
    "label", "name", "title", "type", "quota_type", "period", "window", "window_name", "chute_id",
};
static const char *const chutes_unit_keys[] = {"unit", "units", "currency", "quota_unit"};
static const char *const chutes_reset_keys[] = {
    "reset_at", "resets_at", "reset_time", "next_reset_at", "renews_at", "renewal_at", "period_end",
    "current_period_end", "expires_at", "window_end", "end_time",
};

static gboolean chutes_is_quota(json_object *object) {
    double ignored = 0;
    return chutes_number(object, chutes_limit_keys, G_N_ELEMENTS(chutes_limit_keys), &ignored) ||
           chutes_number(object, chutes_used_keys, G_N_ELEMENTS(chutes_used_keys), &ignored) ||
           chutes_number(object, chutes_remaining_keys, G_N_ELEMENTS(chutes_remaining_keys), &ignored) ||
           chutes_number(object, chutes_used_percent_keys, G_N_ELEMENTS(chutes_used_percent_keys), &ignored) ||
           chutes_number(object, chutes_remaining_percent_keys, G_N_ELEMENTS(chutes_remaining_percent_keys), &ignored);
}

static gboolean chutes_window_text(const char *raw, gint64 *minutes) {
    char *text = clean_value(raw, 128);
    if (!text) return FALSE;
    char *lower = g_ascii_strdown(text, -1);
    g_free(text);
    GString *builder = g_string_new(NULL);
    for (const char *cursor = lower; *cursor; cursor++) {
        if (!g_ascii_isspace(*cursor)) g_string_append_c(builder, *cursor);
    }
    g_free(lower);
    char *compact = g_string_free(builder, FALSE);
    char *end = NULL;
    double value = g_ascii_strtod(compact, &end);
    gboolean valid = end != compact && value > 0 && isfinite(value);
    double multiplier = 0;
    if (valid && (g_str_has_prefix(end, "min") || strcmp(end, "m") == 0)) multiplier = 1;
    else if (valid && (g_str_has_prefix(end, "hour") || g_str_has_prefix(end, "hr") || strcmp(end, "h") == 0))
        multiplier = 60;
    else if (valid && (g_str_has_prefix(end, "day") || strcmp(end, "d") == 0))
        multiplier = 24 * 60;
    else if (valid && (g_str_has_prefix(end, "month") || strcmp(end, "mo") == 0))
        multiplier = 30 * 24 * 60;
    else
        valid = FALSE;
    if (valid && value * multiplier <= (double)G_MAXINT64) *minutes = (gint64)llround(value * multiplier);
    else valid = FALSE;
    g_free(compact);
    return valid;
}

static gboolean chutes_window_minutes(json_object *object, gint64 *minutes) {
    static const char *const minute_keys[] = {"window_minutes", "period_minutes", "duration_minutes"};
    static const char *const hour_keys[] = {"window_hours", "period_hours", "duration_hours"};
    static const char *const day_keys[] = {"window_days", "period_days", "duration_days"};
    static const char *const second_keys[] = {"window_seconds", "period_seconds", "duration_seconds"};
    static const char *const text_keys[] = {"window", "window_size", "period", "duration", "interval"};
    double value = 0;
    double multiplier = 0;
    if (chutes_number(object, minute_keys, G_N_ELEMENTS(minute_keys), &value)) multiplier = 1;
    else if (chutes_number(object, hour_keys, G_N_ELEMENTS(hour_keys), &value)) multiplier = 60;
    else if (chutes_number(object, day_keys, G_N_ELEMENTS(day_keys), &value)) multiplier = 24 * 60;
    else if (chutes_number(object, second_keys, G_N_ELEMENTS(second_keys), &value)) multiplier = 1.0 / 60.0;
    if (multiplier > 0 && isfinite(value) && value * multiplier <= (double)G_MAXINT64) {
        *minutes = (gint64)llround(value * multiplier);
        return TRUE;
    }
    char *text = chutes_string(object, text_keys, G_N_ELEMENTS(text_keys));
    gboolean valid = text && chutes_window_text(text, minutes);
    g_free(text);
    return valid;
}

static void chutes_quota_clear(ChutesQuota *quota) {
    g_free(quota->label);
    g_free(quota->unit);
    memset(quota, 0, sizeof(*quota));
}

static gboolean parse_chutes_quota(json_object *object,
                                   const char *default_label,
                                   gint64 default_minutes,
                                   ChutesQuota *quota) {
    quota->label = chutes_string(object, chutes_label_keys, G_N_ELEMENTS(chutes_label_keys));
    if (!quota->label && default_label) quota->label = g_strdup(default_label);
    quota->unit = chutes_string(object, chutes_unit_keys, G_N_ELEMENTS(chutes_unit_keys));
    if (!quota->unit) quota->unit = g_strdup("credits");
    quota->has_limit = chutes_number(object, chutes_limit_keys, G_N_ELEMENTS(chutes_limit_keys), &quota->limit);
    quota->has_used = chutes_number(object, chutes_used_keys, G_N_ELEMENTS(chutes_used_keys), &quota->used);
    quota->has_remaining =
        chutes_number(object, chutes_remaining_keys, G_N_ELEMENTS(chutes_remaining_keys), &quota->remaining);
    double percent = 0;
    if (chutes_number(object, chutes_used_percent_keys, G_N_ELEMENTS(chutes_used_percent_keys), &percent)) {
        quota->has_percent = TRUE;
        quota->percent = CLAMP(fabs(percent) < 1 ? percent * 100 : percent, 0, 100);
    } else if (chutes_number(
                   object, chutes_remaining_percent_keys, G_N_ELEMENTS(chutes_remaining_percent_keys), &percent)) {
        quota->has_percent = TRUE;
        percent = CLAMP(fabs(percent) < 1 ? percent * 100 : percent, 0, 100);
        quota->percent = 100 - percent;
    }
    quota->has_window_minutes = chutes_window_minutes(object, &quota->window_minutes);
    if (!quota->has_window_minutes && default_minutes > 0) {
        quota->has_window_minutes = TRUE;
        quota->window_minutes = default_minutes;
    }
    json_object *reset = chutes_value(object, chutes_reset_keys, G_N_ELEMENTS(chutes_reset_keys));
    quota->has_resets_at = timestamp_from_json(reset, FALSE, &quota->resets_at_ms);
    if (!quota->has_limit && quota->has_used && quota->has_remaining) {
        quota->limit = quota->used + quota->remaining;
        quota->has_limit = isfinite(quota->limit);
    }
    if (!quota->has_used && quota->has_limit && quota->has_remaining) {
        quota->used = quota->limit - quota->remaining;
        quota->has_used = isfinite(quota->used);
    }
    if (!quota->has_percent && quota->has_used && quota->has_limit && quota->limit > 0) {
        quota->percent = quota->used / quota->limit * 100;
        quota->has_percent = isfinite(quota->percent);
    }
    if (quota->has_percent) quota->percent = CLAMP(quota->percent, 0, 100);
    if (quota->has_percent) return TRUE;
    chutes_quota_clear(quota);
    return FALSE;
}

static char *format_amount(double value) {
    if (fabs(value - round(value)) < 0.0001) return g_strdup_printf("%.0f", value);
    char *text = g_strdup_printf("%.2f", value);
    while (strchr(text, '.') && g_str_has_suffix(text, "0")) text[strlen(text) - 1] = '\0';
    if (g_str_has_suffix(text, ".")) text[strlen(text) - 1] = '\0';
    return text;
}

static CodexBarQuotaWindow *chutes_window(const ChutesQuota *quota, const char *id, const char *title) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, quota->label ? quota->label : title);
    window->used_percent = quota->percent;
    if (quota->has_window_minutes) {
        window->has_window_minutes = TRUE;
        window->window_minutes = quota->window_minutes;
    }
    if (quota->has_resets_at) {
        window->has_resets_at = TRUE;
        window->resets_at_ms = quota->resets_at_ms;
    }
    if (quota->has_limit && quota->limit > 0 && (quota->has_used || quota->has_remaining)) {
        double used = quota->has_used ? quota->used : MAX(0, quota->limit - quota->remaining);
        char *used_text = format_amount(used);
        char *limit_text = format_amount(quota->limit);
        window->detail = g_strdup_printf("%s/%s%s%s",
                                         used_text,
                                         limit_text,
                                         quota->unit && *quota->unit ? " " : "",
                                         quota->unit ? quota->unit : "");
        g_free(used_text);
        g_free(limit_text);
    }
    return window;
}

static int chutes_kind(const ChutesQuota *quota) {
    char *label = g_ascii_strdown(quota->label ? quota->label : "", -1);
    char *unit = g_ascii_strdown(quota->unit ? quota->unit : "", -1);
    char *combined = g_strdup_printf("%s %s", label, unit);
    g_free(label);
    g_free(unit);
    gboolean rolling = strstr(combined, "rolling") || strstr(combined, "4h") || strstr(combined, "4 h") ||
                       strstr(combined, "4-hour") || strstr(combined, "four hour") ||
                       strstr(combined, "four-hour") ||
                       (quota->has_window_minutes && quota->window_minutes == 240);
    gboolean monthly = strstr(combined, "month") || strstr(combined, "billing") ||
                       strstr(combined, "subscription") ||
                       (quota->has_window_minutes && quota->window_minutes >= 28 * 24 * 60);
    g_free(combined);
    return rolling ? 1 : (monthly ? 2 : 0);
}

static void collect_quota_objects(json_object *value, GPtrArray *objects, GHashTable *seen, guint depth) {
    if (!value || depth > 16) return;
    if (json_object_is_type(value, json_type_array)) {
        size_t count = json_object_array_length(value);
        for (size_t index = 0; index < count; index++) {
            collect_quota_objects(json_object_array_get_idx(value, index), objects, seen, depth + 1);
        }
        return;
    }
    if (!json_object_is_type(value, json_type_object)) return;
    if (chutes_is_quota(value)) {
        const char *serialized = json_object_to_json_string_ext(value, JSON_C_TO_STRING_PLAIN);
        if (!g_hash_table_contains(seen, serialized)) {
            g_hash_table_add(seen, g_strdup(serialized));
            g_ptr_array_add(objects, value);
        }
    }
    json_object_object_foreach(value, key, child) {
        (void)key;
        collect_quota_objects(child, objects, seen, depth + 1);
    }
}

static json_object *chutes_dictionary(json_object *object, const char *const *keys, size_t count) {
    json_object *value = chutes_value(object, keys, count);
    return value && json_object_is_type(value, json_type_object) ? value : NULL;
}

static ChutesSubscriptionState chutes_subscription_state(json_object *root,
                                                         json_object *data,
                                                         json_object *subscription) {
    static const char *const active_keys[] = {
        "active", "is_active", "subscription_active", "has_subscription",
    };
    gboolean active = FALSE;
    if (chutes_bool(root, active_keys, G_N_ELEMENTS(active_keys), &active) ||
        chutes_bool(data, active_keys, G_N_ELEMENTS(active_keys), &active) ||
        chutes_bool(subscription, active_keys, G_N_ELEMENTS(active_keys), &active)) {
        return active ? CHUTES_SUBSCRIPTION_ACTIVE : CHUTES_SUBSCRIPTION_INACTIVE;
    }
    static const char *const status_keys[] = {"status", "state", "subscription_status"};
    char *status = chutes_string(root, status_keys, G_N_ELEMENTS(status_keys));
    if (!status) status = chutes_string(data, status_keys, G_N_ELEMENTS(status_keys));
    if (!status) status = chutes_string(subscription, status_keys, G_N_ELEMENTS(status_keys));
    if (!status) return CHUTES_SUBSCRIPTION_UNKNOWN;
    char *lower = g_ascii_strdown(status, -1);
    g_free(status);
    ChutesSubscriptionState state = CHUTES_SUBSCRIPTION_UNKNOWN;
    if (strstr(lower, "active") && !strstr(lower, "inactive")) state = CHUTES_SUBSCRIPTION_ACTIVE;
    else if (strstr(lower, "free") || strstr(lower, "inactive") || strstr(lower, "cancel") ||
             strstr(lower, "none") || strstr(lower, "expired"))
        state = CHUTES_SUBSCRIPTION_INACTIVE;
    g_free(lower);
    return state;
}

static CodexBarProvider *parse_chutes_root(json_object *root, gint64 now_ms) {
    static const char *const data_keys[] = {"data", "result"};
    static const char *const subscription_keys[] = {
        "subscription", "subscription_usage", "current_subscription", "plan",
    };
    static const char *const rolling_keys[] = {
        "rolling", "rolling_window", "rolling_4h", "four_hour", "four_hour_usage", "window_4h",
    };
    static const char *const monthly_keys[] = {
        "monthly", "monthly_usage", "subscription", "subscription_usage", "billing_period",
    };
    static const char *const plan_keys[] = {
        "plan_name", "plan", "tier", "subscription_plan", "subscription_tier",
    };
    json_object *data = chutes_dictionary(root, data_keys, G_N_ELEMENTS(data_keys));
    if (!data) data = root;
    json_object *subscription = chutes_dictionary(root, subscription_keys, G_N_ELEMENTS(subscription_keys));
    if (!subscription) subscription = chutes_dictionary(data, subscription_keys, G_N_ELEMENTS(subscription_keys));
    json_object *rolling_object = chutes_dictionary(root, rolling_keys, G_N_ELEMENTS(rolling_keys));
    if (!rolling_object) rolling_object = chutes_dictionary(data, rolling_keys, G_N_ELEMENTS(rolling_keys));
    json_object *monthly_object = chutes_dictionary(root, monthly_keys, G_N_ELEMENTS(monthly_keys));
    if (!monthly_object) monthly_object = chutes_dictionary(data, monthly_keys, G_N_ELEMENTS(monthly_keys));

    ChutesQuota rolling = {0};
    ChutesQuota monthly = {0};
    gboolean has_rolling = rolling_object && parse_chutes_quota(rolling_object, "4-hour quota", 240, &rolling);
    gboolean has_monthly = monthly_object && parse_chutes_quota(monthly_object, "Monthly quota", 43200, &monthly);
    GPtrArray *objects = g_ptr_array_new();
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    collect_quota_objects(root, objects, seen, 0);
    GPtrArray *fallback = g_ptr_array_new_with_free_func(g_free);
    for (guint index = 0; index < objects->len; index++) {
        json_object *object = g_ptr_array_index(objects, index);
        if (object == rolling_object || object == monthly_object) continue;
        ChutesQuota *quota = g_new0(ChutesQuota, 1);
        if (!parse_chutes_quota(object, NULL, 0, quota)) {
            g_free(quota);
            continue;
        }
        int kind = chutes_kind(quota);
        if (!has_rolling && kind == 1) {
            rolling = *quota;
            has_rolling = TRUE;
            g_free(quota);
        } else if (!has_monthly && kind == 2) {
            monthly = *quota;
            has_monthly = TRUE;
            g_free(quota);
        } else {
            g_ptr_array_add(fallback, quota);
        }
    }
    g_ptr_array_unref(objects);
    g_hash_table_unref(seen);

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("chutes");
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    if (has_rolling) codexbar_provider_add_quota_window(provider, chutes_window(&rolling, "primary", "4-hour quota"));
    if (has_monthly) codexbar_provider_add_quota_window(provider, chutes_window(&monthly, "secondary", "Monthly quota"));
    guint fallback_index = 0;
    if (!has_rolling && !has_monthly && fallback_index < fallback->len) {
        ChutesQuota *quota = g_ptr_array_index(fallback, fallback_index++);
        codexbar_provider_add_quota_window(provider, chutes_window(quota, "primary", "Quota"));
    }
    if (!has_monthly && fallback_index < fallback->len && (has_rolling || fallback_index > 0)) {
        ChutesQuota *quota = g_ptr_array_index(fallback, fallback_index++);
        codexbar_provider_add_quota_window(provider, chutes_window(quota, "secondary", "Quota"));
    }
    for (guint index = fallback_index; index < fallback->len; index++) {
        ChutesQuota *quota = g_ptr_array_index(fallback, index);
        char *id = g_strdup_printf("quota-%u", index + 1);
        codexbar_provider_add_quota_window(provider, chutes_window(quota, id, "Quota"));
        g_free(id);
    }
    for (guint index = 0; index < fallback->len; index++) {
        ChutesQuota *quota = g_ptr_array_index(fallback, index);
        chutes_quota_clear(quota);
    }
    g_ptr_array_unref(fallback);
    ChutesSubscriptionState state = chutes_subscription_state(root, data, subscription);
    char *plan = chutes_string(root, plan_keys, G_N_ELEMENTS(plan_keys));
    if (!plan) plan = chutes_string(data, plan_keys, G_N_ELEMENTS(plan_keys));
    if (!plan) plan = chutes_string(subscription, plan_keys, G_N_ELEMENTS(plan_keys));
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    if (plan) provider->identity->login_method = plan;
    else if (state == CHUTES_SUBSCRIPTION_INACTIVE)
        provider->identity->login_method = g_strdup("No active subscription");
    else if (state == CHUTES_SUBSCRIPTION_UNKNOWN && provider->quota_windows->len == 0)
        provider->identity->login_method = g_strdup("No usage data");
    json_object *renewal = chutes_value(root, chutes_reset_keys, G_N_ELEMENTS(chutes_reset_keys));
    if (!renewal) renewal = chutes_value(data, chutes_reset_keys, G_N_ELEMENTS(chutes_reset_keys));
    if (!renewal) renewal = chutes_value(subscription, chutes_reset_keys, G_N_ELEMENTS(chutes_reset_keys));
    provider->has_subscription_renews_at = timestamp_from_json(renewal, FALSE, &provider->subscription_renews_at_ms);
    if (!provider->has_subscription_renews_at && has_monthly && monthly.has_resets_at) {
        provider->has_subscription_renews_at = TRUE;
        provider->subscription_renews_at_ms = monthly.resets_at_ms;
    }
    add_confidence(provider, "exact");
    chutes_quota_clear(&rolling);
    chutes_quota_clear(&monthly);
    return provider;
}

CodexBarProvider *codexbar_chutes_parse_usage_bytes(const char *json,
                                                     size_t length,
                                                     gint64 now_ms,
                                                     GError **error) {
    json_object *parsed = parse_json_document(json, length);
    if (!parsed || (!json_object_is_type(parsed, json_type_object) &&
                    !json_object_is_type(parsed, json_type_array))) {
        if (parsed) json_object_put(parsed);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse Chutes usage response");
        return NULL;
    }
    json_object *root = parsed;
    if (json_object_is_type(parsed, json_type_array)) {
        root = json_object_new_object();
        json_object_object_add(root, "quotas", json_object_get(parsed));
    }
    CodexBarProvider *provider = parse_chutes_root(root, now_ms);
    if (root != parsed) json_object_put(root);
    json_object_put(parsed);
    return provider;
}

static gboolean validate_chutes_response(CodexBarHttpResponse *response, GError **error) {
    if (response->status >= 200 && response->status < 300) return TRUE;
    if (response->status == 401 || response->status == 403) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Chutes API key was rejected");
    } else {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Chutes usage API returned HTTP %ld", response->status);
    }
    return FALSE;
}

static CodexBarHttpResponse *chutes_request(const char *url,
                                            const char *authorization,
                                            CodexBarAPIProviderTransport transport,
                                            GCancellable *cancellable,
                                            GError **error) {
    const CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
    };
    CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = API_TIMEOUT_SECONDS,
        .maximum_response_bytes = API_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = perform_request(&request, transport, error);
    if (!response) return NULL;
    if (validate_chutes_response(response, error)) return response;
    codexbar_http_response_free(response);
    return NULL;
}

static json_object *chutes_quota_definitions(json_object *root) {
    if (json_object_is_type(root, json_type_array)) return json_object_get(root);
    if (!json_object_is_type(root, json_type_object)) return json_object_new_array();
    static const char *const quota_keys[] = {"quotas"};
    static const char *const data_keys[] = {"data"};
    json_object *quotas = chutes_value(root, quota_keys, G_N_ELEMENTS(quota_keys));
    if (quotas && json_object_is_type(quotas, json_type_array)) return json_object_get(quotas);
    json_object *data = chutes_value(root, data_keys, G_N_ELEMENTS(data_keys));
    if (data && json_object_is_type(data, json_type_array)) return json_object_get(data);
    if (data && json_object_is_type(data, json_type_object)) {
        quotas = chutes_value(data, quota_keys, G_N_ELEMENTS(quota_keys));
        if (quotas && json_object_is_type(quotas, json_type_array)) return json_object_get(quotas);
    }
    return json_object_new_array();
}

static char *chutes_quota_id(json_object *definition) {
    static const char *const id_keys[] = {"chute_id", "id"};
    return chutes_string(definition, id_keys, G_N_ELEMENTS(id_keys));
}

static json_object *chutes_response_dictionary(json_object *root) {
    if (!root || !json_object_is_type(root, json_type_object)) return NULL;
    static const char *const keys[] = {"data", "result"};
    json_object *nested = chutes_value(root, keys, G_N_ELEMENTS(keys));
    if (nested && json_object_is_type(nested, json_type_object)) return nested;
    return root;
}

static json_object *merged_json_object(json_object *base, json_object *overlay) {
    json_object *result = json_object_new_object();
    json_object_object_foreach(base, base_key, base_value) {
        json_object_object_add(result, base_key, json_object_get(base_value));
    }
    json_object_object_foreach(overlay, overlay_key, overlay_value) {
        json_object_object_add(result, overlay_key, json_object_get(overlay_value));
    }
    return result;
}

static CodexBarQuotaWindow *clone_window(const CodexBarQuotaWindow *source) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new(source->id, source->title);
    g_free(window->output_id);
    window->output_id = g_strdup(source->output_id);
    window->usage_known = source->usage_known;
    window->used_percent = source->used_percent;
    window->has_window_minutes = source->has_window_minutes;
    window->window_minutes = source->window_minutes;
    window->has_resets_at = source->has_resets_at;
    window->resets_at_ms = source->resets_at_ms;
    window->detail = g_strdup(source->detail);
    window->reset_description = g_strdup(source->reset_description);
    return window;
}

static gboolean provider_has_window(const CodexBarProvider *provider, const char *id) {
    for (guint index = 0; index < provider->quota_windows->len; index++) {
        if (g_str_equal(codexbar_provider_quota_window(provider, index)->id, id)) return TRUE;
    }
    return FALSE;
}

static void merge_chutes_windows(CodexBarProvider *subscription, const CodexBarProvider *quotas) {
    for (guint index = 0; index < quotas->quota_windows->len; index++) {
        const CodexBarQuotaWindow *window = codexbar_provider_quota_window(quotas, index);
        if (!provider_has_window(subscription, window->id)) {
            codexbar_provider_add_quota_window(subscription, clone_window(window));
        }
    }
}

CodexBarProvider *codexbar_chutes_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarAPIProviderTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    char *key = resolve_key(config, "CHUTES_API_KEY");
    if (!key) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Missing Chutes API key");
        return NULL;
    }
    char *base = validated_base_url("CHUTES_API_URL", CHUTES_DEFAULT_BASE_URL, error);
    if (!base) {
        g_free(key);
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", key);
    g_free(key);
    char *url = append_path(base, "users/me/subscription_usage");
    CodexBarHttpResponse *response = chutes_request(url, authorization, transport, cancellable, error);
    g_free(url);
    if (!response) goto failure;
    CodexBarProvider *subscription = codexbar_chutes_parse_usage_bytes(
        response->body, response->body_length, now_ms, error);
    codexbar_http_response_free(response);
    if (!subscription) goto failure;
    if (provider_has_window(subscription, "primary") && provider_has_window(subscription, "secondary")) {
        g_free(authorization);
        g_free(base);
        return subscription;
    }

    url = append_path(base, "users/me/quotas");
    GError *quota_error = NULL;
    response = chutes_request(url, authorization, transport, cancellable, &quota_error);
    g_free(url);
    if (!response) {
        if (quota_error && quota_error->domain == G_IO_ERROR &&
            (quota_error->code == G_IO_ERROR_CANCELLED || quota_error->code == G_IO_ERROR_PERMISSION_DENIED)) {
            codexbar_provider_free(subscription);
            g_propagate_error(error, quota_error);
            goto failure;
        }
        g_clear_error(&quota_error);
        g_free(authorization);
        g_free(base);
        return subscription;
    }
    json_object *quota_root = parse_json_document(response->body, response->body_length);
    codexbar_http_response_free(response);
    if (!quota_root) {
        g_free(authorization);
        g_free(base);
        return subscription;
    }
    json_object *definitions = chutes_quota_definitions(quota_root);
    json_object *enriched = json_object_new_array();
    size_t definition_count = json_object_array_length(definitions);
    for (size_t index = 0; index < definition_count; index++) {
        json_object *definition = json_object_array_get_idx(definitions, index);
        if (!definition || !json_object_is_type(definition, json_type_object)) continue;
        json_object *merged = json_object_get(definition);
        char *id = chutes_quota_id(definition);
        if (id) {
            char *escaped = g_uri_escape_string(id, NULL, FALSE);
            char *path = g_strdup_printf("users/me/quota_usage/%s", escaped);
            char *usage_url = append_path(base, path);
            g_free(path);
            g_free(escaped);
            GError *usage_error = NULL;
            CodexBarHttpResponse *usage_response =
                chutes_request(usage_url, authorization, transport, cancellable, &usage_error);
            g_free(usage_url);
            if (!usage_response && usage_error && usage_error->domain == G_IO_ERROR &&
                (usage_error->code == G_IO_ERROR_CANCELLED || usage_error->code == G_IO_ERROR_PERMISSION_DENIED)) {
                g_free(id);
                json_object_put(merged);
                json_object_put(enriched);
                json_object_put(definitions);
                json_object_put(quota_root);
                codexbar_provider_free(subscription);
                g_propagate_error(error, usage_error);
                goto failure;
            }
            if (usage_response) {
                json_object *usage_root = parse_json_document(usage_response->body, usage_response->body_length);
                json_object *usage = chutes_response_dictionary(usage_root);
                if (usage) {
                    json_object *combined = merged_json_object(definition, usage);
                    json_object_put(merged);
                    merged = combined;
                }
                if (usage_root) json_object_put(usage_root);
                codexbar_http_response_free(usage_response);
            }
            g_clear_error(&usage_error);
        }
        g_free(id);
        json_object_array_add(enriched, merged);
    }
    CodexBarProvider *quotas = NULL;
    if (definition_count > 0) {
        json_object *root = json_object_new_object();
        json_object_object_add(root, "quotas", json_object_get(enriched));
        const char *text = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PLAIN);
        quotas = codexbar_chutes_parse_usage_bytes(text, strlen(text), now_ms, NULL);
        json_object_put(root);
    } else {
        const char *text = json_object_to_json_string_ext(quota_root, JSON_C_TO_STRING_PLAIN);
        quotas = codexbar_chutes_parse_usage_bytes(text, strlen(text), now_ms, NULL);
    }
    json_object_put(enriched);
    json_object_put(definitions);
    json_object_put(quota_root);
    if (quotas && quotas->quota_windows->len > 0) merge_chutes_windows(subscription, quotas);
    codexbar_provider_free(quotas);
    g_free(authorization);
    g_free(base);
    return subscription;

failure:
    g_free(authorization);
    g_free(base);
    return NULL;
}

CodexBarProvider *codexbar_chutes_fetch_with_transport(const CodexBarProviderConfig *config,
                                                        CodexBarAPIProviderTransport transport,
                                                        gint64 now_ms,
                                                        GError **error) {
    return codexbar_chutes_fetch_with_transport_and_cancellable(config, transport, NULL, now_ms, error);
}

CodexBarProvider *codexbar_chutes_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error) {
    return codexbar_chutes_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_chutes_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_chutes_fetch_with_cancellable(config, NULL, error);
}
