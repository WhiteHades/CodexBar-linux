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

static gboolean load_credentials(const CodexBarProviderConfig *config, ClaudeCredentials *credentials, GError **error) {
    json_object *configured_token = NULL;
    if (config && config->raw && json_object_object_get_ex(config->raw, "oauthToken", &configured_token) &&
        json_object_is_type(configured_token, json_type_string)) {
        credentials->access_token = g_strstrip(g_strdup(json_object_get_string(configured_token)));
        if (g_str_has_prefix(credentials->access_token, "sk-ant-oat")) return TRUE;
        g_clear_pointer(&credentials->access_token, g_free);
    }
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
        window->used_percent =
            codexbar_usage_percent_display(codexbar_usage_percent_from_raw(utilization));
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

CodexBarProvider *codexbar_claude_parse_web_usage(const char *text,
                                                  const char *organization,
                                                  const char *organization_id,
                                                  gint64 updated_at_ms,
                                                  GError **error) {
    json_object *root = parse_json_object(text, error);
    if (!root) return NULL;
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("claude");
    provider->source = g_strdup("web");
    provider->explicit_quota_slots = TRUE;
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = updated_at_ms;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->organization = organization ? g_strdup(organization) : NULL;
    provider->identity->account_id = organization_id ? g_strdup(organization_id) : NULL;

    CodexBarQuotaWindow *session = parse_window(root, "five_hour", "session", "session", 5 * 60);
    if (!session) {
        session = codexbar_quota_window_new("session", "session");
        session->usage_known = TRUE;
        session->used_percent = 0;
        session->has_window_minutes = TRUE;
        session->window_minutes = 5 * 60;
    }
    codexbar_provider_add_quota_window(provider, session);
    CodexBarQuotaWindow *weekly = parse_window(root, "seven_day", "weekly", "weekly", 7 * 24 * 60);
    if (weekly) codexbar_provider_add_quota_window(provider, weekly);
    CodexBarQuotaWindow *sonnet = parse_window(root, "seven_day_sonnet", "sonnet", "sonnet", 7 * 24 * 60);
    if (!sonnet) sonnet = parse_window(root, "seven_day_opus", "opus", "opus", 7 * 24 * 60);
    if (sonnet) codexbar_provider_add_quota_window(provider, sonnet);
    add_routines_window(provider, root);
    add_scoped_windows(provider, root);
    apply_extra_usage(provider, root, TRUE, updated_at_ms);
    json_object_put(root);
    return provider;
}

static char *strip_ansi(const char *text) {
    GString *clean = g_string_new(NULL);
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor;) {
        if (*cursor != 0x1b) {
            g_string_append_c(clean, (char)*cursor++);
            continue;
        }
        cursor++;
        if (*cursor == '[') {
            cursor++;
            while (*cursor && !(*cursor >= 0x40 && *cursor <= 0x7e)) cursor++;
            if (*cursor) cursor++;
        } else if (*cursor) {
            cursor++;
        }
    }
    return g_string_free(clean, FALSE);
}

static gboolean percent_from_line(const char *line, double *used_percent) {
    const char *percent = strchr(line, '%');
    if (!percent) return FALSE;
    const char *start = percent;
    while (start > line && (g_ascii_isdigit(start[-1]) || start[-1] == '.')) start--;
    if (start == percent) return FALSE;
    char *number = g_strndup(start, (gsize)(percent - start));
    char *end = NULL;
    double value = g_ascii_strtod(number, &end);
    gboolean valid = end && *end == '\0' && isfinite(value);
    g_free(number);
    if (!valid) return FALSE;
    char *lower = g_ascii_strdown(line, -1);
    *used_percent = strstr(lower, "left") ? 100.0 - value : value;
    *used_percent = CLAMP(*used_percent, 0, 100);
    g_free(lower);
    return TRUE;
}

