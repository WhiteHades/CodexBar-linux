#include "codex.h"

#include "http.h"
#include "managed_codex.h"
#include "version.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>
#include <sys/utsname.h>

#define CODEX_MAXIMUM_AUTH_BYTES (1024U * 1024U)
#define CODEX_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)
#define CODEX_PAT_WHOAMI_URL "https://auth.openai.com/api/accounts/v1/user-auth-credential/whoami"
#define CODEX_USAGE_URL "https://chatgpt.com/backend-api/wham/usage"

enum {
    CODEX_ERROR_MALFORMED = 1,
    CODEX_ERROR_CREDENTIALS_MISSING = 10,
    CODEX_ERROR_CREDENTIALS_INVALID = 11,
    CODEX_ERROR_UNAUTHORIZED = 12,
    CODEX_ERROR_HTTP = 13,
};

static GQuark codex_error_quark(void) {
    return g_quark_from_static_string("codexbar-codex-error");
}

static gboolean json_whitespace(char character) {
    return character == ' ' || character == '\t' || character == '\n' || character == '\r';
}

static json_object *parse_json_document(const char *json, size_t length) {
    if (!json || length > G_MAXINT || !g_utf8_validate(json, (gssize)length, NULL) ||
        memchr(json, '\0', length)) {
        return NULL;
    }
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

static char *normalized_plan(const char *plan) {
    if (g_str_equal(plan, "prolite")) return g_strdup("Pro 5x");
    if (g_str_equal(plan, "pro")) return g_strdup("Pro");
    if (g_str_equal(plan, "plus")) return g_strdup("Plus");
    if (g_str_equal(plan, "team")) return g_strdup("Team");
    if (g_str_equal(plan, "business")) return g_strdup("Business");
    if (g_str_equal(plan, "free")) return g_strdup("Free");
    return g_strdup(plan);
}

static void parse_window(json_object *limits, const char *key, const char *title, CodexBarProvider *provider) {
    json_object *object = NULL;
    json_object *used = NULL;
    if (!json_object_object_get_ex(limits, key, &object) || !json_object_is_type(object, json_type_object) ||
        !json_object_object_get_ex(object, "usedPercent", &used)) {
        return;
    }
    CodexBarQuotaWindow *window = codexbar_quota_window_new(key, title);
    window->usage_known = TRUE;
    window->used_percent = codexbar_usage_percent_from_raw(json_object_get_double(used)).raw;
    json_object *duration = NULL;
    if (json_object_object_get_ex(object, "windowDurationMins", &duration) &&
        (json_object_is_type(duration, json_type_int) || json_object_is_type(duration, json_type_double))) {
        window->has_window_minutes = TRUE;
        window->window_minutes = json_object_get_int64(duration);
    }
    json_object *reset = NULL;
    if (json_object_object_get_ex(object, "resetsAt", &reset) &&
        (json_object_is_type(reset, json_type_int) || json_object_is_type(reset, json_type_double))) {
        gint64 resets_at = json_object_get_int64(reset);
        if (resets_at >= 0 && resets_at <= G_MAXINT64 / 1000) {
            window->has_resets_at = TRUE;
            window->resets_at_ms = resets_at * 1000;
        }
    }
    codexbar_provider_add_quota_window(provider, window);
}

CodexBarProvider *codexbar_codex_parse_rate_limits(const char *json, GError **error) {
    json_object *root = json_tokener_parse(json);
    json_object *result = NULL;
    json_object *limits = NULL;
    if (!root || !json_object_object_get_ex(root, "result", &result) ||
        !json_object_object_get_ex(result, "rateLimits", &limits) || !json_object_is_type(limits, json_type_object)) {
        g_set_error_literal(error, codex_error_quark(), 1, "Codex rate limits response is malformed");
        if (root) json_object_put(root);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("codex");
    provider->source = g_strdup("cli");
    json_object *plan = NULL;
    if (json_object_object_get_ex(limits, "planType", &plan) && json_object_is_type(plan, json_type_string)) {
        provider->plan = normalized_plan(json_object_get_string(plan));
    }
    parse_window(limits, "primary", "session", provider);
    parse_window(limits, "secondary", "weekly", provider);
    json_object *credits = NULL;
    json_object *has_credits = NULL;
    json_object *balance = NULL;
    if (json_object_object_get_ex(limits, "credits", &credits) &&
        json_object_object_get_ex(credits, "hasCredits", &has_credits) &&
        json_object_get_boolean(has_credits) &&
        json_object_object_get_ex(credits, "balance", &balance) && !json_object_is_type(balance, json_type_null)) {
        codexbar_provider_add_balance(
            provider, codexbar_balance_new("credits", "credits", json_object_get_double(balance), "credits"));
    }
    json_object_put(root);
    return provider;
}

static gboolean json_number_value(json_object *object, const char *key, double *result) {
    json_object *value = NULL;
    if (!object || !json_object_object_get_ex(object, key, &value)) return FALSE;
    if (json_object_is_type(value, json_type_int) || json_object_is_type(value, json_type_double)) {
        *result = json_object_get_double(value);
        return TRUE;
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    const char *text = json_object_get_string(value);
    char *end = NULL;
    double parsed = g_ascii_strtod(text, &end);
    if (!end || end == text || *end != '\0') return FALSE;
    *result = parsed;
    return TRUE;
}

static void parse_http_window(json_object *rate_limit,
                              const char *key,
                              const char *id,
                              const char *title,
                              CodexBarProvider *provider) {
    json_object *window_value = NULL;
    if (!rate_limit || !json_object_object_get_ex(rate_limit, key, &window_value) ||
        !json_object_is_type(window_value, json_type_object)) {
        return;
    }
    double used_percent = 0;
    if (!json_number_value(window_value, "used_percent", &used_percent)) return;
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = codexbar_usage_percent_from_raw(used_percent).raw;
    double seconds = 0;
    if (json_number_value(window_value, "limit_window_seconds", &seconds) && isfinite(seconds) && seconds > 0) {
        double minutes = seconds / 60.0;
        if (minutes < 0x1p63) {
            window->has_window_minutes = TRUE;
            window->window_minutes = (gint64)minutes;
        }
    }
    double reset = 0;
    if (json_number_value(window_value, "reset_at", &reset) && isfinite(reset) && reset >= 0 && reset < 0x1p63) {
        gint64 reset_seconds = (gint64)reset;
        if (reset_seconds <= G_MAXINT64 / 1000) {
            window->has_resets_at = TRUE;
            window->resets_at_ms = reset_seconds * 1000;
        }
    }
    codexbar_provider_add_quota_window(provider, window);
}

static json_object *individual_limit(json_object *root) {
    json_object *limit = NULL;
    if (json_object_object_get_ex(root, "individual_limit", &limit) &&
        json_object_is_type(limit, json_type_object)) {
        return limit;
    }
    json_object *rate_limit = NULL;
    if (json_object_object_get_ex(root, "rate_limit", &rate_limit) &&
        json_object_is_type(rate_limit, json_type_object) &&
        json_object_object_get_ex(rate_limit, "individual_limit", &limit) &&
        json_object_is_type(limit, json_type_object)) {
        return limit;
    }
    json_object *spend_control = NULL;
    if (json_object_object_get_ex(root, "spend_control", &spend_control) &&
        json_object_is_type(spend_control, json_type_object) &&
        json_object_object_get_ex(spend_control, "individual_limit", &limit) &&
        json_object_is_type(limit, json_type_object)) {
        return limit;
    }
    return NULL;
}

static void parse_individual_limit(json_object *root,
                                   CodexBarProvider *provider,
                                   gint64 updated_at_ms) {
    json_object *object = individual_limit(root);
    double limit = 0;
    if (!object || !json_number_value(object, "limit", &limit) || !isfinite(limit) || limit <= 0) return;
    double used = 0;
    double remaining_percent = 0;
    gboolean has_used = json_number_value(object, "used", &used) && isfinite(used);
    gboolean has_remaining_percent = json_number_value(object, "remaining_percent", &remaining_percent) &&
                                        isfinite(remaining_percent);
    if (!has_used && has_remaining_percent) {
        used = limit * CLAMP(100.0 - remaining_percent, 0.0, 100.0) / 100.0;
    }
    used = MAX(0.0, used);
    if (!has_remaining_percent) {
        remaining_percent = CLAMP(100.0 - used / limit * 100.0, 0.0, 100.0);
    }
    CodexBarBalance *balance = codexbar_balance_new(
        "codex-credit-limit", "monthly credit limit", MAX(0.0, limit - used), "credits");
    balance->has_used = TRUE;
    balance->used = used;
    balance->has_limit = TRUE;
    balance->limit = limit;
    balance->has_remaining_percent = TRUE;
    balance->remaining_percent = CLAMP(remaining_percent, 0.0, 100.0);
    balance->has_updated_at = TRUE;
    balance->updated_at_ms = updated_at_ms;
    double reset = 0;
    if ((json_number_value(object, "resets_at", &reset) ||
         json_number_value(object, "reset_at", &reset)) &&
        isfinite(reset) && reset > 0 && reset <= (double)G_MAXINT64 / 1000.0) {
        balance->has_resets_at = TRUE;
        balance->resets_at_ms = (gint64)reset * 1000;
    }
    if (provider->balances->len == 0) {
        codexbar_provider_add_balance(
            provider, codexbar_balance_new("credits", "credits", 0.0, "credits"));
    }
    codexbar_provider_add_balance(provider, balance);
}

static CodexBarProvider *parse_http_usage_document(const char *json,
                                                   size_t length,
                                                   const char *source,
                                                   gint64 updated_at_ms,
                                                   GError **error) {
    json_object *root = parse_json_document(json, length);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, codex_error_quark(), CODEX_ERROR_MALFORMED,
                            "Codex usage API response is malformed");
        return NULL;
    }
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("codex");
    provider->source = g_strdup(source);
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = updated_at_ms;
    json_object *plan = NULL;
    if (json_object_object_get_ex(root, "plan_type", &plan) && json_object_is_type(plan, json_type_string)) {
        provider->plan = normalized_plan(json_object_get_string(plan));
    }
    json_object *rate_limit = NULL;
    if (json_object_object_get_ex(root, "rate_limit", &rate_limit) &&
        json_object_is_type(rate_limit, json_type_object)) {
        parse_http_window(rate_limit, "primary_window", "primary", "session", provider);
        parse_http_window(rate_limit, "secondary_window", "secondary", "weekly", provider);
    }
    json_object *credits = NULL;
    json_object *has_credits = NULL;
    double balance = 0;
    if (json_object_object_get_ex(root, "credits", &credits) && json_object_is_type(credits, json_type_object) &&
        json_object_object_get_ex(credits, "has_credits", &has_credits) &&
        json_object_get_boolean(has_credits) && json_number_value(credits, "balance", &balance)) {
        codexbar_provider_add_balance(provider, codexbar_balance_new("credits", "credits", balance, "credits"));
    }
    parse_individual_limit(root, provider, updated_at_ms);
    json_object_put(root);
    if (provider->quota_windows->len == 0 && provider->balances->len == 0) {
        codexbar_provider_free(provider);
        g_set_error_literal(error, codex_error_quark(), CODEX_ERROR_MALFORMED,
                            "Codex usage API response has no usage data");
        return NULL;
    }
    return provider;
}

CodexBarProvider *codexbar_codex_parse_http_usage(const char *json,
                                                  const char *source,
                                                  gint64 updated_at_ms,
                                                  GError **error) {
    return parse_http_usage_document(
        json, json ? strlen(json) : 0, source, updated_at_ms, error);
}

gboolean codexbar_codex_apply_account(CodexBarProvider *provider, const char *json, GError **error) {
    json_object *root = json_tokener_parse(json);
    json_object *result = NULL;
    json_object *account = NULL;
    if (!root || !json_object_object_get_ex(root, "result", &result) ||
        !json_object_object_get_ex(result, "account", &account) ||
        !json_object_is_type(account, json_type_object)) {
        g_set_error_literal(error, codex_error_quark(), 5, "Codex account response is malformed");
        if (root) json_object_put(root);
        return FALSE;
    }

    json_object *type = NULL;
    if (json_object_object_get_ex(account, "type", &type) && json_object_is_type(type, json_type_string) &&
        g_str_equal(json_object_get_string(type), "chatgpt")) {
        json_object *email = NULL;
        json_object *plan = NULL;
        if (json_object_object_get_ex(account, "email", &email) && json_object_is_type(email, json_type_string)) {
            g_free(provider->account);
            provider->account = g_strdup(json_object_get_string(email));
        }
        if (json_object_object_get_ex(account, "planType", &plan) && json_object_is_type(plan, json_type_string)) {
            g_free(provider->plan);
            provider->plan = normalized_plan(json_object_get_string(plan));
        }
    }
    json_object_put(root);
    return TRUE;
}

static gboolean write_message(GOutputStream *stream, const char *message, GError **error) {
    char *line = g_strdup_printf("%s\n", message);
    gsize written = 0;
    gboolean success = g_output_stream_write_all(stream, line, strlen(line), &written, NULL, error) &&
                       g_output_stream_flush(stream, NULL, error);
    g_free(line);
    return success;
}

static char *read_response(
    GDataInputStream *stream, GPollableInputStream *pollable, int wanted_id, GError **error) {
    gint64 deadline = g_get_monotonic_time() + (8 * G_TIME_SPAN_SECOND);
    for (int lines = 0; lines < 64; lines++) {
        while (!g_pollable_input_stream_is_readable(pollable)) {
            if (g_get_monotonic_time() >= deadline) {
                g_set_error(error, codex_error_quark(), 4, "Codex RPC timed out waiting for response %d", wanted_id);
                return NULL;
            }
            g_usleep(10 * 1000);
        }
        gsize length = 0;
        char *line = g_data_input_stream_read_line(stream, &length, NULL, error);
        if (!line) {
            return NULL;
        }
        json_object *message = json_tokener_parse(line);
        json_object *id = NULL;
        gboolean matches = message && json_object_object_get_ex(message, "id", &id) &&
                           json_object_get_int(id) == wanted_id;
        if (matches) {
            json_object *rpc_error = NULL;
            if (json_object_object_get_ex(message, "error", &rpc_error)) {
                json_object *text = NULL;
                const char *description = json_object_object_get_ex(rpc_error, "message", &text)
                                              ? json_object_get_string(text)
                                              : "unknown RPC error";
                g_set_error(error, codex_error_quark(), 2, "Codex RPC failed: %s", description);
                json_object_put(message);
                g_free(line);
                return NULL;
            }
            json_object_put(message);
            return line;
        }
        if (message) json_object_put(message);
        g_free(line);
    }
    g_set_error_literal(error, codex_error_quark(), 3, "Codex RPC produced too many unrelated messages");
    return NULL;
}

CodexBarProvider *codexbar_codex_fetch_with_home(const char *home_path, GError **error) {
    const char *binary = g_getenv("CODEX_CLI_PATH");
    if (!binary || binary[0] == '\0') binary = "codex";
    const char *argv[] = {binary, "-s", "read-only", "-a", "untrusted", "app-server", NULL};
    GSubprocessLauncher *launcher = g_subprocess_launcher_new(
        G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE);
    if (home_path) g_subprocess_launcher_setenv(launcher, "CODEX_HOME", home_path, TRUE);
    GSubprocess *process = g_subprocess_launcher_spawnv(launcher, argv, error);
    g_object_unref(launcher);
    if (!process) return NULL;
    GOutputStream *input = g_subprocess_get_stdin_pipe(process);
    GInputStream *raw_output = g_subprocess_get_stdout_pipe(process);
    GDataInputStream *output = g_data_input_stream_new(raw_output);
    char *initialize = g_strdup_printf(
        "{\"id\":1,\"method\":\"initialize\",\"params\":{\"clientInfo\":{\"name\":\"codexbar-linux\",\"version\":\"%s\"}}}",
        CODEXBAR_LINUX_VERSION);
    gboolean sent = write_message(input, initialize, error);
    g_free(initialize);
    char *response = sent ? read_response(output, G_POLLABLE_INPUT_STREAM(raw_output), 1, error) : NULL;
    g_free(response);
    if (!response && error && *error) {
        g_subprocess_force_exit(process);
        g_object_unref(output);
        g_object_unref(process);
        return NULL;
    }
    sent = write_message(input, "{\"method\":\"initialized\",\"params\":{}}", error) &&
           write_message(input, "{\"id\":2,\"method\":\"account/rateLimits/read\",\"params\":{}}", error);
    response = sent ? read_response(output, G_POLLABLE_INPUT_STREAM(raw_output), 2, error) : NULL;
    CodexBarProvider *provider = response ? codexbar_codex_parse_rate_limits(response, error) : NULL;
    g_free(response);
    if (provider) {
        GError *account_error = NULL;
        if (write_message(input, "{\"id\":3,\"method\":\"account/read\",\"params\":{}}", &account_error)) {
            response = read_response(output, G_POLLABLE_INPUT_STREAM(raw_output), 3, &account_error);
            if (response) {
                codexbar_codex_apply_account(provider, response, &account_error);
                g_free(response);
            }
        }
        g_clear_error(&account_error);
    }
    g_subprocess_force_exit(process);
    g_object_unref(output);
    g_object_unref(process);
    return provider;
}

static char *config_string(const CodexBarProviderConfig *config, const char *key) {
    json_object *value = NULL;
    if (!config || !config->raw || !json_object_object_get_ex(config->raw, key, &value) ||
        !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    char *text = g_strstrip(g_strdup(json_object_get_string(value)));
    if (text[0] != '\0') return text;
    g_free(text);
    return NULL;
}

static char *clean_environment(const char *name) {
    const char *raw = g_getenv(name);
    if (!raw) return NULL;
    char *value = g_strstrip(g_strdup(raw));
    if (value[0] != '\0') return value;
    g_free(value);
    return NULL;
}

static char *clean_json_header_value(json_object *value, gboolean *invalid) {
    *invalid = FALSE;
    if (!value || !json_object_is_type(value, json_type_string)) return NULL;
    const char *raw = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!raw || memchr(raw, '\0', length) || !g_utf8_validate(raw, (gssize)length, NULL)) {
        *invalid = TRUE;
        return NULL;
    }
    char *clean = g_strstrip(g_strndup(raw, length));
    for (const unsigned char *cursor = (const unsigned char *)clean; *cursor; cursor++) {
        if (*cursor < 32 || *cursor == 127) {
            *invalid = TRUE;
            g_free(clean);
            return NULL;
        }
    }
    if (clean[0] != '\0') return clean;
    g_free(clean);
    return NULL;
}

static char *ambient_codex_home(void) {
    char *home = clean_environment("HOME");
    if (!home) home = g_strdup(g_get_home_dir());
    char *codex_home = g_build_filename(home, ".codex", NULL);
    g_free(home);
    return codex_home;
}

static gboolean canonical_paths_equal(const char *left, const char *right) {
    if (!left || !right) return FALSE;
    char *canonical_left = g_canonicalize_filename(left, NULL);
    char *canonical_right = g_canonicalize_filename(right, NULL);
    gboolean equal = g_str_equal(canonical_left, canonical_right);
    g_free(canonical_left);
    g_free(canonical_right);
    return equal;
}

static gboolean managed_or_fail_closed_home(const char *path) {
    if (!path || path[0] == '\0') return FALSE;
    char *canonical = g_canonicalize_filename(path, NULL);
    char *basename = g_path_get_basename(canonical);
    char *fail_closed_basename = g_path_get_basename(CODEXBAR_MANAGED_CODEX_UNREADABLE_HOME);
    gboolean fail_closed = g_str_equal(basename, fail_closed_basename);
    gboolean managed = FALSE;
    char **parts = g_strsplit(canonical, G_DIR_SEPARATOR_S, -1);
    for (guint index = 0; parts[index]; index++) {
        if (g_str_equal(parts[index], "managed-codex-homes")) {
            managed = TRUE;
            break;
        }
    }
    g_strfreev(parts);
    g_free(fail_closed_basename);
    g_free(basename);
    g_free(canonical);
    return fail_closed || managed;
}

static const char *configured_pat_scope_home(const CodexBarProviderConfig *config) {
    if (config && config->has_codex_active_source) {
        if (config->codex_active_source == CODEXBAR_CODEX_SOURCE_MANAGED_ACCOUNT) {
            return CODEXBAR_MANAGED_CODEX_UNREADABLE_HOME;
        }
        if (config->codex_active_source == CODEXBAR_CODEX_SOURCE_PROFILE_HOME &&
            config->codex_active_home_path && config->codex_active_home_path[0] != '\0') {
            return config->codex_active_home_path;
        }
    }
    const char *environment_home = g_getenv("CODEX_HOME");
    return environment_home && environment_home[0] != '\0' ? environment_home : NULL;
}

static gboolean load_pat_from_home(const char *codex_home, char **token, GError **error) {
    *token = NULL;
    char *path = g_build_filename(codex_home, "auth.json", NULL);
    char *contents = NULL;
    gsize length = 0;
    GError *read_error = NULL;
    if (!g_file_get_contents(path, &contents, &length, &read_error)) {
        if (read_error && read_error->domain == G_FILE_ERROR && read_error->code == G_FILE_ERROR_NOENT) {
            g_set_error(error,
                        codex_error_quark(),
                        CODEX_ERROR_CREDENTIALS_MISSING,
                        "Codex auth.json was not found at %s. Run `codex login` to sign in.",
                        path);
        } else {
            g_set_error(error,
                        codex_error_quark(),
                        CODEX_ERROR_CREDENTIALS_MISSING,
                        "Codex auth.json at %s could not be read. Check its permissions or run `codex login`.",
                        path);
        }
        g_clear_error(&read_error);
        g_free(path);
        return FALSE;
    }
    g_free(path);
    if (length == 0 || length > CODEX_MAXIMUM_AUTH_BYTES) {
        g_free(contents);
        g_set_error_literal(error,
                            codex_error_quark(),
                            CODEX_ERROR_CREDENTIALS_INVALID,
                            "Codex auth.json is empty or exceeds the 1 MiB safety limit");
        return FALSE;
    }
    json_object *root = parse_json_document(contents, length);
    g_free(contents);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error,
                            codex_error_quark(),
                            CODEX_ERROR_CREDENTIALS_INVALID,
                            "Codex auth.json contains invalid JSON");
        return FALSE;
    }

    json_object *snake = NULL;
    json_object *camel = NULL;
    json_object_object_get_ex(root, "personal_access_token", &snake);
    json_object_object_get_ex(root, "personalAccessToken", &camel);
    gboolean invalid = FALSE;
    *token = clean_json_header_value(snake, &invalid);
    if (!*token && !invalid) *token = clean_json_header_value(camel, &invalid);
    json_object_put(root);
    if (invalid) {
        g_clear_pointer(token, g_free);
        g_set_error_literal(error,
                            codex_error_quark(),
                            CODEX_ERROR_CREDENTIALS_INVALID,
                            "Codex personal access token contains invalid characters");
        return FALSE;
    }
    if (*token) return TRUE;
    g_set_error_literal(error,
                        codex_error_quark(),
                        CODEX_ERROR_CREDENTIALS_MISSING,
                        "Codex auth.json contains no personal access token");
    return FALSE;
}

