#include "zai.h"

#include "http.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define ZAI_GLOBAL_URL "https://api.z.ai/api/monitor/usage/quota/limit"
#define ZAI_BIGMODEL_URL "https://open.bigmodel.cn/api/monitor/usage/quota/limit"
#define ZAI_QUOTA_PATH "/api/monitor/usage/quota/limit"
#define ZAI_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)
#define ZAI_TIMEOUT_SECONDS 20L

typedef enum {
    ZAI_LIMIT_UNKNOWN,
    ZAI_LIMIT_TOKENS,
    ZAI_LIMIT_TIME,
} ZaiLimitType;

typedef struct {
    ZaiLimitType type;
    int unit;
    gint64 number;
    gboolean has_usage;
    double usage;
    gboolean has_current;
    double current;
    gboolean has_remaining;
    double remaining;
    gboolean has_percentage;
    double percentage;
    gboolean has_reset;
    gint64 reset_ms;
} ZaiLimit;

static json_object *object_member(json_object *object, const char *name) {
    json_object *value = NULL;
    if (!object || json_object_get_type(object) != json_type_object ||
        !json_object_object_get_ex(object, name, &value)) {
        return NULL;
    }
    return value;
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

static gboolean json_integer(json_object *value, gint64 *result) {
    if (!value || json_object_get_type(value) != json_type_int) {
        return FALSE;
    }
    *result = json_object_get_int64(value);
    return TRUE;
}

static json_object *parse_json_object(const char *text, GError **error) {
    if (!text || text[0] == '\0') {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Z.ai returned an empty response");
        return NULL;
    }
    size_t length = strlen(text);
    if (length > ZAI_MAXIMUM_RESPONSE_BYTES || length > G_MAXINT) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Z.ai response is too large");
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
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Z.ai returned invalid JSON");
        return NULL;
    }
    return root;
}

static ZaiLimit parse_limit(json_object *value) {
    ZaiLimit limit = {0};
    if (!value || json_object_get_type(value) != json_type_object) {
        return limit;
    }
    json_object *type_value = object_member(value, "type");
    const char *type = type_value && json_object_get_type(type_value) == json_type_string
                           ? json_object_get_string(type_value)
                           : NULL;
    if (g_strcmp0(type, "TOKENS_LIMIT") == 0) {
        limit.type = ZAI_LIMIT_TOKENS;
    } else if (g_strcmp0(type, "TIME_LIMIT") == 0) {
        limit.type = ZAI_LIMIT_TIME;
    } else {
        return limit;
    }

    gint64 unit = 0;
    if (json_integer(object_member(value, "unit"), &unit) && unit >= G_MININT && unit <= G_MAXINT) {
        limit.unit = (int)unit;
    }
    json_integer(object_member(value, "number"), &limit.number);
    limit.has_usage = json_number(object_member(value, "usage"), &limit.usage);
    limit.has_current = json_number(object_member(value, "currentValue"), &limit.current);
    limit.has_remaining = json_number(object_member(value, "remaining"), &limit.remaining);
    limit.has_percentage = json_number(object_member(value, "percentage"), &limit.percentage);
    limit.has_reset = json_integer(object_member(value, "nextResetTime"), &limit.reset_ms);
    return limit;
}

static gboolean limit_window_minutes(const ZaiLimit *limit, gint64 *minutes) {
    if (limit->number <= 0) {
        return FALSE;
    }
    gint64 multiplier = 0;
    switch (limit->unit) {
    case 1:
        multiplier = 24 * 60;
        break;
    case 3:
        multiplier = 60;
        break;
    case 5:
        multiplier = 1;
        break;
    case 6:
        multiplier = 7 * 24 * 60;
        break;
    default:
        return FALSE;
    }
    if (limit->number > G_MAXINT64 / multiplier) {
        return FALSE;
    }
    *minutes = limit->number * multiplier;
    return TRUE;
}

static double limit_used_percent(const ZaiLimit *limit) {
    if (limit->has_usage && limit->usage > 0.0) {
        gboolean known = FALSE;
        double used = 0.0;
        if (limit->has_remaining) {
            used = limit->usage - limit->remaining;
            known = TRUE;
        }
        if (limit->has_current && (!known || limit->current > used)) {
            used = limit->current;
            known = TRUE;
        }
        if (known) {
            used = CLAMP(used, 0.0, limit->usage);
            return codexbar_usage_percent_display(codexbar_usage_percent_from_ratio(used, limit->usage));
        }
    }
    return limit->has_percentage ? limit->percentage : 0.0;
}