static gboolean percent_after_label(char **lines, const char *label, double *used_percent) {
    char *wanted = g_ascii_strdown(label, -1);
    for (guint index = 0; lines[index]; index++) {
        char *lower = g_ascii_strdown(lines[index], -1);
        gboolean match = strstr(lower, wanted) != NULL;
        g_free(lower);
        if (!match) continue;
        for (guint offset = 0; offset < 12 && lines[index + offset]; offset++) {
            if (offset > 0) {
                char *candidate = g_ascii_strdown(lines[index + offset], -1);
                gboolean boundary = strstr(candidate, "current ") != NULL;
                g_free(candidate);
                if (boundary) break;
            }
            if (percent_from_line(lines[index + offset], used_percent)) {
                g_free(wanted);
                return TRUE;
            }
        }
    }
    g_free(wanted);
    return FALSE;
}

static char *line_value(char **lines, const char *prefix) {
    size_t length = strlen(prefix);
    for (guint index = 0; lines[index]; index++) {
        char *line = g_strstrip(lines[index]);
        if (g_ascii_strncasecmp(line, prefix, length) != 0) continue;
        char *value = g_strstrip(g_strdup(line + length));
        if (value[0] != '\0') return value;
        g_free(value);
    }
    return NULL;
}

CodexBarProvider *codexbar_claude_parse_cli_usage(const char *text, gint64 updated_at_ms, GError **error) {
    char *clean = strip_ansi(text ? text : "");
    char *lower = g_ascii_strdown(clean, -1);
    if (strstr(lower, "failed to load usage data") ||
        strstr(lower, "currently using your subscription to power your claude code usage")) {
        g_set_error_literal(error, claude_error_quark(), 9, "Claude CLI /usage did not return subscription quota data");
        g_free(lower);
        g_free(clean);
        return NULL;
    }
    g_free(lower);
    char **lines = g_strsplit(clean, "\n", -1);
    double session = 0;
    if (!percent_after_label(lines, "Current session", &session)) {
        g_strfreev(lines);
        g_free(clean);
        g_set_error_literal(error, claude_error_quark(), 9, "Could not parse Claude CLI Current session usage");
        return NULL;
    }
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("claude");
    provider->source = g_strdup("cli");
    provider->explicit_quota_slots = TRUE;
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = updated_at_ms;
    CodexBarQuotaWindow *window = codexbar_quota_window_new("session", "session");
    window->usage_known = TRUE;
    window->used_percent = session;
    window->has_window_minutes = TRUE;
    window->window_minutes = 5 * 60;
    codexbar_provider_add_quota_window(provider, window);
    double weekly = 0;
    if (percent_after_label(lines, "Current week (all models)", &weekly)) {
        window = codexbar_quota_window_new("weekly", "weekly");
        window->usage_known = TRUE;
        window->used_percent = weekly;
        window->has_window_minutes = TRUE;
        window->window_minutes = 7 * 24 * 60;
        codexbar_provider_add_quota_window(provider, window);
    }
    double sonnet = 0;
    if (percent_after_label(lines, "Current week (Sonnet only)", &sonnet) ||
        percent_after_label(lines, "Current week (Opus only)", &sonnet)) {
        window = codexbar_quota_window_new("sonnet", "sonnet");
        window->usage_known = TRUE;
        window->used_percent = sonnet;
        window->has_window_minutes = TRUE;
        window->window_minutes = 7 * 24 * 60;
        codexbar_provider_add_quota_window(provider, window);
    }
    provider->account = line_value(lines, "Account:");
    char *organization = line_value(lines, "Org:");
    if (provider->account || organization) {
        provider->identity = g_new0(CodexBarProviderIdentity, 1);
        provider->identity->organization = organization;
    } else {
        g_free(organization);
    }
    g_strfreev(lines);
    g_free(clean);
    return provider;
}

static CodexBarProvider *fetch_oauth(const CodexBarProviderConfig *config,
                                    CodexBarClaudeTransport transport,
                                    GCancellable *cancellable,
                                    gint64 now_ms,
                                    GError **error) {
    ClaudeCredentials credentials = {0};
    if (!load_credentials(config, &credentials, error)) return NULL;
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
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = transport(&request, error);
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
                                                                   now_ms,
                                                                   error);
    codexbar_http_response_free(response);
    credentials_clear(&credentials);
    return provider;
}