static gboolean load_pat_credentials(const CodexBarProviderConfig *config, char **token, GError **error) {
    char *ambient_home = ambient_codex_home();
    const char *scoped_home = configured_pat_scope_home(config);
    if (scoped_home && !managed_or_fail_closed_home(scoped_home) &&
        !canonical_paths_equal(scoped_home, ambient_home)) {
        GError *scoped_error = NULL;
        if (load_pat_from_home(scoped_home, token, &scoped_error)) {
            g_clear_error(&scoped_error);
            g_free(ambient_home);
            return TRUE;
        }
        /* Profile homes keep a local PAT when present and otherwise use ambient ~/.codex. */
        g_clear_error(&scoped_error);
    }
    gboolean loaded = load_pat_from_home(ambient_home, token, error);
    g_free(ambient_home);
    return loaded;
}

gboolean codexbar_codex_pat_is_available(const CodexBarProviderConfig *config) {
    char *token = NULL;
    GError *error = NULL;
    gboolean available = load_pat_credentials(config, &token, &error);
    g_clear_pointer(&token, g_free);
    g_clear_error(&error);
    return available;
}

static gboolean load_oauth_credentials(const CodexBarProviderConfig *config,
                                       char **access_token,
                                       char **account_id,
                                       GError **error) {
    *access_token = config_string(config, "oauthToken");
    if (!*access_token) *access_token = clean_environment("CODEXBAR_CODEX_OAUTH_TOKEN");
    *account_id = config_string(config, "accountID");
    if (!*account_id) *account_id = clean_environment("CODEXBAR_CODEX_ACCOUNT_ID");
    if (*access_token) return TRUE;

    const char *configured_home = g_getenv("CODEX_HOME");
    char *codex_home = configured_home && configured_home[0] != '\0'
                           ? g_strdup(configured_home)
                           : g_build_filename(g_get_home_dir(), ".codex", NULL);
    char *path = g_build_filename(codex_home, "auth.json", NULL);
    g_free(codex_home);
    char *contents = NULL;
    gsize length = 0;
    GError *read_error = NULL;
    if (!g_file_get_contents(path, &contents, &length, &read_error)) {
        g_set_error(error, codex_error_quark(), CODEX_ERROR_CREDENTIALS_MISSING,
                    "Codex auth.json was not found. Run `codex` to log in.");
        g_clear_error(&read_error);
        g_free(path);
        return FALSE;
    }
    g_free(path);
    if (length == 0 || length > CODEX_MAXIMUM_AUTH_BYTES) {
        g_free(contents);
        g_set_error_literal(error, codex_error_quark(), CODEX_ERROR_CREDENTIALS_INVALID,
                            "Codex auth.json is invalid");
        return FALSE;
    }
    json_object *root = parse_json_document(contents, length);
    g_free(contents);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, codex_error_quark(), CODEX_ERROR_CREDENTIALS_INVALID,
                            "Codex auth.json contains invalid JSON");
        return FALSE;
    }
    json_object *tokens = NULL;
    if (json_object_object_get_ex(root, "tokens", &tokens) && json_object_is_type(tokens, json_type_object)) {
        json_object *value = NULL;
        if ((json_object_object_get_ex(tokens, "access_token", &value) ||
             json_object_object_get_ex(tokens, "accessToken", &value)) &&
            json_object_is_type(value, json_type_string)) {
            *access_token = g_strstrip(g_strdup(json_object_get_string(value)));
        }
        value = NULL;
        if (!*account_id && (json_object_object_get_ex(tokens, "account_id", &value) ||
                             json_object_object_get_ex(tokens, "accountId", &value)) &&
            json_object_is_type(value, json_type_string)) {
            *account_id = g_strstrip(g_strdup(json_object_get_string(value)));
        }
    }
    json_object_put(root);
    if (*access_token && (*access_token)[0] != '\0') return TRUE;
    g_clear_pointer(access_token, g_free);
    g_clear_pointer(account_id, g_free);
    g_set_error_literal(error, codex_error_quark(), CODEX_ERROR_CREDENTIALS_MISSING,
                        "Codex auth.json contains no OAuth access token");
    return FALSE;
}

