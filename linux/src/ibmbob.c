#include "ibmbob.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define IBMBOB_BASE_URL "https://api.us-east.bob.ibm.com"
#define IBMBOB_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)
#define IBMBOB_MAXIMUM_TEXT_BYTES 65536U

static char *clean_value(const char *raw) {
    if (!raw) return NULL;
    char *clean = g_strstrip(g_strdup(raw));
    size_t length = strlen(clean);
    if (length >= 2 && ((clean[0] == '\'' && clean[length - 1] == '\'') ||
                        (clean[0] == '"' && clean[length - 1] == '"'))) {
        clean[length - 1] = '\0';
        memmove(clean, clean + 1, length - 1);
        g_strstrip(clean);
        length = strlen(clean);
    }
    if (length == 0 || length > IBMBOB_MAXIMUM_TEXT_BYTES || !g_utf8_validate(clean, -1, NULL)) {
        g_free(clean);
        return NULL;
    }
    for (const unsigned char *cursor = (const unsigned char *)clean; *cursor; cursor++) {
        if (*cursor < 32 || *cursor == 127) {
            g_free(clean);
            return NULL;
        }
    }
    return clean;
}

static char *resolve_api_key(const CodexBarProviderConfig *config) {
    char *key = clean_value(config ? config->api_key : NULL);
    if (!key) key = clean_value(g_getenv("BOBSHELL_API_KEY"));
    return key;
}

static json_object *parse_json_document(const char *json, size_t length) {
    if (!json || length > G_MAXINT || !g_utf8_validate(json, (gssize)length, NULL)) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, json, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && g_ascii_isspace((guchar)json[consumed])) consumed++;
    gboolean valid = parse_error == json_tokener_success && root && consumed == length;
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static gboolean json_string(json_object *object, const char *name, gboolean required, char **result) {
    json_object *value = NULL;
    *result = NULL;
    if (!json_object_object_get_ex(object, name, &value) || json_object_is_type(value, json_type_null)) {
        return !required;
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (memchr(text, '\0', length) || length > IBMBOB_MAXIMUM_TEXT_BYTES ||
        !g_utf8_validate(text, (gssize)length, NULL)) {
        return FALSE;
    }
    for (size_t index = 0; index < length; index++) {
        unsigned char character = (unsigned char)text[index];
        if (character < 32 || character == 127) return FALSE;
    }
    *result = clean_value(text);
    return TRUE;
}

static gboolean optional_number(json_object *object, const char *name, gboolean *present, double *result) {
    json_object *value = NULL;
    *present = FALSE;
    if (!json_object_object_get_ex(object, name, &value) || json_object_is_type(value, json_type_null)) return TRUE;
    if (!json_object_is_type(value, json_type_int) && !json_object_is_type(value, json_type_double)) return FALSE;
    *result = json_object_get_double(value);
    *present = isfinite(*result);
    return *present;
}

static gboolean parse_refresh_at(json_object *instance, gboolean *present, gint64 *result) {
    json_object *value = NULL;
    *present = FALSE;
    if (!json_object_object_get_ex(instance, "refresh_at", &value) || json_object_is_type(value, json_type_null)) {
        return TRUE;
    }
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        double seconds = json_object_get_double(value);
        if (!isfinite(seconds)) return FALSE;
        if (!(seconds > 0.0) || seconds > (double)G_MAXINT64 / 1000.0) return TRUE;
        *present = TRUE;
        *result = (gint64)(seconds * 1000.0);
        return TRUE;
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    char *text = NULL;
    if (!json_string(instance, "refresh_at", FALSE, &text)) return FALSE;
    if (!text) return TRUE;
    GDateTime *date = g_date_time_new_from_iso8601(text, NULL);
    g_free(text);
    if (!date) return TRUE;
    *present = TRUE;
    *result = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static gboolean valid_dns_host(const char *host) {
    size_t length = host ? strlen(host) : 0;
    if (length == 0 || length > 253 || host[0] == '.' || host[length - 1] == '.') return FALSE;
    size_t label_length = 0;
    for (size_t index = 0; index <= length; index++) {
        unsigned char character = (unsigned char)host[index];
        if (character == '.' || character == '\0') {
            if (label_length == 0 || label_length > 63 || host[index - label_length] == '-' ||
                host[index - 1] == '-') {
                return FALSE;
            }
            label_length = 0;
            continue;
        }
        if (!g_ascii_isalnum(character) && character != '-') return FALSE;
        label_length++;
    }
    return TRUE;
}

static char *regional_base_url(const char *region_domain, GError **error) {
    if (!region_domain) return g_strdup(IBMBOB_BASE_URL);
    char *domain = clean_value(region_domain);
    if (!domain) return g_strdup(IBMBOB_BASE_URL);
    char *lower = g_ascii_strdown(domain, -1);
    g_free(domain);
    char *host = g_str_has_prefix(lower, "api.") ? g_strdup(lower) : g_strdup_printf("api.%s", lower);
    g_free(lower);
    size_t host_length = strlen(host);
    const char *suffix = "bob.ibm.com";
    size_t suffix_length = strlen(suffix);
    gboolean trusted_suffix = host_length == suffix_length && g_str_equal(host, suffix);
    if (host_length > suffix_length && host[host_length - suffix_length - 1] == '.' &&
        g_str_has_suffix(host, suffix)) {
        trusted_suffix = TRUE;
    }
    if (!trusted_suffix || !valid_dns_host(host)) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "IBM Bob returned an untrusted regional API host: %s.",
                    host);
        g_free(host);
        return NULL;
    }
    char *url = g_strdup_printf("https://%s", host);
    GUri *uri = g_uri_parse(url, G_URI_FLAGS_ENCODED, NULL);
    gboolean valid = uri && g_uri_get_scheme(uri) && g_ascii_strcasecmp(g_uri_get_scheme(uri), "https") == 0 &&
                     g_uri_get_host(uri) && g_ascii_strcasecmp(g_uri_get_host(uri), host) == 0 &&
                     g_uri_get_userinfo(uri) == NULL && g_uri_get_port(uri) == -1 &&
                     (!g_uri_get_path(uri) || g_uri_get_path(uri)[0] == '\0') && g_uri_get_query(uri) == NULL &&
                     g_uri_get_fragment(uri) == NULL;
    if (uri) g_uri_unref(uri);
    if (!valid) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_INVALID_ARGUMENT,
                    "IBM Bob returned an untrusted regional API host: %s.",
                    host);
        g_free(url);
        url = NULL;
    }
    g_free(host);
    return url;
}

