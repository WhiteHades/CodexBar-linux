#include "cli_serve.h"

#include "serve.h"

#include <errno.h>
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const char *option_value(int argc, char **argv, int *index) {
    if (*index + 1 >= argc || argv[*index + 1][0] == '-') return NULL;
    (*index)++;
    return argv[*index];
}

static gboolean parse_unsigned(const char *text, guint64 minimum, guint64 maximum, guint64 *value) {
    if (!text || text[0] == '\0') return FALSE;
    errno = 0;
    char *end = NULL;
    guint64 parsed = g_ascii_strtoull(text, &end, 10);
    if (errno != 0 || *end != '\0' || parsed < minimum || parsed > maximum) return FALSE;
    *value = parsed;
    return TRUE;
}

static gboolean parse_seconds(const char *text, double *value) {
    if (!text || text[0] == '\0') return FALSE;
    errno = 0;
    char *end = NULL;
    double parsed = g_ascii_strtod(text, &end);
    if (errno != 0 || *end != '\0' || !isfinite(parsed) || parsed < 0 || parsed > 86400) return FALSE;
    *value = parsed;
    return TRUE;
}

int codexbar_cli_serve_run(int argc, char **argv) {
    guint64 port = 8080;
    double refresh_interval = 60;
    double request_timeout = 30;
    for (int index = 0; index < argc; index++) {
        const char *argument = argv[index];
        if (g_str_equal(argument, "--port")) {
            const char *value = option_value(argc, argv, &index);
            if (!parse_unsigned(value, 1, 65535, &port)) {
                fputs("Error: --port must be between 1 and 65535.\n", stderr);
                return 1;
            }
        } else if (g_str_equal(argument, "--refresh-interval")) {
            const char *value = option_value(argc, argv, &index);
            if (!parse_seconds(value, &refresh_interval)) {
                fputs("Error: --refresh-interval must be zero or greater and no more than 86400.\n", stderr);
                return 1;
            }
        } else if (g_str_equal(argument, "--request-timeout")) {
            const char *value = option_value(argc, argv, &index);
            if (!parse_seconds(value, &request_timeout)) {
                fputs("Error: --request-timeout must be zero or greater and no more than 86400.\n", stderr);
                return 1;
            }
        } else if (g_str_equal(argument, "--log-level")) {
            if (!option_value(argc, argv, &index)) {
                fputs("Error: Missing value for --log-level.\n", stderr);
                return 1;
            }
        } else if (g_str_equal(argument, "--json-output") || g_str_equal(argument, "--verbose") ||
                   g_str_equal(argument, "-v")) {
            continue;
        } else if (g_str_equal(argument, "--help") || g_str_equal(argument, "-h")) {
            puts("Usage: codexbar-linux serve [--port <1..65535>] [--refresh-interval <seconds>]\n"
                 "                            [--request-timeout <seconds>]");
            return 0;
        } else {
            fprintf(stderr, "Error: Unknown argument: %s\n", argument);
            return 1;
        }
    }
    return codexbar_serve_run((unsigned int)port, refresh_interval, request_timeout);
}