static char *manual_cookie(const CodexBarProviderConfig *config, GError **error) {
    char *cookie = config_string(config, "cookieHeader");
    if (!cookie) cookie = clean_environment("CODEXBAR_CODEX_COOKIE_HEADER");
    if (!cookie) {
        g_set_error_literal(error, codex_error_quark(), CODEX_ERROR_CREDENTIALS_MISSING,
                            "Codex web source needs a manual cookieHeader");
        return NULL;
    }
    if (strchr(cookie, '\r') || strchr(cookie, '\n')) {
        g_free(cookie);
        g_set_error_literal(error, codex_error_quark(), CODEX_ERROR_CREDENTIALS_INVALID,
                            "Codex cookieHeader must contain only cookie pairs");
        return NULL;
    }
    if (g_ascii_strncasecmp(cookie, "Cookie:", 7) == 0) {
        char *value = g_strstrip(g_strdup(cookie + 7));
        g_free(cookie);
        cookie = value;
    }
    if (cookie[0] != '\0') return cookie;
    g_free(cookie);
    g_set_error_literal(error, codex_error_quark(), CODEX_ERROR_CREDENTIALS_INVALID,
                        "Codex cookieHeader is empty");
    return NULL;
}

typedef struct {
    char *account_id;
    char *email;
    char *plan_type;
} CodexPatWhoami;