static gboolean base64url_segment(const char *segment) {
    if (!segment || segment[0] == '\0') return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)segment; *cursor; cursor++) {
        if (!g_ascii_isalnum(*cursor) && *cursor != '-' && *cursor != '_') return FALSE;
    }
    return TRUE;
}

static gboolean token_is_jwt(const char *token) {
    char **parts = g_strsplit(token, ".", -1);
    gboolean valid = g_strv_length(parts) == 3 && base64url_segment(parts[0]) &&
                     base64url_segment(parts[1]) && strlen(parts[1]) % 4 != 1 &&
                     base64url_segment(parts[2]);
    if (!valid) {
        g_strfreev(parts);
        return FALSE;
    }
    char *payload = g_strdup(parts[1]);
    for (char *cursor = payload; *cursor; cursor++) {
        if (*cursor == '-') *cursor = '+';
        else if (*cursor == '_') *cursor = '/';
    }
    size_t payload_length = strlen(payload);
    size_t padding = (4 - payload_length % 4) % 4;
    payload = g_realloc(payload, payload_length + padding + 1);
    for (size_t index = 0; index < padding; index++) payload[payload_length + index] = '=';
    payload[payload_length + padding] = '\0';
    gsize decoded_length = 0;
    guchar *decoded = g_base64_decode(payload, &decoded_length);
    json_object *object = decoded ? parse_json_document((const char *)decoded, decoded_length) : NULL;
    valid = object && json_object_is_type(object, json_type_object);
    if (object) json_object_put(object);
    g_free(decoded);
    g_free(payload);
    g_strfreev(parts);
    return valid;
}