static char *configured_string(const CodexBarProviderConfig *config, const char *key) {
    return config && config->raw ? trimmed_json_string(config->raw, key) : NULL;
}

static char *clean_environment(const char *name) {
    const char *raw = g_getenv(name);
    if (!raw) return NULL;
    char *value = g_strstrip(g_strdup(raw));
    size_t length = strlen(value);
    if (length >= 2 && ((value[0] == '\'' && value[length - 1] == '\'') ||
                        (value[0] == '"' && value[length - 1] == '"'))) {
        value[length - 1] = '\0';
        memmove(value, value + 1, length - 1);
        g_strstrip(value);
    }
    if (value[0] != '\0') return value;
    g_free(value);
    return NULL;
}

static char *admin_api_key(const CodexBarProviderConfig *config) {
    char *key = config && config->api_key ? g_strstrip(g_strdup(config->api_key)) : NULL;
    if (key && key[0] == '\0') g_clear_pointer(&key, g_free);
    if (!key) key = clean_environment("ANTHROPIC_ADMIN_KEY");
    if (!key) key = clean_environment("ANTHROPIC_ADMIN_API_KEY");
    return key;
}

static char *manual_cookie_header(const CodexBarProviderConfig *config, GError **error) {
    char *cookie = configured_string(config, "cookieHeader");
    if (!cookie) cookie = clean_environment("CODEXBAR_CLAUDE_COOKIE_HEADER");
    if (!cookie) return NULL;
    if (strchr(cookie, '\r') || strchr(cookie, '\n')) {
        g_free(cookie);
        g_set_error_literal(error, claude_error_quark(), 10,
                            "Claude cookieHeader must contain only cookie pairs");
        return NULL;
    }
    if (g_ascii_strncasecmp(cookie, "Cookie:", 7) == 0) {
        char *value = g_strstrip(g_strdup(cookie + 7));
        g_free(cookie);
        cookie = value;
    }
    char **pairs = g_strsplit(cookie, ";", -1);
    gboolean found = FALSE;
    for (guint index = 0; pairs[index]; index++) {
        char *pair = g_strstrip(pairs[index]);
        if (!g_str_has_prefix(pair, "sessionKey=")) continue;
        const char *value = pair + strlen("sessionKey=");
        found = g_str_has_prefix(value, "sk-ant-") && value[strlen("sk-ant-")] != '\0';
        if (found) break;
    }
    g_strfreev(pairs);
    if (found) return cookie;
    g_free(cookie);
    g_set_error_literal(error, claude_error_quark(), 10,
                        "Claude web source needs a valid sessionKey cookie");
    return NULL;
}

static gboolean http_success(CodexBarHttpResponse *response, const char *source, GError **error) {
    if (response->status == 200) return TRUE;
    if (response->status == 401 || response->status == 403) {
        g_set_error(error, claude_error_quark(), 11, "Claude %s credentials were unauthorized", source);
    } else {
        g_set_error(error, claude_error_quark(), 12,
                    "Claude %s request failed with HTTP %ld", source, response->status);
    }
    return FALSE;
}

static gboolean select_organization(const char *text,
                                    const char *target_id,
                                    char **organization_id,
                                    char **organization_name,
                                    GError **error) {
    json_object *root = json_tokener_parse(text);
    if (!root || !json_object_is_type(root, json_type_array)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, claude_error_quark(), 13, "Claude organizations response was invalid");
        return FALSE;
    }
    json_object *selected = NULL;
    size_t count = json_object_array_length(root);
    for (size_t index = 0; index < count; index++) {
        json_object *candidate = json_object_array_get_idx(root, index);
        char *id = trimmed_json_string(candidate, "uuid");
        if (target_id && id && g_str_equal(target_id, id)) selected = candidate;
        if (!selected && !target_id && id) selected = candidate;
        g_free(id);
        if (selected && target_id) break;
    }
    if (!selected) {
        json_object_put(root);
        g_set_error(error, claude_error_quark(), 14,
                    target_id ? "Claude organization '%s' was not found" : "Claude account has no organization",
                    target_id ? target_id : "");
        return FALSE;
    }
    *organization_id = trimmed_json_string(selected, "uuid");
    *organization_name = trimmed_json_string(selected, "name");
    json_object_put(root);
    return TRUE;
}

