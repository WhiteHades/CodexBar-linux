#include "backend.h"
#include "cli_config.h"
#include "cli_usage.h"
#include "render.h"
#include "tui.h"
#include "version.h"

#include <stdio.h>
#include <string.h>

static int print_usage(const char *program) {
    fprintf(stderr, "Usage: %s [usage] [--provider <name>] [--format text|json]\n", program);
    fprintf(stderr, "       %s <waybar|tui>\n", program);
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
    if (argc == 1) return codexbar_cli_usage_run(0, NULL);
    if (strcmp(argv[1], "usage") == 0) return codexbar_cli_usage_run(argc - 2, argv + 2);
    if (argv[1][0] == '-') return codexbar_cli_usage_run(argc - 1, argv + 1);
    if (argc != 2) return print_usage(argv[0]);

    if (strcmp(argv[1], "tui") == 0) {
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