static char *limit_description(const ZaiLimit *limit) {
    if (limit->type == ZAI_LIMIT_TIME && limit->unit == 5 && limit->number == 1) {
        return g_strdup("Monthly");
    }
    const char *unit = NULL;
    switch (limit->unit) {
    case 1:
        unit = "day";
        break;
    case 3:
        unit = "hour";
        break;
    case 5:
        unit = "minute";
        break;
    case 6:
        unit = "week";
        break;
    default:
        return limit->type == ZAI_LIMIT_TIME ? g_strdup("Monthly") : NULL;
    }
    return g_strdup_printf("%" G_GINT64_FORMAT " %s%s window", limit->number, unit, limit->number == 1 ? "" : "s");
}

static CodexBarQuotaWindow *make_window(const char *id, const char *title, const ZaiLimit *limit) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = limit->has_percentage || (limit->has_usage && (limit->has_current || limit->has_remaining));
    window->used_percent = limit_used_percent(limit);
    gint64 minutes = 0;
    if (limit->type == ZAI_LIMIT_TOKENS && limit_window_minutes(limit, &minutes)) {
        window->has_window_minutes = TRUE;
        window->window_minutes = minutes;
    }
    if (limit->has_reset && limit->reset_ms > 0) {
        window->has_resets_at = TRUE;
        window->resets_at_ms = limit->reset_ms;
    }
    window->reset_description = limit_description(limit);
    return window;
}

static char *string_member_copy(json_object *object, const char *name) {
    json_object *value = object_member(object, name);
    if (!value || json_object_get_type(value) != json_type_string) {
        return NULL;
    }
    char *text = g_strdup(json_object_get_string(value));
    g_strstrip(text);
    if (text[0] == '\0') {
        g_free(text);
        return NULL;
    }
    return text;
}

static char *plan_name(json_object *data) {
    static const char *const names[] = {"planName", "plan", "plan_type", "packageName", "level"};
    for (size_t index = 0; index < G_N_ELEMENTS(names); index++) {
        char *plan = string_member_copy(data, names[index]);
        if (plan) {
            if (g_ascii_islower((guchar)plan[0])) {
                plan[0] = (char)g_ascii_toupper((guchar)plan[0]);
            }
            return plan;
        }
    }
    return NULL;
}

CodexBarProvider *codexbar_zai_parse_usage(const char *json, GError **error) {
    json_object *root = parse_json_object(json, error);
    if (!root) {
        return NULL;
    }

    gint64 code = 0;
    gboolean has_code = json_integer(object_member(root, "code"), &code);
    json_object *success_value = object_member(root, "success");
    gboolean success = success_value && json_object_get_type(success_value) == json_type_boolean &&
                       json_object_get_boolean(success_value);
    if (!success || !has_code || code != 200) {
        json_object_put(root);
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Z.ai quota API returned code %" G_GINT64_FORMAT,
                    has_code ? code : 0);
        return NULL;
    }

    json_object *data = object_member(root, "data");
    if (!data || json_object_get_type(data) != json_type_object) {
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Z.ai response is missing data");
        return NULL;
    }

    ZaiLimit token_limits[64] = {0};
    size_t token_count = 0;
    ZaiLimit time_limit = {0};
    json_object *limits = object_member(data, "limits");
    if (limits && json_object_get_type(limits) == json_type_array) {
        size_t count = json_object_array_length(limits);
        for (size_t index = 0; index < count; index++) {
            ZaiLimit limit = parse_limit(json_object_array_get_idx(limits, index));
            if (limit.type == ZAI_LIMIT_TOKENS && token_count < G_N_ELEMENTS(token_limits)) {
                token_limits[token_count++] = limit;
            } else if (limit.type == ZAI_LIMIT_TIME) {
                time_limit = limit;
            }
        }
    }

    ZaiLimit primary_token = {0};
    ZaiLimit session_token = {0};
    if (token_count == 1) {
        primary_token = token_limits[0];
    } else if (token_count >= 2) {
        size_t shortest = 0;
        size_t longest = 0;
        gint64 shortest_minutes = G_MAXINT64;
        gint64 longest_minutes = G_MININT64;
        for (size_t index = 0; index < token_count; index++) {
            gint64 minutes = 0;
            gint64 sortable = limit_window_minutes(&token_limits[index], &minutes) ? minutes : G_MAXINT64;
            if (sortable < shortest_minutes) {
                shortest_minutes = sortable;
                shortest = index;
            }
            if (sortable >= longest_minutes) {
                longest_minutes = sortable;
                longest = index;
            }
        }
        session_token = token_limits[shortest];
        primary_token = token_limits[longest];
    }

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("zai");
    provider->source = g_strdup("api");
    provider->plan = plan_name(data);
    if (provider->plan) {
        provider->identity = g_new0(CodexBarProviderIdentity, 1);
        provider->identity->login_method = g_strdup(provider->plan);
    }
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = g_get_real_time() / 1000;
    provider->explicit_quota_slots = TRUE;

    if (primary_token.type != ZAI_LIMIT_UNKNOWN) {
        codexbar_provider_add_quota_window(provider, make_window("tokens", "Tokens", &primary_token));
    } else if (time_limit.type != ZAI_LIMIT_UNKNOWN) {
        codexbar_provider_add_quota_window(provider, make_window("mcp", "MCP", &time_limit));
    }
    if (primary_token.type != ZAI_LIMIT_UNKNOWN && time_limit.type != ZAI_LIMIT_UNKNOWN) {
        codexbar_provider_add_quota_window(provider, make_window("mcp", "MCP", &time_limit));
    }
    if (session_token.type != ZAI_LIMIT_UNKNOWN) {
        codexbar_provider_add_quota_window(provider, make_window("session", "Session", &session_token));
    }

    json_object_put(root);
    return provider;
}