static char *authorization_value(const char *token) {
    return g_strdup_printf(token_is_jwt(token) ? "Bearer %s" : "Apikey %s", token);
}

static CodexBarHttpResponse *send_request(const char *url,
                                          const char *authorization,
                                          const char *instance_id,
                                          const char *team_id,
                                          CodexBarIBMBobTransport transport,
                                          GCancellable *cancellable,
                                          GError **error) {
    CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"User-Agent", "CodexBar"},
        {"x-instance-id", instance_id},
        {"x-team-id", team_id},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = instance_id && team_id ? G_N_ELEMENTS(headers) : 4,
        .timeout_seconds = 20,
        .maximum_response_bytes = IBMBOB_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    GError *request_error = NULL;
    CodexBarHttpResponse *response = transport(&request, &request_error);
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        codexbar_http_response_free(response);
        g_clear_error(&request_error);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        return NULL;
    }
    if (!response) {
        if (request_error && request_error->domain == G_IO_ERROR && request_error->code == G_IO_ERROR_CANCELLED) {
            g_propagate_error(error, request_error);
        } else {
            g_clear_error(&request_error);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "IBM Bob network request failed.");
        }
        return NULL;
    }
    g_clear_error(&request_error);
    if (response->status == 401 || response->status == 403) {
        codexbar_http_response_free(response);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "IBM Bob rejected the API key.");
        return NULL;
    }
    if (response->status < 200 || response->status >= 300) {
        long status = response->status;
        codexbar_http_response_free(response);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "IBM Bob API returned HTTP %ld.", status);
        return NULL;
    }
    return response;
}

static gboolean profile_is_valid(json_object *root, json_object **instances, GError **error) {
    if (!root || !json_object_is_type(root, json_type_object) ||
        !json_object_object_get_ex(root, "instances", instances) ||
        !json_object_is_type(*instances, json_type_array)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "IBM Bob profile response is malformed.");
        return FALSE;
    }
    for (size_t instance_index = 0; instance_index < json_object_array_length(*instances); instance_index++) {
        json_object *instance = json_object_array_get_idx(*instances, instance_index);
        json_object *teams = NULL;
        char *instance_id = NULL;
        char *instance_name = NULL;
        char *legacy_name = NULL;
        char *user_id = NULL;
        char *plan_name = NULL;
        char *region_domain = NULL;
        gboolean has_reset = FALSE;
        gint64 reset_ms = 0;
        gboolean valid = instance && json_object_is_type(instance, json_type_object) &&
                         json_string(instance, "instance_id", TRUE, &instance_id) &&
                         json_string(instance, "instance_name", FALSE, &instance_name) &&
                         json_string(instance, "name", FALSE, &legacy_name) &&
                         json_string(instance, "user_id", FALSE, &user_id) &&
                         json_string(instance, "plan_name", FALSE, &plan_name) &&
                         json_string(instance, "region_domain", FALSE, &region_domain) &&
                         parse_refresh_at(instance, &has_reset, &reset_ms) &&
                         json_object_object_get_ex(instance, "teams", &teams) &&
                         json_object_is_type(teams, json_type_array);
        gboolean has_usable_team = FALSE;
        for (size_t team_index = 0; valid && team_index < json_object_array_length(teams); team_index++) {
            json_object *team = json_object_array_get_idx(teams, team_index);
            char *team_id = NULL;
            char *team_name = NULL;
            gboolean has_budget = FALSE;
            double budget = 0.0;
            valid = team && json_object_is_type(team, json_type_object) &&
                    json_string(team, "id", TRUE, &team_id) &&
                    json_string(team, "name", FALSE, &team_name) &&
                    optional_number(team, "budget_limit", &has_budget, &budget);
            has_usable_team = has_usable_team || (team_id && team_id[0] != '\0');
            g_free(team_name);
            g_free(team_id);
        }
        if (valid && instance_id && user_id && has_usable_team) {
            char *base_url = regional_base_url(region_domain, error);
            valid = base_url != NULL;
            g_free(base_url);
        }
        g_free(region_domain);
        g_free(plan_name);
        g_free(user_id);
        g_free(legacy_name);
        g_free(instance_name);
        g_free(instance_id);
        if (!valid) {
            if (!error || !*error) {
                g_set_error_literal(error,
                                    G_IO_ERROR,
                                    G_IO_ERROR_INVALID_DATA,
                                    "IBM Bob profile response is malformed.");
            }
            return FALSE;
        }
    }
    return TRUE;
}