static void pat_whoami_clear(CodexPatWhoami *whoami) {
    g_clear_pointer(&whoami->account_id, g_free);
    g_clear_pointer(&whoami->email, g_free);
    g_clear_pointer(&whoami->plan_type, g_free);
}

static gboolean optional_clean_json_string(json_object *root, const char *key, char **result) {
    *result = NULL;
    json_object *value = NULL;
    if (!json_object_object_get_ex(root, key, &value) || json_object_is_type(value, json_type_null)) {
        return TRUE;
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    gboolean invalid = FALSE;
    *result = clean_json_header_value(value, &invalid);
    return !invalid;
}

static gboolean parse_pat_whoami(const char *json,
                                 size_t length,
                                 CodexPatWhoami *whoami,
                                 GError **error) {
    json_object *root = parse_json_document(json, length);
    gboolean valid = root && json_object_is_type(root, json_type_object) &&
                     optional_clean_json_string(root, "chatgpt_account_id", &whoami->account_id) &&
                     optional_clean_json_string(root, "email", &whoami->email) &&
                     optional_clean_json_string(root, "chatgpt_plan_type", &whoami->plan_type);
    if (root) json_object_put(root);
    if (valid) return TRUE;
    pat_whoami_clear(whoami);
    g_set_error_literal(error,
                        codex_error_quark(),
                        CODEX_ERROR_MALFORMED,
                        "Codex personal-access-token whoami response is malformed");
    return FALSE;
}

static char *normalized_cli_version(const char *raw) {
    if (!raw || !g_utf8_validate(raw, -1, NULL)) return NULL;
    char *copy = g_strstrip(g_strdup(raw));
    if (copy[0] == '\0') {
        g_free(copy);
        return NULL;
    }
    char **parts = g_strsplit_set(copy, " \t\r\n", -1);
    const char *first = NULL;
    const char *second = NULL;
    for (guint index = 0; parts[index]; index++) {
        if (parts[index][0] == '\0') continue;
        if (!first) {
            first = parts[index];
        } else {
            second = parts[index];
            break;
        }
    }
    const char *candidate = first && second && g_ascii_strcasecmp(first, "codex-cli") == 0
                                ? second
                                : first;
    char *version = candidate ? g_strdup(candidate) : NULL;
    for (const unsigned char *cursor = (const unsigned char *)version; version && *cursor; cursor++) {
        if (*cursor < 33 || *cursor == 127) {
            g_clear_pointer(&version, g_free);
            break;
        }
    }
    g_strfreev(parts);
    g_free(copy);
    return version;
}

static char *resolved_cli_version(const CodexBarProviderConfig *config) {
    char *raw = config_string(config, "cliVersion");
    if (!raw) raw = clean_environment("CODEXBAR_CODEX_CLI_VERSION");
    char *version = normalized_cli_version(raw);
    g_free(raw);
    return version;
}

static char *codex_cli_user_agent(const CodexBarProviderConfig *config) {
    struct utsname system = {0};
    const char *release = "unknown";
    const char *machine = "unknown";
    if (uname(&system) == 0) {
        if (system.release[0] != '\0') release = system.release;
        if (system.machine[0] != '\0') machine = system.machine;
    }
    char *version = resolved_cli_version(config);
    char *user_agent = version
                           ? g_strdup_printf("codex_cli_rs/%s (Linux %s; %s)", version, release, machine)
                           : g_strdup_printf("codex_cli_rs (Linux %s; %s)", release, machine);
    g_free(version);
    return user_agent;
}

static CodexBarHttpResponse *pat_request(const char *url,
                                         const char *endpoint,
                                         const char *token,
                                         const char *account_id,
                                         const char *user_agent,
                                         CodexBarCodexTransport transport,
                                         GCancellable *cancellable,
                                         GError **error) {
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) return NULL;
    char *authorization = g_strdup_printf("Bearer %s", token);
    CodexBarHttpRequestHeader headers[5] = {
        {"Authorization", authorization},
        {"User-Agent", user_agent},
        {"Accept", "application/json"},
        {"originator", "codex_cli_rs"},
        {"ChatGPT-Account-Id", account_id},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = account_id ? G_N_ELEMENTS(headers) : G_N_ELEMENTS(headers) - 1,
        .timeout_seconds = 30,
        .maximum_response_bytes = CODEX_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = g_str_has_prefix(url, "http://") ? CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP
                                                            : CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = transport(&request, error);
    g_free(authorization);
    if (cancellable && g_cancellable_is_cancelled(cancellable)) {
        codexbar_http_response_free(response);
        if (error && *error) g_clear_error(error);
        g_cancellable_set_error_if_cancelled(cancellable, error);
        return NULL;
    }
    if (!response) {
        if (error && !*error) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_FAILED,
                                "Codex personal-access-token network request failed");
        }
        return NULL;
    }
    if (response->status == 401 || response->status == 403) {
        g_set_error(error,
                    codex_error_quark(),
                    CODEX_ERROR_UNAUTHORIZED,
                    "Codex personal access token was unauthorized by the %s endpoint",
                    endpoint);
        codexbar_http_response_free(response);
        return NULL;
    }
    if (response->status < 200 || response->status >= 300) {
        g_set_error(error,
                    codex_error_quark(),
                    CODEX_ERROR_HTTP,
                    "Codex personal-access-token %s request failed with HTTP %ld",
                    endpoint,
                    response->status);
        codexbar_http_response_free(response);
        return NULL;
    }
    return response;
}

