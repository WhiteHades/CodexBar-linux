#include "codex.h"

#include "http.h"
#include "version.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

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
    if (json_number_value(window_value, "limit_window_seconds", &seconds) && seconds > 0) {
        window->has_window_minutes = TRUE;
        window->window_minutes = (gint64)(seconds / 60.0);
    }
    double reset = 0;
    if (json_number_value(window_value, "reset_at", &reset) && reset >= 0 && reset <= G_MAXINT64 / 1000) {
        window->has_resets_at = TRUE;
        window->resets_at_ms = (gint64)reset * 1000;
    }
    codexbar_provider_add_quota_window(provider, window);
}

CodexBarProvider *codexbar_codex_parse_http_usage(const char *json,
                                                  const char *source,
                                                  gint64 updated_at_ms,
                                                  GError **error) {
    json_object *root = json_tokener_parse(json);
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
    json_object_put(root);
    if (provider->quota_windows->len == 0 && provider->balances->len == 0) {
        codexbar_provider_free(provider);
        g_set_error_literal(error, codex_error_quark(), CODEX_ERROR_MALFORMED,
                            "Codex usage API response has no usage data");
        return NULL;
    }
    return provider;
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
    if (length == 0 || length > 1024 * 1024) {
        g_free(contents);
        g_set_error_literal(error, codex_error_quark(), CODEX_ERROR_CREDENTIALS_INVALID,
                            "Codex auth.json is invalid");
        return FALSE;
    }
    json_object *root = json_tokener_parse(contents);
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
                          : "https://chatgpt.com/backend-api/wham/usage";
    CodexBarHttpRequestHeader headers[5] = {
        {g_str_equal(source, "oauth") ? "Authorization" : "Cookie",
         g_str_equal(source, "oauth") ? authorization : credential},
        {"Accept", "application/json"},
        {"User-Agent", "CodexBar"},
        {"ChatGPT-Account-Id", account_id},
        {"OpenAI-Beta", "codex-1"},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = account_id ? G_N_ELEMENTS(headers) : G_N_ELEMENTS(headers) - 2,
        .timeout_seconds = 30,
        .maximum_response_bytes = 1024 * 1024,
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
    CodexBarProvider *provider = codexbar_codex_parse_http_usage(
        response->body, source, g_get_real_time() / 1000, error);
    codexbar_http_response_free(response);
    return provider;
}

CodexBarProvider *codexbar_codex_fetch_with_adapters(const CodexBarProviderConfig *config,
                                                     const char *source,
                                                     CodexBarCodexTransport transport,
                                                     CodexBarCodexCLIFetcher cli_fetcher,
                                                     GCancellable *cancellable,
                                                     GError **error) {
    const char *selected = source ? source : "auto";
    if (g_str_equal(selected, "cli")) return cli_fetcher(error);
    if (g_str_equal(selected, "oauth")) return fetch_http_usage(config, "oauth", transport, cancellable, error);
    if (g_str_equal(selected, "web")) return fetch_http_usage(config, "web", transport, cancellable, error);
    if (!g_str_equal(selected, "auto")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported Codex source: %s", selected);
        return NULL;
    }
    GError *oauth_error = NULL;
    CodexBarProvider *provider = fetch_http_usage(config, "oauth", transport, cancellable, &oauth_error);
    if (provider) return provider;
    gboolean fallback = oauth_error && oauth_error->domain == codex_error_quark() &&
                        (oauth_error->code == CODEX_ERROR_CREDENTIALS_MISSING ||
                         oauth_error->code == CODEX_ERROR_UNAUTHORIZED);
    if (!fallback) {
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
