#include "claude.h"

#include "http.h"
#include "process.h"

#include <errno.h>
#include <glib.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

enum {
    CLAUDE_RESPONSE_LIMIT = 1024 * 1024,
};

typedef struct {
    char *access_token;
    char *rate_limit_tier;
    char *subscription_type;
} ClaudeCredentials;

static GQuark claude_error_quark(void) {
    return g_quark_from_static_string("codexbar-claude-error");
}

static void credentials_clear(ClaudeCredentials *credentials) {
    g_free(credentials->access_token);
    g_free(credentials->rate_limit_tier);
    g_free(credentials->subscription_type);
    *credentials = (ClaudeCredentials){0};
}

static char *trimmed_json_string(json_object *object, const char *key) {
    json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    char *text = g_strstrip(g_strdup(json_object_get_string(value)));
    if (text[0] != '\0') return text;
    g_free(text);
    return NULL;
}

static json_object *parse_json_object(const char *text, GError **error) {
    if (!text) {
        g_set_error_literal(error, claude_error_quark(), 1, "Claude OAuth response was empty");
        return NULL;
    }
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT);
    json_object *root = json_tokener_parse_ex(tokener, text, (int)strlen(text));
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    json_tokener_free(tokener);
    if (parse_error == json_tokener_success && root && json_object_is_type(root, json_type_object)) return root;
    if (root) json_object_put(root);
    g_set_error_literal(error, claude_error_quark(), 1, "Claude OAuth response was invalid");
    return NULL;
}

static char *credentials_path(void) {
    const char *configured = g_getenv("CLAUDE_CONFIG_DIR");
    if (configured && configured[0] != '\0') return g_build_filename(configured, ".credentials.json", NULL);
    return g_build_filename(g_get_home_dir(), ".claude", ".credentials.json", NULL);
}

static gboolean load_credentials(ClaudeCredentials *credentials, GError **error) {
    const char *environment_token = g_getenv("CODEXBAR_CLAUDE_OAUTH_TOKEN");
    if (environment_token) {
        credentials->access_token = g_strstrip(g_strdup(environment_token));
        if (credentials->access_token[0] != '\0') return TRUE;
        g_clear_pointer(&credentials->access_token, g_free);
    }

    char *path = credentials_path();
    char *contents = NULL;
    gsize length = 0;
    GError *read_error = NULL;
    if (!g_file_get_contents(path, &contents, &length, &read_error)) {
        g_set_error(error,
                    claude_error_quark(),
                    2,
                    "Claude OAuth credentials were not found. Run `claude login`.");
        g_clear_error(&read_error);
        g_free(path);
        return FALSE;
    }
    g_free(path);
    if (length == 0 || length > CLAUDE_RESPONSE_LIMIT) {
        g_free(contents);
        g_set_error_literal(error, claude_error_quark(), 3, "Claude OAuth credentials are invalid");
        return FALSE;
    }

    json_object *root = parse_json_object(contents, error);
    g_free(contents);
    if (!root) return FALSE;
    json_object *oauth = NULL;
    if (!json_object_object_get_ex(root, "claudeAiOauth", &oauth) ||
        !json_object_is_type(oauth, json_type_object)) {
        json_object_put(root);
        g_set_error_literal(error,
                            claude_error_quark(),
                            3,
                            "Claude OAuth credentials are missing. Run `claude login`.");
        return FALSE;
    }
    credentials->access_token = trimmed_json_string(oauth, "accessToken");
    credentials->rate_limit_tier = trimmed_json_string(oauth, "rateLimitTier");
    credentials->subscription_type = trimmed_json_string(oauth, "subscriptionType");
    json_object *expires = NULL;
    gboolean has_expiry = json_object_object_get_ex(oauth, "expiresAt", &expires) &&
                          (json_object_is_type(expires, json_type_int) ||
                           json_object_is_type(expires, json_type_double));
    double expires_at = has_expiry ? json_object_get_double(expires) : 0;
    json_object_put(root);
    if (!credentials->access_token) {
        credentials_clear(credentials);
        g_set_error_literal(error,
                            claude_error_quark(),
                            3,
                            "Claude OAuth access token is missing. Run `claude login`.");
        return FALSE;
    }
    if (!has_expiry || !isfinite(expires_at) || expires_at <= (double)(g_get_real_time() / 1000)) {
        credentials_clear(credentials);
        g_set_error_literal(error, claude_error_quark(), 4, "Claude OAuth token expired. Run `claude login`.");
        return FALSE;
    }
    return TRUE;
}