static CodexBarProvider *fetch_web(const CodexBarProviderConfig *config,
                                  CodexBarClaudeTransport transport,
                                  GCancellable *cancellable,
                                  gint64 now_ms,
                                  GError **error) {
    char *cookie = manual_cookie_header(config, error);
    if (!cookie) {
        if (!error || !*error) {
            g_set_error_literal(error, claude_error_quark(), 10,
                                "Claude web source needs a manual cookieHeader");
        }
        return NULL;
    }
    const CodexBarHttpRequestHeader headers[] = {{"Cookie", cookie}, {"Accept", "application/json"}};
    const CodexBarHttpRequest organizations_request = {
        .url = "https://claude.ai/api/organizations",
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 15,
        .maximum_response_bytes = CLAUDE_RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = transport(&organizations_request, error);
    if (!response) {
        g_free(cookie);
        return NULL;
    }
    if (!http_success(response, "web organizations", error)) {
        codexbar_http_response_free(response);
        g_free(cookie);
        return NULL;
    }
    char *organization_id = NULL;
    char *organization_name = NULL;
    gboolean selected = select_organization(
        response->body, config ? config->workspace_id : NULL, &organization_id, &organization_name, error);
    codexbar_http_response_free(response);
    if (!selected) {
        g_free(cookie);
        return NULL;
    }
    char *escaped = g_uri_escape_string(organization_id, NULL, TRUE);
    char *url = g_strdup_printf("https://claude.ai/api/organizations/%s/usage", escaped);
    g_free(escaped);
    const CodexBarHttpRequest usage_request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 15,
        .maximum_response_bytes = CLAUDE_RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    response = transport(&usage_request, error);
    g_free(url);
    g_free(cookie);
    if (!response) {
        g_free(organization_name);
        g_free(organization_id);
        return NULL;
    }
    if (!http_success(response, "web usage", error)) {
        codexbar_http_response_free(response);
        g_free(organization_name);
        g_free(organization_id);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_claude_parse_web_usage(
        response->body, organization_name, organization_id, now_ms, error);
    codexbar_http_response_free(response);
    g_free(organization_name);
    g_free(organization_id);
    return provider;
}

static gboolean response_array(const char *text, json_object **root, GError **error) {
    *root = json_tokener_parse(text);
    json_object *data = NULL;
    if (*root && json_object_is_type(*root, json_type_object) &&
        json_object_object_get_ex(*root, "data", &data) && json_object_is_type(data, json_type_array)) {
        return TRUE;
    }
    if (*root) json_object_put(*root);
    *root = NULL;
    g_set_error_literal(error, claude_error_quark(), 15, "Claude Admin API response was invalid");
    return FALSE;
}

static double admin_cost_total(json_object *root) {
    json_object *data = json_object_object_get(root, "data");
    double total = 0;
    for (size_t bucket_index = 0; bucket_index < json_object_array_length(data); bucket_index++) {
        json_object *bucket = json_object_array_get_idx(data, bucket_index);
        json_object *results = json_object_object_get(bucket, "results");
        if (!results || !json_object_is_type(results, json_type_array)) continue;
        for (size_t index = 0; index < json_object_array_length(results); index++) {
            json_object *entry = json_object_array_get_idx(results, index);
            double amount = 0;
            if (json_number(entry, "amount", &amount)) total += amount / 100.0;
            else {
                char *raw = trimmed_json_string(entry, "amount");
                if (raw) total += g_ascii_strtod(raw, NULL) / 100.0;
                g_free(raw);
            }
        }
    }
    return total;
}

static gint64 object_integer(json_object *object, const char *key) {
    json_object *value = NULL;
    return object && json_object_object_get_ex(object, key, &value) &&
                   (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double))
               ? json_object_get_int64(value)
               : 0;
}

static gint64 admin_token_total(json_object *root) {
    json_object *data = json_object_object_get(root, "data");
    gint64 total = 0;
    for (size_t bucket_index = 0; bucket_index < json_object_array_length(data); bucket_index++) {
        json_object *bucket = json_object_array_get_idx(data, bucket_index);
        json_object *results = json_object_object_get(bucket, "results");
        if (!results || !json_object_is_type(results, json_type_array)) continue;
        for (size_t index = 0; index < json_object_array_length(results); index++) {
            json_object *entry = json_object_array_get_idx(results, index);
            total += object_integer(entry, "uncached_input_tokens");
            total += object_integer(entry, "cache_read_input_tokens");
            total += object_integer(entry, "output_tokens");
            json_object *cache = json_object_object_get(entry, "cache_creation");
            total += object_integer(cache, "ephemeral_1h_input_tokens");
            total += object_integer(cache, "ephemeral_5m_input_tokens");
        }
    }
    return total;
}

static char *admin_url(const char *base, gint64 now_ms, const char *group) {
    GDateTime *end = g_date_time_new_from_unix_utc(now_ms / 1000);
    GDateTime *start = g_date_time_add_days(end, -30);
    char *start_text = g_date_time_format(start, "%Y-%m-%dT00:00:00Z");
    char *end_text = g_date_time_format(end, "%Y-%m-%dT23:59:59Z");
    char *url = g_strdup_printf(
        "%s?starting_at=%s&ending_at=%s&bucket_width=1d&limit=31&group_by%%5B%%5D=%s",
        base, start_text, end_text, group);
    g_free(end_text);
    g_free(start_text);
    g_date_time_unref(start);
    g_date_time_unref(end);
    return url;
}

static CodexBarHttpResponse *admin_request(const char *url,
                                          const char *key,
                                          CodexBarClaudeTransport transport,
                                          GCancellable *cancellable,
                                          GError **error) {
    const CodexBarHttpRequestHeader headers[] = {
        {"x-api-key", key}, {"anthropic-version", "2023-06-01"},
        {"Accept", "application/json"}, {"User-Agent", "CodexBar/1.0"},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = G_N_ELEMENTS(headers),
        .timeout_seconds = 20,
        .maximum_response_bytes = CLAUDE_RESPONSE_LIMIT,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    return transport(&request, error);
}

static CodexBarProvider *fetch_admin_api(const CodexBarProviderConfig *config,
                                         CodexBarClaudeTransport transport,
                                         GCancellable *cancellable,
                                         gint64 now_ms,
                                         GError **error) {
    char *key = admin_api_key(config);
    if (!key) {
        g_set_error_literal(error, claude_error_quark(), 16,
                            "Claude API usage needs an Anthropic Admin API key");
        return NULL;
    }
    char *cost_url = admin_url("https://api.anthropic.com/v1/organizations/cost_report", now_ms, "description");
    CodexBarHttpResponse *cost_response = admin_request(cost_url, key, transport, cancellable, error);
    g_free(cost_url);
    if (!cost_response) {
        g_free(key);
        return NULL;
    }
    if (!http_success(cost_response, "Admin API cost report", error)) {
        codexbar_http_response_free(cost_response);
        g_free(key);
        return NULL;
    }
    json_object *cost_root = NULL;
    if (!response_array(cost_response->body, &cost_root, error)) {
        codexbar_http_response_free(cost_response);
        g_free(key);
        return NULL;
    }
    codexbar_http_response_free(cost_response);
    char *messages_url = admin_url(
        "https://api.anthropic.com/v1/organizations/usage_report/messages", now_ms, "model");
    CodexBarHttpResponse *messages_response = admin_request(messages_url, key, transport, cancellable, error);
    g_free(messages_url);
    g_free(key);
    if (!messages_response) {
        json_object_put(cost_root);
        return NULL;
    }
    if (!http_success(messages_response, "Admin API messages", error)) {
        codexbar_http_response_free(messages_response);
        json_object_put(cost_root);
        return NULL;
    }
    json_object *messages_root = NULL;
    if (!response_array(messages_response->body, &messages_root, error)) {
        codexbar_http_response_free(messages_response);
        json_object_put(cost_root);
        return NULL;
    }
    codexbar_http_response_free(messages_response);
    double cost = admin_cost_total(cost_root);
    gint64 tokens = admin_token_total(messages_root);
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("claude");
    provider->source = g_strdup("api");
    provider->plan = g_strdup("Admin API");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = g_strdup("Admin API");
    provider->provider_cost = g_new0(CodexBarProviderCost, 1);
    provider->provider_cost->used = cost;
    provider->provider_cost->limit = 0;
    provider->provider_cost->currency = g_strdup("USD");
    provider->provider_cost->period = g_strdup("Last 30 days");
    provider->provider_cost->has_updated_at = TRUE;
    provider->provider_cost->updated_at_ms = now_ms;
    provider->token_cost = g_new0(CodexBarTokenCost, 1);
    provider->token_cost->has_last_days_tokens = TRUE;
    provider->token_cost->last_days_tokens = tokens;
    provider->token_cost->has_last_days_cost = TRUE;
    provider->token_cost->last_days_cost = cost;
    provider->token_cost->currency = g_strdup("USD");
    provider->token_cost->history_label = g_strdup("Last 30 days");
    provider->token_cost->has_history_days = TRUE;
    provider->token_cost->history_days = 30;
    provider->usage_extensions = json_object_new_object();
    json_object *details = json_object_new_object();
    json_object_object_add(details, "costUSD", json_object_new_double(cost));
    json_object_object_add(details, "totalTokens", json_object_new_int64(tokens));
    json_object_object_add(provider->usage_extensions, "claudeAdminAPIUsage", details);
    json_object_put(messages_root);
    json_object_put(cost_root);
    return provider;
}

static CodexBarProvider *fetch_cli(CodexBarClaudeRunner runner,
                                   GCancellable *cancellable,
                                   gint64 now_ms,
                                   GError **error) {
    const char *binary = g_getenv("CLAUDE_CLI_PATH");
    if (!binary || binary[0] == '\0') binary = "claude";
    const char *arguments[] = {binary, "/usage", NULL};
    char **environment = g_get_environ();
    const char *secrets[] = {
        "CODEXBAR_CLAUDE_OAUTH_TOKEN", "CODEXBAR_CLAUDE_OAUTH_SCOPES", "ANTHROPIC_ADMIN_KEY",
        "ANTHROPIC_ADMIN_API_KEY", "ANTHROPIC_API_KEY", NULL,
    };
    for (guint index = 0; secrets[index]; index++) {
        environment = g_environ_unsetenv(environment, secrets[index]);
    }
    environment = g_environ_setenv(environment, "DISABLE_AUTOUPDATER", "1", TRUE);
    environment = g_environ_setenv(environment, "NO_COLOR", "1", TRUE);
    const CodexBarProcessRequest request = {
        .arguments = arguments,
        .environment = (const char *const *)environment,
        .timeout_milliseconds = 15000,
        .termination_grace_milliseconds = 300,
        .maximum_output_bytes = CLAUDE_RESPONSE_LIMIT,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = runner(&request, cancellable, error);
    g_strfreev(environment);
    if (!result) return NULL;
    if (!codexbar_process_result_succeeded(result)) {
        g_set_error(error, claude_error_quark(), 17,
                    "Claude CLI /usage exited with status %d", result->exit_status);
        codexbar_process_result_free(result);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_claude_parse_cli_usage(result->standard_output, now_ms, error);
    codexbar_process_result_free(result);
    return provider;
}

CodexBarProvider *codexbar_claude_fetch_with_adapters_for_runtime(
    const CodexBarProviderConfig *config,
    const char *source,
    gboolean cli_runtime,
    CodexBarClaudeTransport transport,
    CodexBarClaudeRunner runner,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    const char *selected = source ? source : "auto";
    if (g_str_equal(selected, "api")) return fetch_admin_api(config, transport, cancellable, now_ms, error);
    if (g_str_equal(selected, "oauth")) return fetch_oauth(config, transport, cancellable, now_ms, error);
    if (g_str_equal(selected, "web")) return fetch_web(config, transport, cancellable, now_ms, error);
    if (g_str_equal(selected, "cli")) return fetch_cli(runner, cancellable, now_ms, error);
    if (!g_str_equal(selected, "auto")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported Claude source: %s", selected);
        return NULL;
    }
    char *key = admin_api_key(config);
    if (key) {
        g_free(key);
        return fetch_admin_api(config, transport, cancellable, now_ms, error);
    }
    if (!cli_runtime) {
        GError *oauth_error = NULL;
        CodexBarProvider *provider = fetch_oauth(config, transport, cancellable, now_ms, &oauth_error);
        if (provider) return provider;
        if (cancellable && g_cancellable_is_cancelled(cancellable)) {
            if (oauth_error) g_propagate_error(error, oauth_error);
            else g_cancellable_set_error_if_cancelled(cancellable, error);
            return NULL;
        }
        g_clear_error(&oauth_error);
        GError *cli_error = NULL;
        provider = fetch_cli(runner, cancellable, now_ms, &cli_error);
        if (provider) return provider;
        if (cancellable && g_cancellable_is_cancelled(cancellable)) {
            if (cli_error) g_propagate_error(error, cli_error);
            else g_cancellable_set_error_if_cancelled(cancellable, error);
            return NULL;
        }
        g_clear_error(&cli_error);
    }
    GError *web_error = NULL;
    char *cookie = manual_cookie_header(config, &web_error);
    if (cookie) {
        g_free(cookie);
        CodexBarProvider *provider = fetch_web(config, transport, cancellable, now_ms, &web_error);
        if (provider) return provider;
        if (cancellable && g_cancellable_is_cancelled(cancellable)) {
            g_propagate_error(error, web_error);
            return NULL;
        }
    }
    g_clear_error(&web_error);
    if (cli_runtime) return fetch_cli(runner, cancellable, now_ms, error);
    g_set_error_literal(error, claude_error_quark(), 18, "No Claude OAuth, CLI, or web source is available");
    return NULL;
}

CodexBarProvider *codexbar_claude_fetch_with_adapters(const CodexBarProviderConfig *config,
                                                      const char *source,
                                                      CodexBarClaudeTransport transport,
                                                      CodexBarClaudeRunner runner,
                                                      GCancellable *cancellable,
                                                      gint64 now_ms,
                                                      GError **error) {
    return codexbar_claude_fetch_with_adapters_for_runtime(
        config, source, TRUE, transport, runner, cancellable, now_ms, error);
}

CodexBarProvider *codexbar_claude_fetch(const CodexBarProviderConfig *config,
                                       const char *source,
                                       GCancellable *cancellable,
                                       GError **error) {
    return codexbar_claude_fetch_for_runtime(config, source, TRUE, cancellable, error);
}

CodexBarProvider *codexbar_claude_fetch_for_runtime(const CodexBarProviderConfig *config,
                                                   const char *source,
                                                   gboolean cli_runtime,
                                                   GCancellable *cancellable,
                                                   GError **error) {
    return codexbar_claude_fetch_with_adapters_for_runtime(config,
                                                           source,
                                                           cli_runtime,
                                                           codexbar_http_send,
                                                           codexbar_process_run,
                                                           cancellable,
                                                           g_get_real_time() / 1000,
                                                           error);
}
