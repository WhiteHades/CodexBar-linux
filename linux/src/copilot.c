#include "copilot.h"

#include "http.h"

#include <errno.h>
#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

#define COPILOT_API_VERSION "2026-03-10"
#define COPILOT_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)
#define COPILOT_TIMEOUT_SECONDS 20L

typedef struct {
    gboolean present;
    gboolean unlimited;
    gboolean has_entitlement;
    double entitlement;
    gboolean has_remaining;
    double remaining;
    gboolean has_percent;
    double percent_remaining;
} CopilotQuota;

static gboolean parse_json_number(json_object *value, double *result) {
    if (!value) {
        return FALSE;
    }

    enum json_type type = json_object_get_type(value);
    if (type == json_type_int || type == json_type_double) {
        double number = json_object_get_double(value);
        if (!isfinite(number)) {
            return FALSE;
        }
        *result = number;
        return TRUE;
    }
    if (type != json_type_string) {
        return FALSE;
    }

    const char *text = json_object_get_string(value);
    if (!text || text[0] == '\0') {
        return FALSE;
    }
    errno = 0;
    char *end = NULL;
    double number = g_ascii_strtod(text, &end);
    if (errno == ERANGE || !end || end == text || end[0] != '\0' || !isfinite(number)) {
        return FALSE;
    }
    *result = number;
    return TRUE;
}

static json_object *object_member(json_object *object, const char *name) {
    json_object *value = NULL;
    if (!object || json_object_get_type(object) != json_type_object ||
        !json_object_object_get_ex(object, name, &value)) {
        return NULL;
    }
    return value;
}

static gboolean quota_is_placeholder(const CopilotQuota *quota) {
    return quota->present && !quota->unlimited && quota->has_entitlement && quota->entitlement == 0.0 &&
           quota->has_remaining && quota->remaining == 0.0;
}

static gboolean quota_is_finite_window(const CopilotQuota *quota) {
    return quota->present && !quota->unlimited && !quota_is_placeholder(quota) && quota->has_percent &&
           isfinite(quota->percent_remaining);
}

static CopilotQuota parse_quota(json_object *value) {
    CopilotQuota quota = {0};
    if (!value || json_object_get_type(value) != json_type_object) {
        return quota;
    }

    quota.present = TRUE;
    quota.has_entitlement = parse_json_number(object_member(value, "entitlement"), &quota.entitlement);
    quota.has_remaining = parse_json_number(object_member(value, "remaining"), &quota.remaining);
    quota.has_percent = parse_json_number(object_member(value, "percent_remaining"), &quota.percent_remaining);

    json_object *unlimited = object_member(value, "unlimited");
    quota.unlimited = unlimited && json_object_get_type(unlimited) == json_type_boolean &&
                      json_object_get_boolean(unlimited);
    if (quota.unlimited) {
        quota.has_percent = TRUE;
        quota.percent_remaining = 100.0;
    } else if (!quota.has_percent && quota.has_entitlement && quota.entitlement > 0.0 && quota.has_remaining) {
        quota.has_percent = TRUE;
        quota.percent_remaining = quota.remaining / quota.entitlement * 100.0;
    }
    return quota;
}

static CopilotQuota quota_member(json_object *object, const char *name) {
    return parse_quota(object_member(object, name));
}

static gboolean quota_name_known(const char *name) {
    return g_str_equal(name, "premium_interactions") || g_str_equal(name, "completions") ||
           g_str_equal(name, "chat") || g_str_equal(name, "chat_messages");
}

static CopilotQuota first_usable_quota(json_object *object, const char *const *names, size_t count) {
    CopilotQuota unlimited = {0};
    for (size_t index = 0; index < count; index++) {
        CopilotQuota quota = quota_member(object, names[index]);
        if (quota_is_finite_window(&quota)) {
            return quota;
        }
        if (!unlimited.present && quota.present && quota.unlimited) {
            unlimited = quota;
        }
    }
    return unlimited;
}

static CopilotQuota monthly_quota(json_object *monthly, json_object *limited, const char *name) {
    CopilotQuota quota = {0};
    double entitlement = 0.0;
    double remaining = 0.0;
    if (!parse_json_number(object_member(monthly, name), &entitlement) || entitlement <= 0.0 ||
        !parse_json_number(object_member(limited, name), &remaining)) {
        return quota;
    }
    quota.present = TRUE;
    quota.has_entitlement = TRUE;
    quota.entitlement = entitlement;
    quota.has_remaining = TRUE;
    quota.remaining = remaining;
    quota.has_percent = TRUE;
    quota.percent_remaining = remaining / entitlement * 100.0;
    return quota;
}

static CopilotQuota select_quota(CopilotQuota direct, CopilotQuota monthly) {
    if (quota_is_finite_window(&direct)) {
        return direct;
    }
    if (quota_is_finite_window(&monthly)) {
        return monthly;
    }
    if (direct.present && direct.unlimited) {
        return direct;
    }
    return (CopilotQuota){0};
}