static char *claude_user_agent(void) {
    const char *arguments[] = {"claude", "--version", NULL};
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = 2000,
        .termination_grace_milliseconds = 100,
        .maximum_output_bytes = 4096,
    };
    GError *error = NULL;
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, &error);
    char *version = NULL;
    if (codexbar_process_result_succeeded(result)) {
        char *output = g_strstrip(g_strdup(result->standard_output));
        char **parts = g_strsplit_set(output, " \t\r\n", 2);
        if (parts[0] && parts[0][0] != '\0' && strlen(parts[0]) <= 32) {
            gboolean safe = TRUE;
            for (const unsigned char *cursor = (const unsigned char *)parts[0]; *cursor; cursor++) {
                if (!g_ascii_isalnum(*cursor) && *cursor != '.' && *cursor != '-') safe = FALSE;
            }
            if (safe) version = g_strdup(parts[0]);
        }
        g_strfreev(parts);
        g_free(output);
    }
    codexbar_process_result_free(result);
    g_clear_error(&error);
    if (!version) version = g_strdup("2.1.0");
    char *user_agent = g_strdup_printf("claude-code/%s", version);
    g_free(version);
    return user_agent;
}

static gboolean json_number(json_object *object, const char *key, double *number) {
    json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value) ||
        (!json_object_is_type(value, json_type_int) && !json_object_is_type(value, json_type_double))) {
        return FALSE;
    }
    double parsed = json_object_get_double(value);
    if (!isfinite(parsed)) return FALSE;
    *number = parsed;
    return TRUE;
}

static gboolean json_boolean(json_object *object, const char *key, gboolean *value) {
    json_object *raw = NULL;
    if (!object || !json_object_object_get_ex(object, key, &raw) ||
        !json_object_is_type(raw, json_type_boolean)) {
        return FALSE;
    }
    *value = json_object_get_boolean(raw);
    return TRUE;
}

static gboolean parse_time(json_object *object, const char *key, gint64 *milliseconds) {
    char *text = trimmed_json_string(object, key);
    if (!text) return FALSE;
    GDateTime *date = g_date_time_new_from_iso8601(text, NULL);
    g_free(text);
    if (!date) return FALSE;
    *milliseconds = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static CodexBarQuotaWindow *parse_window(json_object *root,
                                         const char *key,
                                         const char *id,
                                         const char *title,
                                         gint64 minutes) {
    json_object *value = NULL;
    double utilization = 0;
    if (!json_object_object_get_ex(root, key, &value) || !json_object_is_type(value, json_type_object) ||
        !json_number(value, "utilization", &utilization)) {
        return NULL;
    }
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = utilization;
    window->has_window_minutes = minutes > 0;
    window->window_minutes = minutes;
    window->has_resets_at = parse_time(value, "resets_at", &window->resets_at_ms);
    return window;
}

static gboolean key_exists(json_object *object, const char *key) {
    json_object *value = NULL;
    return json_object_object_get_ex(object, key, &value);
}

static void add_routines_window(CodexBarProvider *provider, json_object *root) {
    static const char *const keys[] = {
        "seven_day_routines", "seven_day_claude_routines", "claude_routines", "routines",
        "routine", "seven_day_cowork", "cowork",
    };
    gboolean known = FALSE;
    CodexBarQuotaWindow *window = NULL;
    for (size_t index = 0; index < G_N_ELEMENTS(keys); index++) {
        if (key_exists(root, keys[index])) known = TRUE;
        window = parse_window(root, keys[index], "claude-routines", "Daily Routines", 7 * 24 * 60);
        if (window) break;
    }
    if (!window && known) {
        window = codexbar_quota_window_new("claude-routines", "Daily Routines");
        window->usage_known = TRUE;
        window->used_percent = 0;
        window->has_window_minutes = TRUE;
        window->window_minutes = 7 * 24 * 60;
    }
    if (window) codexbar_provider_add_quota_window(provider, window);
}

static char *slug(const char *text) {
    if (!text) return NULL;
    GString *output = g_string_new(NULL);
    gboolean dash = FALSE;
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor; cursor++) {
        if (g_ascii_isalnum(*cursor)) {
            g_string_append_c(output, (char)g_ascii_tolower(*cursor));
            dash = FALSE;
        } else if (output->len > 0 && !dash) {
            g_string_append_c(output, '-');
            dash = TRUE;
        }
    }
    while (output->len > 0 && output->str[output->len - 1] == '-') g_string_truncate(output, output->len - 1);
    return g_string_free(output, FALSE);
}

