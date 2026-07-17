#include "codex.h"

#include "version.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

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
    window->used_percent = CLAMP(json_object_get_double(used), 0.0, 100.0);
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

CodexBarProvider *codexbar_codex_fetch(GError **error) {
    const char *binary = g_getenv("CODEX_CLI_PATH");
    if (!binary || binary[0] == '\0') binary = "codex";
    const char *argv[] = {binary, "-s", "read-only", "-a", "untrusted", "app-server", NULL};
    GSubprocess *process = g_subprocess_newv(
        argv, G_SUBPROCESS_FLAGS_STDIN_PIPE | G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_SILENCE, error);
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
