#include "backend.h"
#include "cli_cache.h"
#include "cli_cards.h"
#include "cli_config.h"
#include "cli_cost.h"
#include "cli_diagnose.h"
#include "cli_sessions.h"
#include "cli_serve.h"
#include "cli_usage.h"
#include "render.h"
#include "tui.h"
#include "version.h"

#include <glib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

typedef enum {
    CODEXBAR_TERMINAL_NOT_FOUND,
    CODEXBAR_TERMINAL_LAUNCHED,
    CODEXBAR_TERMINAL_FAILED,
} CodexBarTerminalLaunchResult;

typedef struct {
    const char *program;
    const char *const *arguments;
    size_t argument_count;
} CodexBarTerminalCommand;

static CodexBarTerminalLaunchResult try_terminal(
    const CodexBarTerminalCommand *command,
    const char *executable,
    GError **error
) {
    char *terminal = g_find_program_in_path(command->program);
    if (!terminal) return CODEXBAR_TERMINAL_NOT_FOUND;

    char **arguments = g_new0(char *, command->argument_count + 4);
    arguments[0] = terminal;
    for (size_t index = 0; index < command->argument_count; index++) {
        arguments[index + 1] = g_strdup(command->arguments[index]);
    }
    arguments[command->argument_count + 1] = g_strdup(executable);
    arguments[command->argument_count + 2] = g_strdup("tui");

    gboolean launched = g_spawn_async(
        NULL,
        arguments,
        NULL,
        G_SPAWN_STDOUT_TO_DEV_NULL | G_SPAWN_STDERR_TO_DEV_NULL,
        NULL,
        NULL,
        NULL,
        error
    );
    g_strfreev(arguments);
    return launched ? CODEXBAR_TERMINAL_LAUNCHED : CODEXBAR_TERMINAL_FAILED;
}

static char *resolve_executable(const char *program) {
    char *executable = g_file_read_link("/proc/self/exe", NULL);
    if (executable) return executable;
    if (strchr(program, G_DIR_SEPARATOR)) return g_canonicalize_filename(program, NULL);
    return g_find_program_in_path(program);
}

static int launch_tui_in_terminal(const char *program) {
    static const char *const xdg_arguments[] = {
        "--app-id=com.steipete.codexbar",
        "--title=CodexBar",
        "-e",
    };
    static const char *const ghostty_arguments[] = {
        "--class=com.steipete.codexbar",
        "--title=CodexBar",
        "-e",
    };
    static const char *const alacritty_arguments[] = {
        "--class",
        "com.steipete.codexbar",
        "--title",
        "CodexBar",
        "-e",
    };
    static const char *const kitty_arguments[] = {
        "--class",
        "com.steipete.codexbar",
        "--title",
        "CodexBar",
    };
    static const char *const foot_arguments[] = {
        "--app-id=com.steipete.codexbar",
        "--title=CodexBar",
    };
    static const char *const wezterm_arguments[] = {
        "start",
        "--class",
        "com.steipete.codexbar",
        "--",
    };
    static const char *const gnome_terminal_arguments[] = {
        "--title=CodexBar",
        "--",
    };
    static const char *const konsole_arguments[] = {
        "--name",
        "com.steipete.codexbar",
        "-p",
        "tabtitle=CodexBar",
        "-e",
    };
    static const char *const xterm_arguments[] = {
        "-class",
        "CodexBar",
        "-title",
        "CodexBar",
        "-e",
    };
    static const CodexBarTerminalCommand terminals[] = {
        {"xdg-terminal-exec", xdg_arguments, G_N_ELEMENTS(xdg_arguments)},
        {"ghostty", ghostty_arguments, G_N_ELEMENTS(ghostty_arguments)},
        {"alacritty", alacritty_arguments, G_N_ELEMENTS(alacritty_arguments)},
        {"kitty", kitty_arguments, G_N_ELEMENTS(kitty_arguments)},
        {"foot", foot_arguments, G_N_ELEMENTS(foot_arguments)},
        {"wezterm", wezterm_arguments, G_N_ELEMENTS(wezterm_arguments)},
        {"gnome-terminal", gnome_terminal_arguments, G_N_ELEMENTS(gnome_terminal_arguments)},
        {"konsole", konsole_arguments, G_N_ELEMENTS(konsole_arguments)},
        {"xterm", xterm_arguments, G_N_ELEMENTS(xterm_arguments)},
    };

    char *executable = resolve_executable(program);
    if (!executable) {
        fprintf(stderr, "Cannot resolve the codexbar-linux executable.\n");
        return 1;
    }

    GError *error = NULL;
    for (size_t index = 0; index < G_N_ELEMENTS(terminals); index++) {
        CodexBarTerminalLaunchResult result = try_terminal(&terminals[index], executable, &error);
        if (result == CODEXBAR_TERMINAL_LAUNCHED) {
            g_free(executable);
            return 0;
        }
        if (result == CODEXBAR_TERMINAL_FAILED) {
            fprintf(stderr, "Cannot open a terminal for CodexBar: %s\n", error->message);
            g_error_free(error);
            g_free(executable);
            return 1;
        }
    }

    fprintf(stderr, "Cannot open CodexBar: no supported terminal emulator was found.\n");
    g_free(executable);
    return 1;
}

static int print_usage(const char *program) {
    fprintf(stderr, "Usage: %s [usage] [--provider <name>] [--format text|json]\n", program);
    fprintf(stderr, "       %s cards [--provider <name|both|all>] [--brief]\n", program);
    fprintf(stderr, "       %s cost [--provider <codex|claude|both|all>] [--format text|json]\n", program);
    fprintf(stderr, "       %s sessions [list|focus] [--json]\n", program);
    fprintf(stderr, "       %s serve [--port <port>] [--refresh-interval <seconds>]\n", program);
    fprintf(stderr, "       %s cache clear <--cookies|--cost|--all> [--provider <name>]\n", program);
    fprintf(stderr, "       %s diagnose --provider <name|all> --format json\n", program);
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
    if (argc >= 2 && strcmp(argv[1], "cards") == 0) return codexbar_cli_cards_run(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "cost") == 0) return codexbar_cli_cost_run(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "sessions") == 0) return codexbar_cli_sessions_run(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "serve") == 0) return codexbar_cli_serve_run(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "cache") == 0) return codexbar_cli_cache_run(argc - 2, argv + 2);
    if (argc >= 2 && strcmp(argv[1], "diagnose") == 0) return codexbar_cli_diagnose_run(argc - 2, argv + 2);
    if (argc == 1) return codexbar_cli_usage_run(0, NULL);
    if (strcmp(argv[1], "usage") == 0) return codexbar_cli_usage_run(argc - 2, argv + 2);
    if (argv[1][0] == '-') return codexbar_cli_usage_run(argc - 1, argv + 1);
    if (argc != 2) return print_usage(argv[0]);

    if (strcmp(argv[1], "tui") == 0) {
        if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO)) return launch_tui_in_terminal(argv[0]);
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