static gboolean all_models_scope(const char *id, const char *name) {
    char *name_slug = slug(name);
    char *id_slug = slug(id);
    gboolean all = (name_slug && g_str_equal(name_slug, "all-models")) ||
                   (id_slug && (g_str_equal(id_slug, "all-models") || g_str_has_suffix(id_slug, "-all-models")));
    g_free(name_slug);
    g_free(id_slug);
    return all;
}

static void add_scoped_windows(CodexBarProvider *provider, json_object *root) {
    json_object *limits = NULL;
    if (!json_object_object_get_ex(root, "limits", &limits) || !json_object_is_type(limits, json_type_array)) return;
    GHashTable *seen = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    size_t count = json_object_array_length(limits);
    for (size_t index = 0; index < count; index++) {
        json_object *entry = json_object_array_get_idx(limits, index);
        if (!entry || !json_object_is_type(entry, json_type_object)) continue;
        char *kind = trimmed_json_string(entry, "kind");
        char *group = trimmed_json_string(entry, "group");
        double percent = 0;
        json_object *scope = NULL;
        json_object *model = NULL;
        gboolean valid = kind && group && g_str_equal(kind, "weekly_scoped") && g_str_equal(group, "weekly") &&
                         json_number(entry, "percent", &percent) &&
                         json_object_object_get_ex(entry, "scope", &scope) &&
                         json_object_is_type(scope, json_type_object) &&
                         json_object_object_get_ex(scope, "model", &model) &&
                         json_object_is_type(model, json_type_object);
        char *model_id = valid ? trimmed_json_string(model, "id") : NULL;
        char *model_name = valid ? trimmed_json_string(model, "display_name") : NULL;
        char *identity = model_id ? g_strdup(model_id) : model_name ? g_strdup(model_name) : NULL;
        char *model_slug = slug(identity);
        if (valid && model_name && model_slug && model_slug[0] != '\0' &&
            !all_models_scope(model_id, model_name) && !g_hash_table_contains(seen, model_slug)) {
            g_hash_table_add(seen, g_strdup(model_slug));
            char *id = g_strdup_printf("claude-weekly-scoped-%s", model_slug);
            char *title = g_strdup_printf("%s only", model_name);
            CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
            window->usage_known = TRUE;
            window->used_percent = percent;
            window->has_window_minutes = TRUE;
            window->window_minutes = 7 * 24 * 60;
            window->has_resets_at = parse_time(entry, "resets_at", &window->resets_at_ms);
            codexbar_provider_add_quota_window(provider, window);
            g_free(title);
            g_free(id);
        }
        g_free(model_slug);
        g_free(identity);
        g_free(model_name);
        g_free(model_id);
        g_free(group);
        g_free(kind);
    }
    g_hash_table_unref(seen);
}