static CodexBarProvider *fetch_pat_usage(const CodexBarProviderConfig *config,
                                         CodexBarCodexTransport transport,
                                         GCancellable *cancellable,
                                         GError **error) {
    char *token = NULL;
    if (!load_pat_credentials(config, &token, error)) return NULL;
    char *user_agent = codex_cli_user_agent(config);
    CodexPatWhoami whoami = {0};
    CodexBarProvider *provider = NULL;

    CodexBarHttpResponse *response = pat_request(CODEX_PAT_WHOAMI_URL,
                                                 "whoami",
                                                 token,
                                                 NULL,
                                                 user_agent,
                                                 transport,
                                                 cancellable,
                                                 error);
    if (!response) goto cleanup;
    gboolean parsed = parse_pat_whoami(response->body, response->body_length, &whoami, error);
    codexbar_http_response_free(response);
    if (!parsed) goto cleanup;

    const char *override = g_getenv("CODEXBAR_CODEX_USAGE_URL");
    const char *usage_url = override && override[0] != '\0' ? override : CODEX_USAGE_URL;
    response = pat_request(usage_url,
                           "usage",
                           token,
                           whoami.account_id,
                           user_agent,
                           transport,
                           cancellable,
                           error);
    if (!response) goto cleanup;
    provider = parse_http_usage_document(
        response->body, response->body_length, "api", g_get_real_time() / 1000, error);
    codexbar_http_response_free(response);
    if (!provider) goto cleanup;

    char *whoami_plan = whoami.plan_type ? normalized_plan(whoami.plan_type) : NULL;
    if (!provider->plan && whoami_plan) provider->plan = g_strdup(whoami_plan);
    if (whoami.email) {
        g_free(provider->account);
        provider->account = g_strdup(whoami.email);
    }
    if (whoami.account_id || provider->plan || whoami_plan) {
        if (!provider->identity) provider->identity = g_new0(CodexBarProviderIdentity, 1);
        if (whoami.account_id) provider->identity->account_id = g_strdup(whoami.account_id);
        provider->identity->login_method = g_strdup(provider->plan ? provider->plan : whoami_plan);
    }
    g_free(whoami_plan);
    if (!provider->usage_extensions) provider->usage_extensions = json_object_new_object();
    json_object_object_add(
        provider->usage_extensions, "dataConfidence", json_object_new_string("exact"));
    json_object_object_add(
        provider->usage_extensions, "codexCredentialKind", json_object_new_string("pat"));

cleanup:
    pat_whoami_clear(&whoami);
    g_free(user_agent);
    g_free(token);
    return provider;
}