static char *api_host_url(const char *raw, GError **error) {
    char *trimmed = g_strdup(raw);
    g_strstrip(trimmed);
    char *candidate = strstr(trimmed, "://") ? g_strdup(trimmed) : g_strdup_printf("https://%s", trimmed);
    g_free(trimmed);
    char *normalized = codexbar_http_normalize_endpoint(candidate, CODEXBAR_HTTP_HTTPS_ONLY, error);
    g_free(candidate);
    if (!normalized) {
        return NULL;
    }

    GError *uri_error = NULL;
    GUri *uri = g_uri_parse(normalized, G_URI_FLAGS_NONE, &uri_error);
    if (!uri || g_uri_get_userinfo(uri) || !g_uri_get_host(uri) || g_uri_get_fragment(uri)) {
        if (uri) {
            g_uri_unref(uri);
        }
        g_free(normalized);
        if (uri_error) {
            g_propagate_prefixed_error(error, uri_error, "Invalid Z.ai API host: ");
        } else {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid Z.ai API host");
        }
        return NULL;
    }
    const char *path = g_uri_get_path(uri);
    const char *resolved_path = !path || path[0] == '\0' || g_str_equal(path, "/") ? ZAI_QUOTA_PATH : path;
    char *url = g_uri_join(G_URI_FLAGS_NONE,
                           "https",
                           NULL,
                           g_uri_get_host(uri),
                           g_uri_get_port(uri),
                           resolved_path,
                           g_uri_get_query(uri),
                           NULL);
    g_uri_unref(uri);
    g_free(normalized);
    return url;
}

char *codexbar_zai_quota_url(const CodexBarProviderConfig *config, GError **error) {
    const char *quota_override = g_getenv("Z_AI_QUOTA_URL");
    if (quota_override && quota_override[0] != '\0') {
        return codexbar_http_normalize_endpoint(quota_override, CODEXBAR_HTTP_HTTPS_ONLY, error);
    }
    const char *host_override = g_getenv("Z_AI_API_HOST");
    if (host_override && host_override[0] != '\0') {
        return api_host_url(host_override, error);
    }
    return g_strdup(config && g_strcmp0(config->region, "bigmodel-cn") == 0 ? ZAI_BIGMODEL_URL : ZAI_GLOBAL_URL);
}

CodexBarProvider *codexbar_zai_fetch(const CodexBarProviderConfig *config, GError **error) {
    const char *environment_token = g_getenv("Z_AI_API_KEY");
    const char *configured_token = config ? config->api_key : NULL;
    const char *selected_token = environment_token && environment_token[0] != '\0' ? environment_token : configured_token;
    char *token = g_strdup(selected_token ? selected_token : "");
    g_strstrip(token);
    if (token[0] == '\0') {
        g_free(token);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "Z.ai API token not found. Set Z_AI_API_KEY or configure api_key.");
        return NULL;
    }

    char *url = codexbar_zai_quota_url(config, error);
    if (!url) {
        g_free(token);
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", token);
    g_free(token);
    const CodexBarHttpRequestHeader headers[] = {
        {"Accept", "application/json"},
        {"Authorization", authorization},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = ZAI_TIMEOUT_SECONDS,
        .maximum_response_bytes = ZAI_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
    };
    CodexBarHttpResponse *response = codexbar_http_send(&request, error);
    g_free(authorization);
    g_free(url);
    if (!response) {
        return NULL;
    }
    if (response->status == 401 || response->status == 403) {
        codexbar_http_response_free(response);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Z.ai rejected the API token");
        return NULL;
    }
    if (response->status != 200) {
        long status = response->status;
        codexbar_http_response_free(response);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Z.ai request failed with HTTP %ld", status);
        return NULL;
    }

    CodexBarProvider *provider = codexbar_zai_parse_usage(response->body, error);
    codexbar_http_response_free(response);
    return provider;
}