static const char *currency_code(json_object *extra) {
    json_object *currency = NULL;
    if (!json_object_object_get_ex(extra, "currency", &currency) ||
        !json_object_is_type(currency, json_type_string)) {
        return "USD";
    }
    const char *text = json_object_get_string(currency);
    if (strlen(text) != 3 || !g_ascii_isalpha(text[0]) || !g_ascii_isalpha(text[1]) ||
        !g_ascii_isalpha(text[2])) {
        return "USD";
    }
    return text;
}

static void apply_extra_usage(CodexBarProvider *provider, json_object *root, gboolean has_primary, gint64 now_ms) {
    json_object *extra = NULL;
    gboolean enabled = FALSE;
    double limit = 0;
    double used = 0;
    if (!json_object_object_get_ex(root, "extra_usage", &extra) ||
        !json_object_is_type(extra, json_type_object) || !json_boolean(extra, "is_enabled", &enabled) || !enabled ||
        !json_number(extra, "monthly_limit", &limit) || !json_number(extra, "used_credits", &used)) {
        return;
    }
    limit /= 100.0;
    used /= 100.0;
    provider->provider_cost = g_new0(CodexBarProviderCost, 1);
    provider->provider_cost->used = used;
    provider->provider_cost->limit = limit;
    provider->provider_cost->currency = g_ascii_strup(currency_code(extra), -1);
    provider->provider_cost->period = g_strdup(has_primary ? "Monthly cap" : "Spend limit");
    provider->provider_cost->has_updated_at = TRUE;
    provider->provider_cost->updated_at_ms = now_ms;
    if (!has_primary && limit > 0) {
        double utilization = 0;
        if (!json_number(extra, "utilization", &utilization)) utilization = used / limit * 100.0;
        CodexBarQuotaWindow *window = codexbar_quota_window_new("spend-limit", "Spend limit");
        window->usage_known = TRUE;
        window->used_percent = CLAMP(utilization, 0, 100);
        codexbar_provider_add_quota_window(provider, window);
    }
}

static char *plan_label(const char *subscription_type, const char *rate_limit_tier) {
    const char *inputs[] = {subscription_type, rate_limit_tier};
    const char *plan = NULL;
    for (size_t index = 0; index < G_N_ELEMENTS(inputs) && !plan; index++) {
        if (!inputs[index]) continue;
        char *lower = g_ascii_strdown(inputs[index], -1);
        if (strstr(lower, "max")) plan = "Claude Max";
        else if (strstr(lower, "pro")) plan = "Claude Pro";
        else if (strstr(lower, "team")) plan = "Claude Team";
        else if (strstr(lower, "enterprise")) plan = "Claude Enterprise";
        else if (strstr(lower, "ultra")) plan = "Claude Ultra";
        g_free(lower);
    }
    if (!plan) return NULL;
    if (!g_str_equal(plan, "Claude Max") || !rate_limit_tier) return g_strdup(plan);
    char **words = g_strsplit_set(rate_limit_tier, "_- .", -1);
    char *multiplier = NULL;
    for (guint index = 0; words[index] && words[index + 1]; index++) {
        if (g_ascii_strcasecmp(words[index], "max") != 0) continue;
        size_t length = strlen(words[index + 1]);
        if (length > 1 && words[index + 1][length - 1] == 'x') {
            char *number = g_strndup(words[index + 1], length - 1);
            if (number[0] != '\0' && strspn(number, "0123456789") == strlen(number)) {
                multiplier = g_strdup(words[index + 1]);
            }
            g_free(number);
        }
        break;
    }
    g_strfreev(words);
    char *label = multiplier ? g_strdup_printf("%s %s", plan, multiplier) : g_strdup(plan);
    g_free(multiplier);
    return label;
}