static CodexBarProvider *fetch_http_usage(const CodexBarProviderConfig *config,
                                          const char *source,
                                          CodexBarCodexTransport transport,
                                          GCancellable *cancellable,
                                          GError **error) {
    char *credential = NULL;
    char *account_id = NULL;
    char *authorization = NULL;
    if (g_str_equal(source, "oauth")) {
        if (!load_oauth_credentials(config, &credential, &account_id, error)) return NULL;
        authorization = g_strdup_printf("Bearer %s", credential);
    } else {
        credential = manual_cookie(config, error);
        if (!credential) return NULL;
    }
    const char *override = g_getenv("CODEXBAR_CODEX_USAGE_URL");
    const char *url = override && override[0] != '\0'
                          ? override
                          : CODEX_USAGE_URL;
    CodexBarHttpRequestHeader headers[5] = {
        {g_str_equal(source, "oauth") ? "Authorization" : "Cookie",
         g_str_equal(source, "oauth") ? authorization : credential},
        {"Accept", "application/json"},
        {"User-Agent", "CodexBar"},
        {"OpenAI-Beta", "codex-1"},
        {"ChatGPT-Account-Id", account_id},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = account_id ? G_N_ELEMENTS(headers) : G_N_ELEMENTS(headers) - 1,
        .timeout_seconds = 30,
        .maximum_response_bytes = CODEX_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = g_str_has_prefix(url, "http://") ? CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP
                                                           : CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
        .cancellable = cancellable,
    };
    CodexBarHttpResponse *response = transport(&request, error);
    g_free(authorization);
    g_free(account_id);
    g_free(credential);
    if (!response) return NULL;
    if (response->status == 401 || response->status == 403) {
        g_set_error(error, codex_error_quark(), CODEX_ERROR_UNAUTHORIZED,
                    "Codex %s credentials were unauthorized", source);
        codexbar_http_response_free(response);
        return NULL;
    }
    if (response->status < 200 || response->status >= 300) {
        g_set_error(error, codex_error_quark(), CODEX_ERROR_HTTP,
                    "Codex %s usage request failed with HTTP %ld", source, response->status);
        codexbar_http_response_free(response);
        return NULL;
    }
    CodexBarProvider *provider = parse_http_usage_document(
        response->body, response->body_length, source, g_get_real_time() / 1000, error);
    codexbar_http_response_free(response);
    return provider;
}