static gboolean add_unique_plan(GPtrArray *plans, const char *plan) {
    if (!plan) return FALSE;
    for (guint index = 0; index < plans->len; index++) {
        if (g_str_equal(g_ptr_array_index(plans, index), plan)) return FALSE;
    }
    g_ptr_array_add(plans, g_strdup(plan));
    return TRUE;
}

static gint compare_strings(gconstpointer left, gconstpointer right) {
    return g_strcmp0(*(char *const *)left, *(char *const *)right);
}

static char *joined_plans(GPtrArray *plans) {
    if (plans->len == 0) return NULL;
    g_ptr_array_sort(plans, compare_strings);
    GString *joined = g_string_new(NULL);
    for (guint index = 0; index < plans->len; index++) {
        if (index > 0) g_string_append(joined, ", ");
        g_string_append(joined, g_ptr_array_index(plans, index));
    }
    return g_string_free(joined, FALSE);
}

static char *format_bobcoins(double value) {
    return trunc(value) == value ? g_strdup_printf("%.0f", value) : g_strdup_printf("%.2f", value);
}

static gint64 calendar_month_minutes(gint64 now_ms) {
    GDateTime *now = g_date_time_new_from_unix_utc(now_ms / 1000);
    if (!now) return 0;
    guint8 days = g_date_get_days_in_month((GDateMonth)g_date_time_get_month(now),
                                           (GDateYear)g_date_time_get_year(now));
    g_date_time_unref(now);
    return (gint64)days * 24 * 60;
}

