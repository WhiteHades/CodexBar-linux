#include "backend.h"
#include "cli_cache.h"
#include "cli_cards.h"
#include "cli_config.h"
#include "cli_cost.h"
#include "cli_diagnose.h"
#include "cli_sessions.h"
#include "cli_serve.h"
#include "cli_usage.h"
#include "desktop.h"
#include "render.h"
#include "tui.h"
#include "version.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

static int print_usage(const char *program) {
    fprintf(stderr, "Usage: %s [usage] [--provider <name>] [--format text|json]\n", program);
    fprintf(stderr, "       %s cards [--provider <name|both|all>] [--brief]\n", program);
    fprintf(stderr, "       %s cost [--provider <codex|claude|both|all>] [--format text|json]\n", program);
    fprintf(stderr, "       %s sessions [list|focus] [--json]\n", program);
    fprintf(stderr, "       %s serve [--port <port>] [--refresh-interval <seconds>]\n", program);
    fprintf(stderr, "       %s cache clear <--cookies|--cost|--all> [--provider <name>]\n", program);
    fprintf(stderr, "       %s diagnose --provider <name|all> --format json\n", program);
    fprintf(stderr, "       %s <waybar|tui|status-item>\n", program);
    fprintf(stderr, "       %s config <validate|dump|providers|enable|disable|set-api-key>\n", program);
    fprintf(stderr, "       %s --version\n", program);
    return 2;
}

int main(int argc, char **argv) {
    if (argc == 2 && (strcmp(argv[1], "--version") == 0 || strcmp(argv[1], "-V") == 0)) {
        printf("CodexBar %s\n", CODEXBAR_LINUX_VERSION);
        return 0;
    }
    if (argc == 2 && (strcmp(argv[1], "--help") == 0 || strcmp(argv[1], "-h") == 0)) {
        print_usage(argv[0]);
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "config") == 0) return codexbar_cli_config_run(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "cards") == 0) return codexbar_cli_cards_run(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "cost") == 0) return codexbar_cli_cost_run(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "sessions") == 0) return codexbar_cli_sessions_run(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "serve") == 0) return codexbar_cli_serve_run(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "cache") == 0) return codexbar_cli_cache_run(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "diagnose") == 0) return codexbar_cli_diagnose_run(argc - 2, argv + 2);
    if (argc == 2 && strcmp(argv[1], "status-item") == 0) return codexbar_status_item_run(argv[0]);
    if (argc == 1) return codexbar_cli_usage_run(0, NULL);
    if (strcmp(argv[1], "usage") == 0) return codexbar_cli_usage_run(argc - 2, argv + 2);
    if (argv[1][0] == '-') return codexbar_cli_usage_run(argc - 1, argv + 1);
    if (argc != 2) return print_usage(argv[0]);

    if (strcmp(argv[1], "tui") == 0) {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) {
            GError *error = NULL;
            if (codexbar_desktop_launch_tui(argv[0], &error)) return 0;
            fprintf(stderr, "Cannot open CodexBar: %s.\n", error->message);
            g_error_free(error);
            return 1;
        }
        return codexbar_tui_run();
    }
    if (strcmp(argv[1], "waybar") != 0) {
        return print_usage(argv[0]);
    }

    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_backend_fetch(&error);
    char *output = NULL;
    int status = 0;
    if (!snapshot) {
        output = codexbar_render_waybar_error(error->message);
        g_error_free(error);
        status = 1;
    } else {
        output = codexbar_render_waybar(snapshot);
        codexbar_snapshot_free(snapshot);
    }

    puts(output);
    g_free(output);
    return status;
}