CodexBarProvider *codexbar_claude_parse_oauth_usage(const char *text,
                                                    const char *rate_limit_tier,
                                                    const char *subscription_type,
                                                    gint64 updated_at_ms,
                                                    GError **error) {
    json_object *root = parse_json_object(text, error);
    if (!root) return NULL;
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("claude");
    provider->source = g_strdup("oauth");
    provider->explicit_quota_slots = TRUE;
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = updated_at_ms;
    provider->plan = plan_label(subscription_type, rate_limit_tier);
    if (provider->plan) {
        provider->identity = g_new0(CodexBarProviderIdentity, 1);
        provider->identity->login_method = g_strdup(provider->plan);
    }

    CodexBarQuotaWindow *primary = parse_window(root, "five_hour", "session", "session", 5 * 60);
    if (!primary) primary = parse_window(root, "seven_day", "weekly", "weekly", 7 * 24 * 60);
    if (!primary) {
        primary = parse_window(root, "seven_day_oauth_apps", "weekly-oauth-apps", "weekly", 7 * 24 * 60);
    }
    if (!primary) primary = parse_window(root, "seven_day_sonnet", "sonnet", "sonnet", 7 * 24 * 60);
    if (primary) codexbar_provider_add_quota_window(provider, primary);
    CodexBarQuotaWindow *weekly = parse_window(root, "seven_day", "weekly", "weekly", 7 * 24 * 60);
    if (weekly) codexbar_provider_add_quota_window(provider, weekly);
    CodexBarQuotaWindow *sonnet = parse_window(root, "seven_day_sonnet", "sonnet", "sonnet", 7 * 24 * 60);
    if (sonnet) codexbar_provider_add_quota_window(provider, sonnet);
    add_routines_window(provider, root);
    add_scoped_windows(provider, root);
    apply_extra_usage(provider, root, primary != NULL, updated_at_ms);
    json_object_put(root);

    if (provider->quota_windows->len == 0 && !provider->provider_cost) {
        codexbar_provider_free(provider);
        g_set_error_literal(error, claude_error_quark(), 5, "Claude OAuth response has no usage data");
        return NULL;
    }
    return provider;
}

CodexBarProvider *codexbar_claude_fetch(const CodexBarProviderConfig *config, GError **error) {
    (void)config;
    ClaudeCredentials credentials = {0};
    if (!load_credentials(&credentials, error)) return NULL;
    char *authorization = g_strdup_printf("Bearer %s", credentials.access_token);
    char *user_agent = claude_user_agent();
    const CodexBarHttpRequestHeader headers[] = {
        {"Authorization", authorization},
        {"Accept", "application/json"},
        {"Content-Type", "application/json"},
        {"anthropic-beta", "oauth-2025-04-20"},
        {"User-Agent", user_agent},
    };
    const CodexBarHttpRequest request = {
        .url = "https://api.anthropic.com/api/oauth/usage",
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 30,
        .maximum_response_bytes = CLAUDE_RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
    };
    CodexBarHttpResponse *response = codexbar_http_send(&request, error);
    g_free(user_agent);
    g_free(authorization);
    if (!response) {
        credentials_clear(&credentials);
        return NULL;
    }
    if (response->status != 200) {
        if (response->status == 401) {
            g_set_error_literal(error,
                                claude_error_quark(),
                                6,
                                "Claude OAuth request was unauthorized. Run `claude login`.");
        } else if (response->status == 429) {
            g_set_error_literal(error,
                                claude_error_quark(),
                                7,
                                "Claude OAuth usage is rate limited. Wait a few minutes, then refresh.");
        } else {
            g_set_error(error,
                        claude_error_quark(),
                        8,
                        "Claude OAuth request failed with HTTP %ld",
                        response->status);
        }
        codexbar_http_response_free(response);
        credentials_clear(&credentials);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_claude_parse_oauth_usage(response->body,
                                                                   credentials.rate_limit_tier,
                                                                   credentials.subscription_type,
                                                                   g_get_real_time() / 1000,
                                                                   error);
    codexbar_http_response_free(response);
    credentials_clear(&credentials);
    return provider;
}