static CopilotQuota unknown_quota(json_object *snapshots) {
    if (!snapshots || json_object_get_type(snapshots) != json_type_object) {
        return (CopilotQuota){0};
    }
    json_object_object_foreach(snapshots, name, value) {
        if (!quota_name_known(name)) {
            CopilotQuota quota = parse_quota(value);
            if (quota_is_finite_window(&quota) || quota.unlimited) {
                return quota;
            }
        }
    }
    return (CopilotQuota){0};
}

static json_object *parse_json_object(const char *text, GError **error) {
    if (!text) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Copilot returned no data");
        return NULL;
    }
    size_t length = strlen(text);
    if (length > COPILOT_MAXIMUM_RESPONSE_BYTES || length > G_MAXINT) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Copilot response is too large");
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
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Copilot returned invalid JSON");
        return NULL;
    }
    return root;
}

static gboolean parse_reset_milliseconds(const char *raw, gint64 *milliseconds) {
    if (!raw) {
        return FALSE;
    }
    char *trimmed = g_strdup(raw);
    g_strstrip(trimmed);
    if (trimmed[0] == '\0') {
        g_free(trimmed);
        return FALSE;
    }

    char *iso = strchr(trimmed, 'T') ? g_strdup(trimmed) : g_strdup_printf("%sT00:00:00Z", trimmed);
    GTimeZone *utc = g_time_zone_new_utc();
    GDateTime *date = g_date_time_new_from_iso8601(iso, utc);
    g_time_zone_unref(utc);
    g_free(iso);
    g_free(trimmed);
    if (!date) {
        return FALSE;
    }
    *milliseconds = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static char *plan_label(json_object *root) {
    json_object *value = object_member(root, "copilot_plan");
    const char *raw = value && json_object_get_type(value) == json_type_string ? json_object_get_string(value) : NULL;
    if (!raw || raw[0] == '\0') {
        return g_strdup("Unknown");
    }
    char *label = g_ascii_strdown(raw, -1);
    label[0] = (char)g_ascii_toupper((guchar)label[0]);
    return label;
}

static void add_quota_window(CodexBarProvider *provider,
                             const char *id,
                             const char *title,
                             const CopilotQuota *quota,
                             gboolean has_reset,
                             gint64 reset_ms) {
    if (!quota_is_finite_window(quota)) {
        return;
    }
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = 100.0 - quota->percent_remaining;
    if (has_reset) {
        window->has_resets_at = TRUE;
        window->resets_at_ms = reset_ms;
    }
    if (window->used_percent > 100.0) {
        window->reset_description = g_strdup_printf("%.0f%% used", window->used_percent);
    }
    codexbar_provider_add_quota_window(provider, window);
}

CodexBarProvider *codexbar_copilot_parse_usage(const char *json, GError **error) {
    json_object *root = parse_json_object(json, error);
    if (!root) {
        return NULL;
    }

    json_object *snapshots = object_member(root, "quota_snapshots");
    json_object *monthly = object_member(root, "monthly_quotas");
    json_object *limited = object_member(root, "limited_user_quotas");
    static const char *const premium_names[] = {"premium_interactions", "completions"};
    static const char *const chat_names[] = {"chat", "chat_messages"};

    CopilotQuota premium_direct = first_usable_quota(snapshots, premium_names, G_N_ELEMENTS(premium_names));
    CopilotQuota chat_direct = first_usable_quota(snapshots, chat_names, G_N_ELEMENTS(chat_names));
    if (!chat_direct.present) {
        chat_direct = unknown_quota(snapshots);
    }
    CopilotQuota premium_monthly = monthly_quota(monthly, limited, "completions");
    CopilotQuota chat_monthly = monthly_quota(monthly, limited, "chat");
    CopilotQuota premium = select_quota(premium_direct, premium_monthly);
    CopilotQuota chat = select_quota(chat_direct, chat_monthly);

    json_object *token_billing_value = object_member(root, "token_based_billing");
    gboolean token_billing = token_billing_value && json_object_get_type(token_billing_value) == json_type_boolean &&
                             json_object_get_boolean(token_billing_value);
    gboolean unlimited = premium.unlimited || chat.unlimited;
    if (!quota_is_finite_window(&premium) && !quota_is_finite_window(&chat) && !token_billing && !unlimited) {
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Copilot response has no usable quota data");
        return NULL;
    }

    gint64 reset_ms = 0;
    json_object *reset_value = object_member(root, "quota_reset_date");
    gboolean has_reset = reset_value && json_object_get_type(reset_value) == json_type_string &&
                         parse_reset_milliseconds(json_object_get_string(reset_value), &reset_ms);

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("copilot");
    provider->source = g_strdup("api");
    provider->plan = plan_label(root);
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = g_strdup(provider->plan);
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = g_get_real_time() / 1000;
    provider->explicit_quota_slots = TRUE;
    add_quota_window(provider, "premium", "Premium", &premium, has_reset, reset_ms);
    add_quota_window(provider, "chat", "Chat", &chat, has_reset, reset_ms);

    json_object_put(root);
    return provider;
}

static gboolean valid_dns_name(const char *host) {
    if (!host || host[0] == '\0' || g_hostname_is_ip_address(host)) {
        return FALSE;
    }
    size_t length = strlen(host);
    if (length > 253 || host[0] == '.' || host[length - 1] == '.') {
        return FALSE;
    }
    gboolean label_start = TRUE;
    unsigned char previous = 0;
    for (const unsigned char *cursor = (const unsigned char *)host; *cursor; cursor++) {
        if (*cursor == '.') {
            if (label_start || previous == '-') {
                return FALSE;
            }
            label_start = TRUE;
        } else if (g_ascii_isalnum(*cursor)) {
            label_start = FALSE;
        } else if (*cursor == '-') {
            if (label_start) {
                return FALSE;
            }
        } else {
            return FALSE;
        }
        previous = *cursor;
    }
    return previous != '-';
}

char *codexbar_copilot_usage_url(const char *enterprise_host, GError **error) {
    char *raw = g_strdup(enterprise_host ? enterprise_host : "");
    g_strstrip(raw);
    if (raw[0] == '\0') {
        g_free(raw);
        return g_strdup("https://api.github.com/copilot_internal/user");
    }

    char *uri_text = strstr(raw, "://") ? g_strdup(raw) : g_strdup_printf("https://%s", raw);
    GError *uri_error = NULL;
    GUri *uri = g_uri_parse(uri_text, G_URI_FLAGS_PARSE_RELAXED, &uri_error);
    g_free(uri_text);
    g_free(raw);
    if (!uri) {
        g_propagate_prefixed_error(error, uri_error, "Invalid Copilot enterprise host: ");
        return NULL;
    }

    const char *host = g_uri_get_host(uri);
    int port = g_uri_get_port(uri);
    char *ascii_host = host ? g_hostname_to_ascii(host) : NULL;
    if (g_uri_get_userinfo(uri) || !valid_dns_name(ascii_host)) {
        g_uri_unref(uri);
        g_free(ascii_host);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Invalid Copilot enterprise host");
        return NULL;
    }

    char *lower = g_ascii_strdown(ascii_host, -1);
    g_free(ascii_host);
    const char *prefix = g_str_has_prefix(lower, "api.") ? "" : "api.";
    char *url = port > 0
                    ? g_strdup_printf("https://%s%s:%d/copilot_internal/user", prefix, lower, port)
                    : g_strdup_printf("https://%s%s/copilot_internal/user", prefix, lower);
    g_free(lower);
    g_uri_unref(uri);
    return url;
}

CodexBarProvider *codexbar_copilot_fetch(const CodexBarProviderConfig *config, GError **error) {
    const char *environment_token = g_getenv("COPILOT_API_TOKEN");
    const char *configured_token = config ? config->api_key : NULL;
    const char *selected_token = environment_token && environment_token[0] != '\0' ? environment_token : configured_token;
    char *token = g_strdup(selected_token ? selected_token : "");
    g_strstrip(token);
    if (token[0] == '\0') {
        g_free(token);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_NOT_FOUND,
                            "Copilot API token not found. Set COPILOT_API_TOKEN or configure api_key.");
        return NULL;
    }

    char *url = codexbar_copilot_usage_url(config ? config->enterprise_host : NULL, error);
    if (!url) {
        g_free(token);
        return NULL;
    }
    char *authorization = g_strdup_printf("Bearer %s", token);
    g_free(token);
    const CodexBarHttpRequestHeader headers[] = {
        {"Accept", "application/vnd.github+json"},
        {"Authorization", authorization},
        {"Editor-Version", "vscode/1.96.2"},
        {"Editor-Plugin-Version", "copilot-chat/0.26.7"},
        {"User-Agent", "GitHubCopilotChat/0.26.7"},
        {"X-GitHub-Api-Version", COPILOT_API_VERSION},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = COPILOT_TIMEOUT_SECONDS,
        .maximum_response_bytes = COPILOT_MAXIMUM_RESPONSE_BYTES,
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
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Copilot rejected the API token");
        return NULL;
    }
    if (response->status != 200) {
        long status = response->status;
        codexbar_http_response_free(response);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Copilot request failed with HTTP %ld", status);
        return NULL;
    }

    CodexBarProvider *provider = codexbar_copilot_parse_usage(response->body, error);
    codexbar_http_response_free(response);
    return provider;
}
