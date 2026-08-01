#include "cli_serve.h"

#include "serve.h"

#include <errno.h>
#include <glib.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

static gboolean parse_ipv4_host(const char *text) {
    char **parts = g_strsplit(text, ".", -1);
    gboolean valid = g_strv_length(parts) == 4;
    for (guint index = 0; valid && parts[index]; index++) {
        const char *part = parts[index];
        if (part[0] == '\0' || (part[0] == '0' && part[1] != '\0')) {
            valid = FALSE;
            break;
        }
        for (const char *cursor = part; *cursor; cursor++) {
            if (!g_ascii_isdigit(*cursor)) {
                valid = FALSE;
                break;
            }
        }
        if (!valid) break;
        char *end = NULL;
        guint64 value = g_ascii_strtoull(part, &end, 10);
        valid = *end == '\0' && value <= 255;
    }
    g_strfreev(parts);
    return valid;
}

static char *normalized_host(const char *raw) {
    char *host = g_strstrip(g_strdup(raw ? raw : ""));
    if (g_ascii_strcasecmp(host, "localhost") == 0) {
        g_free(host);
        return g_strdup("127.0.0.1");
    }
    if (host[0] == '\0' || !parse_ipv4_host(host)) g_clear_pointer(&host, g_free);
    return host;
}

static gboolean loopback_host(const char *host) {
    return g_str_has_prefix(host, "127.");
}

static char *normalized_token(const char *raw) {
    return raw ? g_strstrip(g_strdup(raw)) : NULL;
}

int codexbar_cli_serve_run(int argc, char **argv) {
    guint64 port = 8080;
    double refresh_interval = 60;
    double request_timeout = 30;
    const char *host_argument = "127.0.0.1";
    const char *token_argument = NULL;
    gboolean allow_plain_http = FALSE;
    for (int index = 0; index < argc; index++) {
        const char *argument = argv[index];
        if (g_str_equal(argument, "--host")) {
            host_argument = option_value(argc, argv, &index);
            if (!host_argument) {
                fputs("Error: Missing value for --host.\n", stderr);
                return 1;
            }
        } else if (g_str_equal(argument, "--port")) {
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
        } else if (g_str_equal(argument, "--dashboard-token")) {
            token_argument = option_value(argc, argv, &index);
            if (!token_argument) {
                fputs("Error: Missing value for --dashboard-token.\n", stderr);
                return 1;
            }
        } else if (g_str_equal(argument, "--allow-plain-http")) {
            allow_plain_http = TRUE;
        } else if (g_str_equal(argument, "--json-output") || g_str_equal(argument, "--verbose") ||
                   g_str_equal(argument, "-v")) {
            continue;
        } else if (g_str_equal(argument, "--help") || g_str_equal(argument, "-h")) {
            puts("Usage: codexbar-linux serve [--host <localhost|IPv4>] [--port <1..65535>]\n"
                 "                            [--refresh-interval <seconds>] [--request-timeout <seconds>]\n"
                 "                            [--dashboard-token <token>] [--allow-plain-http]\n"
                 "\n"
                 "GET /dashboard/v1/snapshot requires a bearer token. Prefer\n"
                 "CODEXBAR_DASHBOARD_TOKEN; non-loopback hosts also require\n"
                 "--allow-plain-http because credentials cross the network in cleartext.");
            return 0;
        } else {
            fprintf(stderr, "Error: Unknown argument: %s\n", argument);
            return 1;
        }
    }
    char *host = normalized_host(host_argument);
    if (!host) {
        fputs("Error: --host must be 'localhost' or an IPv4 address.\n", stderr);
        return 1;
    }
    const char *environment_token = g_getenv("CODEXBAR_DASHBOARD_TOKEN");
    const char *raw_token = environment_token ? environment_token : token_argument;
    const char *token_source = environment_token ? "CODEXBAR_DASHBOARD_TOKEN" : "--dashboard-token";
    char *dashboard_token = normalized_token(raw_token);
    if (raw_token && dashboard_token[0] == '\0') {
        fprintf(stderr, "Error: %s must not be empty or whitespace.\n", token_source);
        g_free(dashboard_token);
        g_free(host);
        return 1;
    }
    if (!loopback_host(host) && !dashboard_token) {
        fprintf(stderr,
                "Error: --dashboard-token (or CODEXBAR_DASHBOARD_TOKEN) is required for non-loopback --host '%s'.\n",
                host);
        g_free(host);
        return 1;
    }
    if (!loopback_host(host) && !allow_plain_http) {
        fprintf(stderr,
                "Error: Refusing to serve the dashboard token over cleartext HTTP on non-loopback --host '%s'. "
                "Pass --allow-plain-http to accept that the bearer token crosses the network unencrypted on every "
                "request.\n",
                host);
        g_free(dashboard_token);
        g_free(host);
        return 1;
    }
    int status = codexbar_serve_run(host,
                                    (unsigned int)port,
                                    refresh_interval,
                                    request_timeout,
                                    dashboard_token);
    g_free(dashboard_token);
    g_free(host);
    return status;
}
