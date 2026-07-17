#include "codex.h"

#include "version.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

static GQuark codex_error_quark(void) {
    return g_quark_from_static_string("codexbar-codex-error");
}

static char *reset_description(json_object *window) {
    json_object *reset = NULL;
    if (!json_object_object_get_ex(window, "resetsAt", &reset)) {
        return NULL;
    }
    GDateTime *time = g_date_time_new_from_unix_local(json_object_get_int64(reset));
    if (!time) {
        return NULL;
    }
    char *description = g_date_time_format(time, "resets %a %H:%M");
    g_date_time_unref(time);
    return description;
}

static void parse_window(json_object *limits, const char *key, CodexBarRateWindow *window) {
    json_object *object = NULL;
    json_object *used = NULL;
    if (!json_object_object_get_ex(limits, key, &object) || !json_object_is_type(object, json_type_object) ||
        !json_object_object_get_ex(object, "usedPercent", &used)) {
        return;
    }
    window->available = TRUE;
    window->used_percent = CLAMP(json_object_get_double(used), 0.0, 100.0);
    window->reset_description = reset_description(object);
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
    CodexBarProvider *provider = g_new0(CodexBarProvider, 1);
    provider->provider = g_strdup("codex");
    provider->source = g_strdup("cli");
    parse_window(limits, "primary", &provider->primary);
    parse_window(limits, "secondary", &provider->secondary);
    json_object *credits = NULL;
    json_object *balance = NULL;
    if (json_object_object_get_ex(limits, "credits", &credits) &&
        json_object_object_get_ex(credits, "balance", &balance) && !json_object_is_type(balance, json_type_null)) {
        provider->has_credits = TRUE;
        provider->credits_remaining = json_object_get_double(balance);
    }
    json_object_put(root);
    return provider;
}

static gboolean write_message(GOutputStream *stream, const char *message, GError **error) {
    char *line = g_strdup_printf("%s\n", message);
    gsize written = 0;
    gboolean success = g_output_stream_write_all(stream, line, strlen(line), &written, NULL, error) &&
                       g_output_stream_flush(stream, NULL, error);
    g_free(line);
    return success;
}

static char *read_response(GDataInputStream *stream, int wanted_id, GError **error) {
    for (int lines = 0; lines < 64; lines++) {
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
    GDataInputStream *output = g_data_input_stream_new(g_subprocess_get_stdout_pipe(process));
    char *initialize = g_strdup_printf(
        "{\"id\":1,\"method\":\"initialize\",\"params\":{\"clientInfo\":{\"name\":\"codexbar-linux\",\"version\":\"%s\"}}}",
        CODEXBAR_LINUX_VERSION);
    gboolean sent = write_message(input, initialize, error);
    g_free(initialize);
    char *response = sent ? read_response(output, 1, error) : NULL;
    g_free(response);
    if (!response && error && *error) {
        g_subprocess_force_exit(process);
        g_object_unref(output);
        g_object_unref(process);
        return NULL;
    }
    sent = write_message(input, "{\"method\":\"initialized\",\"params\":{}}", error) &&
           write_message(input, "{\"id\":2,\"method\":\"account/rateLimits/read\",\"params\":{}}", error);
    response = sent ? read_response(output, 2, error) : NULL;
    CodexBarProvider *provider = response ? codexbar_codex_parse_rate_limits(response, error) : NULL;
    g_free(response);
    g_subprocess_force_exit(process);
    g_object_unref(output);
    g_object_unref(process);
    return provider;
}
