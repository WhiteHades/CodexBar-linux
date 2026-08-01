#include "cli_guard.h"

#include "model.h"
#include "process.h"
#include "provider_registry.h"

#include <json-c/json.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    GUARD_SESSION,
    GUARD_WEEKLY,
} GuardWindow;

typedef enum {
    GUARD_OK,
    GUARD_BLOCKED,
    GUARD_UNKNOWN,
} GuardDecision;

typedef struct {
    const CodexBarProviderDescriptor *provider;
    GuardWindow window;
    double minimum_remaining;
    double timeout_seconds;
    gboolean fail_open;
    gboolean json;
    gboolean pretty;
} GuardOptions;

static gboolean parse_number(const char *raw, double minimum, double maximum, double *value) {
    if (!raw || raw[0] == '\0') return FALSE;
    char *end = NULL;
    double parsed = g_ascii_strtod(raw, &end);
    if (!end || end == raw || *end != '\0' || !isfinite(parsed) || parsed < minimum || parsed > maximum) {
        return FALSE;
    }
    *value = parsed;
    return TRUE;
}

static int argument_error(const char *message) {
    fprintf(stderr, "Error: %s\n", message);
    return 64;
}

static gboolean option_value(int argc, char **argv, int *index, const char **value, const char **error) {
    if (*index + 1 >= argc || argv[*index + 1][0] == '-') {
        *error = argv[*index];
        return FALSE;
    }
    *value = argv[++*index];
    return TRUE;
}

static int parse_options(int argc, char **argv, GuardOptions *options) {
    const char *provider = NULL;
    options->window = GUARD_SESSION;
    options->minimum_remaining = 10.0;
    options->timeout_seconds = 60.0;
    for (int index = 0; index < argc; index++) {
        const char *argument = argv[index];
        const char *value = NULL;
        const char *missing = NULL;
        if (g_str_equal(argument, "--provider")) {
            if (!option_value(argc, argv, &index, &value, &missing)) {
                char *message = g_strdup_printf("Missing value for %s.", missing);
                int result = argument_error(message);
                g_free(message);
                return result;
            }
            provider = value;
        } else if (g_str_equal(argument, "--window")) {
            if (!option_value(argc, argv, &index, &value, &missing)) {
                char *message = g_strdup_printf("Missing value for %s.", missing);
                int result = argument_error(message);
                g_free(message);
                return result;
            }
            if (g_ascii_strcasecmp(value, "session") == 0) {
                options->window = GUARD_SESSION;
            } else if (g_ascii_strcasecmp(value, "weekly") == 0) {
                options->window = GUARD_WEEKLY;
            } else {
                return argument_error("--window must be session|weekly.");
            }
        } else if (g_str_equal(argument, "--min-remaining")) {
            if (!option_value(argc, argv, &index, &value, &missing)) {
                char *message = g_strdup_printf("Missing value for %s.", missing);
                int result = argument_error(message);
                g_free(message);
                return result;
            }
            if (!parse_number(value, 0.0, 100.0, &options->minimum_remaining)) {
                return argument_error("--min-remaining must be a finite percent between 0 and 100.");
            }
        } else if (g_str_equal(argument, "--timeout")) {
            if (!option_value(argc, argv, &index, &value, &missing)) {
                char *message = g_strdup_printf("Missing value for %s.", missing);
                int result = argument_error(message);
                g_free(message);
                return result;
            }
            if (!parse_number(value, 0.0, 86400.0, &options->timeout_seconds)) {
                return argument_error("--timeout must be a finite number of seconds from 0 through 86400.");
            }
        } else if (g_str_equal(argument, "--fail-open")) {
            options->fail_open = TRUE;
        } else if (g_str_equal(argument, "--json")) {
            options->json = TRUE;
        } else if (g_str_equal(argument, "--pretty")) {
            options->pretty = TRUE;
        } else {
            char *message = g_strdup_printf("Unknown argument: %s", argument);
            int result = argument_error(message);
            g_free(message);
            return result;
        }
    }
    if (!provider) return argument_error("guard requires --provider <id>.");
    char *normalized = g_ascii_strdown(provider, -1);
    gboolean multiple = g_str_equal(normalized, "all") || g_str_equal(normalized, "both");
    if (multiple) {
        g_free(normalized);
        return argument_error("guard requires exactly one --provider.");
    }
    options->provider = codexbar_provider_registry_find(normalized);
    g_free(normalized);
    if (!options->provider) {
        char *message = g_strdup_printf("unknown provider '%s'.", provider);
        int result = argument_error(message);
        g_free(message);
        return result;
    }
    return 0;
}