static gboolean source_error_allows_fallback(const GError *error) {
    return error && error->domain == codex_error_quark() &&
           (error->code == CODEX_ERROR_CREDENTIALS_MISSING ||
            error->code == CODEX_ERROR_UNAUTHORIZED);
}

CodexBarProvider *codexbar_codex_fetch_with_adapters(const CodexBarProviderConfig *config,
                                                     const char *source,
                                                     CodexBarCodexTransport transport,
                                                     CodexBarCodexCLIFetcher cli_fetcher,
                                                     GCancellable *cancellable,
                                                     GError **error) {
    const char *selected = source ? source : "auto";
    if (g_str_equal(selected, "cli")) return cli_fetcher(error);
    if (g_str_equal(selected, "api")) return fetch_pat_usage(config, transport, cancellable, error);
    if (g_str_equal(selected, "oauth")) return fetch_http_usage(config, "oauth", transport, cancellable, error);
    if (g_str_equal(selected, "web")) return fetch_http_usage(config, "web", transport, cancellable, error);
    if (!g_str_equal(selected, "auto")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported Codex source: %s", selected);
        return NULL;
    }

    CodexBarProvider *provider = NULL;
    if (codexbar_codex_pat_is_available(config)) {
        GError *pat_error = NULL;
        provider = fetch_pat_usage(config, transport, cancellable, &pat_error);
        if (provider) return provider;
        if (!source_error_allows_fallback(pat_error)) {
            g_propagate_error(error, pat_error);
            return NULL;
        }
        g_clear_error(&pat_error);
    }

    GError *oauth_error = NULL;
    provider = fetch_http_usage(config, "oauth", transport, cancellable, &oauth_error);
    if (provider) return provider;
    if (!source_error_allows_fallback(oauth_error)) {
        g_propagate_error(error, oauth_error);
        return NULL;
    }
    g_clear_error(&oauth_error);
    return cli_fetcher(error);
}

static CodexBarProvider *default_cli_fetch(GError **error) {
    return codexbar_codex_fetch_with_home(NULL, error);
}

CodexBarProvider *codexbar_codex_fetch(const CodexBarProviderConfig *config,
                                       const char *source,
                                       GCancellable *cancellable,
                                       GError **error) {
    return codexbar_codex_fetch_with_adapters(
        config, source, codexbar_http_send, default_cli_fetch, cancellable, error);
}