CodexBarProvider *codexbar_ibmbob_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                        CodexBarIBMBobTransport transport,
                                                                        GCancellable *cancellable,
                                                                        gint64 now_ms,
                                                                        GError **error) {
    g_return_val_if_fail(transport != NULL, NULL);
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) return NULL;
    char *api_key = resolve_api_key(config);
    if (!api_key) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "Missing IBM Bob API key. Set apiKey or BOBSHELL_API_KEY.");
        return NULL;
    }
    char *authorization = authorization_value(api_key);
    g_free(api_key);
    char *profile_url = g_strdup_printf("%s/admin/v1/profile", IBMBOB_BASE_URL);
    CodexBarHttpResponse *profile_response = send_request(
        profile_url, authorization, NULL, NULL, transport, cancellable, error);
    g_free(profile_url);
    if (!profile_response) {
        g_free(authorization);
        return NULL;
    }
    json_object *profile = parse_json_document(profile_response->body, profile_response->body_length);
    codexbar_http_response_free(profile_response);
    json_object *instances = NULL;
    if (!profile_is_valid(profile, &instances, error)) {
        if (profile) json_object_put(profile);
        g_free(authorization);
        return NULL;
    }

    json_object *team_details = json_object_new_array();
    GPtrArray *plans = g_ptr_array_new_with_free_func(g_free);
    double total_used = 0.0;
    double total_limit = 0.0;
    gboolean all_limits_known = TRUE;
    gboolean has_reset = FALSE;
    gint64 earliest_reset_ms = 0;
    guint usable_teams = 0;

    for (size_t instance_index = 0; instance_index < json_object_array_length(instances); instance_index++) {
        if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) goto fail;
        json_object *instance = json_object_array_get_idx(instances, instance_index);
        json_object *teams = NULL;
        json_object_object_get_ex(instance, "teams", &teams);
        char *instance_id = NULL;
        char *instance_name = NULL;
        char *legacy_name = NULL;
        char *user_id = NULL;
        char *plan_name = NULL;
        char *region_domain = NULL;
        json_string(instance, "instance_id", TRUE, &instance_id);
        json_string(instance, "instance_name", FALSE, &instance_name);
        json_string(instance, "name", FALSE, &legacy_name);
        json_string(instance, "user_id", FALSE, &user_id);
        json_string(instance, "plan_name", FALSE, &plan_name);
        json_string(instance, "region_domain", FALSE, &region_domain);
        gboolean instance_has_reset = FALSE;
        gint64 instance_reset_ms = 0;
        parse_refresh_at(instance, &instance_has_reset, &instance_reset_ms);
        if (!instance_id || !user_id) {
            g_free(region_domain);
            g_free(plan_name);
            g_free(user_id);
            g_free(legacy_name);
            g_free(instance_name);
            g_free(instance_id);
            continue;
        }
        char *base_url = regional_base_url(region_domain, error);
        g_free(region_domain);
        if (!base_url) {
            g_free(plan_name);
            g_free(user_id);
            g_free(legacy_name);
            g_free(instance_name);
            g_free(instance_id);
            goto fail;
        }
        const char *display_instance = instance_name ? instance_name : legacy_name ? legacy_name : instance_id;
        for (size_t team_index = 0; team_index < json_object_array_length(teams); team_index++) {
            if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) {
                g_free(base_url);
                g_free(plan_name);
                g_free(user_id);
                g_free(legacy_name);
                g_free(instance_name);
                g_free(instance_id);
                goto fail;
            }
            json_object *team = json_object_array_get_idx(teams, team_index);
            char *team_id = NULL;
            char *team_name = NULL;
            json_string(team, "id", TRUE, &team_id);
            json_string(team, "name", FALSE, &team_name);
            gboolean profile_has_limit = FALSE;
            double profile_limit = 0.0;
            optional_number(team, "budget_limit", &profile_has_limit, &profile_limit);
            if (!team_id) {
                g_free(team_name);
                continue;
            }
            char *escaped_team = g_uri_escape_string(team_id, NULL, FALSE);
            char *escaped_user = g_uri_escape_string(user_id, NULL, FALSE);
            char *url = g_strdup_printf("%s/admin/v1/teams/%s/users/%s", base_url, escaped_team, escaped_user);
            g_free(escaped_user);
            g_free(escaped_team);
            CodexBarHttpResponse *response = send_request(
                url, authorization, instance_id, team_id, transport, cancellable, error);
            g_free(url);
            if (!response) {
                g_free(team_name);
                g_free(team_id);
                g_free(base_url);
                g_free(plan_name);
                g_free(user_id);
                g_free(legacy_name);
                g_free(instance_name);
                g_free(instance_id);
                goto fail;
            }
            json_object *budget = parse_json_document(response->body, response->body_length);
            codexbar_http_response_free(response);
            gboolean has_usage = FALSE;
            double usage = 0.0;
            gboolean response_has_limit = FALSE;
            double response_limit = 0.0;
            gboolean valid_budget = budget && json_object_is_type(budget, json_type_object) &&
                                    optional_number(budget, "usage", &has_usage, &usage) && has_usage &&
                                    optional_number(budget, "budget_limit", &response_has_limit, &response_limit);
            if (!valid_budget) {
                if (budget) json_object_put(budget);
                g_free(team_name);
                g_free(team_id);
                g_free(base_url);
                g_free(plan_name);
                g_free(user_id);
                g_free(legacy_name);
                g_free(instance_name);
                g_free(instance_id);
                g_set_error_literal(error,
                                    G_IO_ERROR,
                                    G_IO_ERROR_INVALID_DATA,
                                    "IBM Bob team usage response is malformed.");
                goto fail;
            }
            json_object_put(budget);
            usage = MAX(0.0, usage);
            gboolean has_limit = response_has_limit ? response_limit >= 0.0
                                                     : profile_has_limit && profile_limit >= 0.0;
            double limit = response_has_limit ? response_limit : profile_limit;
            total_used += usage;
            if (has_limit) total_limit += limit;
            else all_limits_known = FALSE;
            if (!isfinite(total_used) || !isfinite(total_limit)) {
                g_free(team_name);
                g_free(team_id);
                g_free(base_url);
                g_free(plan_name);
                g_free(user_id);
                g_free(legacy_name);
                g_free(instance_name);
                g_free(instance_id);
                g_set_error_literal(error,
                                    G_IO_ERROR,
                                    G_IO_ERROR_INVALID_DATA,
                                    "IBM Bob team usage response is malformed.");
                goto fail;
            }
            if (instance_has_reset && (!has_reset || instance_reset_ms < earliest_reset_ms)) {
                has_reset = TRUE;
                earliest_reset_ms = instance_reset_ms;
            }
            add_unique_plan(plans, plan_name);
            json_object *detail = json_object_new_object();
            json_object_object_add(detail, "instanceID", json_object_new_string(instance_id));
            json_object_object_add(detail, "instanceName", json_object_new_string(display_instance));
            json_object_object_add(detail, "teamID", json_object_new_string(team_id));
            json_object_object_add(detail, "teamName", json_object_new_string(team_name ? team_name : team_id));
            if (plan_name) json_object_object_add(detail, "planName", json_object_new_string(plan_name));
            json_object_object_add(detail, "usedBobcoins", json_object_new_double(usage));
            if (has_limit) json_object_object_add(detail, "limitBobcoins", json_object_new_double(limit));
            if (instance_has_reset) {
                json_object_object_add(detail, "resetsAt", json_object_new_int64(instance_reset_ms));
            }
            json_object_array_add(team_details, detail);
            usable_teams++;
            g_free(team_name);
            g_free(team_id);
        }
        g_free(base_url);
        g_free(plan_name);
        g_free(user_id);
        g_free(legacy_name);
        g_free(instance_name);
        g_free(instance_id);
    }

    if (usable_teams == 0) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "IBM Bob returned no subscription instances or teams for this API key.");
        goto fail;
    }

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("ibmbob");
    provider->source = g_strdup("api");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "Monthly Bobcoins");
    window->usage_known = all_limits_known && total_limit > 0.0;
    if (window->usage_known) {
        window->used_percent = CLAMP(total_used / total_limit * 100.0, 0.0, 100.0);
    }
    window->has_window_minutes = TRUE;
    window->window_minutes = calendar_month_minutes(now_ms);
    window->has_resets_at = has_reset;
    window->resets_at_ms = earliest_reset_ms;
    char *used_text = format_bobcoins(total_used);
    char *limit_text = all_limits_known ? format_bobcoins(total_limit) : NULL;
    window->reset_description = limit_text
                                    ? g_strdup_printf("%s / %s Bobcoins", used_text, limit_text)
                                    : g_strdup_printf("%s Bobcoins used", used_text);
    g_free(limit_text);
    g_free(used_text);
    codexbar_provider_add_quota_window(provider, window);
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->organization = joined_plans(plans);
    provider->identity->login_method = g_strdup("API key");
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));
    json_object *details = json_object_new_object();
    json_object_object_add(details, "usedBobcoins", json_object_new_double(total_used));
    if (all_limits_known) {
        json_object_object_add(details, "limitBobcoins", json_object_new_double(total_limit));
    }
    if (has_reset) json_object_object_add(details, "resetsAt", json_object_new_int64(earliest_reset_ms));
    json_object_object_add(details, "teams", team_details);
    json_object_object_add(provider->usage_extensions, "ibmBobUsage", details);
    g_ptr_array_unref(plans);
    json_object_put(profile);
    g_free(authorization);
    return provider;

fail:
    g_ptr_array_unref(plans);
    json_object_put(team_details);
    json_object_put(profile);
    g_free(authorization);
    return NULL;
}

CodexBarProvider *codexbar_ibmbob_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error) {
    return codexbar_ibmbob_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_ibmbob_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_ibmbob_fetch_with_cancellable(config, NULL, error);
}