static CodexBarProvider *fetch_provider(const char *program,
                                        const CodexBarProviderDescriptor *descriptor,
                                        double timeout_seconds,
                                        const char **reason) {
    const char *arguments[] = {
        program, "usage", "--provider", descriptor->cli_name, "--format", "json", NULL,
    };
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = (guint)(timeout_seconds * 1000.0),
        .termination_grace_milliseconds = 400,
        .maximum_output_bytes = 4U * 1024U * 1024U,
        .new_session = TRUE,
    };
    GError *error = NULL;
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, &error);
    if (!result) {
        *reason = error && error->domain == G_IO_ERROR && error->code == G_IO_ERROR_TIMED_OUT ? "timeout"
                                                                                              : "fetch-failed";
        g_clear_error(&error);
        return NULL;
    }
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(result->standard_output, NULL);
    codexbar_process_result_free(result);
    if (!snapshot) {
        *reason = "fetch-failed";
        return NULL;
    }
    CodexBarProvider *provider = NULL;
    for (guint index = 0; index < snapshot->providers->len; index++) {
        CodexBarProvider *candidate = g_ptr_array_index(snapshot->providers, index);
        if (g_str_equal(candidate->provider, descriptor->id)) {
            provider = g_ptr_array_steal_index(snapshot->providers, index);
            break;
        }
    }
    codexbar_snapshot_free(snapshot);
    if (!provider || provider->error) {
        codexbar_provider_free(provider);
        *reason = "fetch-failed";
        return NULL;
    }
    return provider;
}

static char *percent_string(double value) {
    double rounded = round(value);
    return fabs(value - rounded) < 0.05 ? g_strdup_printf("%.0f%%", rounded) : g_strdup_printf("%.1f%%", value);
}

static void emit_result(const GuardOptions *options,
                        GuardDecision decision,
                        gboolean has_remaining,
                        double remaining,
                        int exit_code,
                        const char *reason) {
    const char *window = options->window == GUARD_SESSION ? "session" : "weekly";
    const char *decision_text = decision == GUARD_OK ? "ok" : decision == GUARD_BLOCKED ? "blocked" : "unknown";
    if (options->json) {
        json_object *payload = json_object_new_object();
        json_object_object_add(payload, "provider", json_object_new_string(options->provider->id));
        json_object_object_add(payload, "window", json_object_new_string(window));
        json_object_object_add(payload,
                               "remainingPercent",
                               has_remaining ? json_object_new_double(remaining) : NULL);
        json_object_object_add(payload,
                               "minimumRemainingPercent",
                               json_object_new_double(options->minimum_remaining));
        json_object_object_add(payload, "decision", json_object_new_string(decision_text));
        json_object_object_add(payload, "exitCode", json_object_new_int(exit_code));
        json_object_object_add(payload, "unavailableReason", reason ? json_object_new_string(reason) : NULL);
        puts(json_object_to_json_string_ext(
            payload, options->pretty ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(payload);
        return;
    }
    char *minimum = percent_string(options->minimum_remaining);
    char *remaining_text = has_remaining ? percent_string(remaining) : g_strdup("unknown");
    const char *verdict = decision == GUARD_OK ? "OK" : decision == GUARD_BLOCKED ? "BLOCKED" : "UNKNOWN";
    printf("%s %s: %s%s — %s (minimum %s%s%s)\n",
           options->provider->id,
           window,
           remaining_text,
           has_remaining ? " remaining" : "",
           verdict,
           minimum,
           reason ? "; " : "",
           reason ? reason : "");
    g_free(remaining_text);
    g_free(minimum);
}

int codexbar_cli_guard_run(const char *program, int argc, char **argv) {
    GuardOptions options = {0};
    int parse_status = parse_options(argc, argv, &options);
    if (parse_status != 0) return parse_status;
    const char *reason = NULL;
    CodexBarProvider *provider = fetch_provider(program, options.provider, options.timeout_seconds, &reason);
    CodexBarQuotaWindow *window = provider ? codexbar_provider_quota_window(
                                                provider, options.window == GUARD_SESSION ? 0 : 1)
                                          : NULL;
    gboolean available = window && window->usage_known;
    double remaining = available ? 100.0 - window->used_percent : 0.0;
    if (!available && !reason) reason = "window-unavailable";
    GuardDecision decision = !available ? GUARD_UNKNOWN
                             : remaining >= options.minimum_remaining ? GUARD_OK
                                                                      : GUARD_BLOCKED;
    int exit_code = decision == GUARD_OK ? 0 : decision == GUARD_BLOCKED ? 1 : options.fail_open ? 0 : 69;
    emit_result(&options, decision, available, remaining, exit_code, reason);
    codexbar_provider_free(provider);
    return exit_code;
}
