#include "tui.h"

#include "backend.h"
#include "version.h"

#include <locale.h>
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>

enum {
    COLOR_NORMAL = 1,
    COLOR_MUTED,
    COLOR_ACCENT,
    COLOR_ERROR,
    COLOR_BORDER,
    COLOR_SELECTED,
    COLOR_TRACK,
};

static void initialize_theme(void) {
    const char *theme = g_getenv("CODEXBAR_THEME");
    if (!theme || theme[0] == '\0') {
        theme = "mocha";
    }

    start_color();
    use_default_colors();
    if (g_str_equal(theme, "system")) {
        init_pair(COLOR_NORMAL, -1, -1);
        init_pair(COLOR_MUTED, COLOR_CYAN, -1);
        init_pair(COLOR_ACCENT, COLOR_GREEN, -1);
        init_pair(COLOR_ERROR, COLOR_RED, -1);
        init_pair(COLOR_BORDER, COLOR_BLUE, -1);
        init_pair(COLOR_SELECTED, COLOR_BLACK, COLOR_GREEN);
        init_pair(COLOR_TRACK, COLOR_BLUE, -1);
    } else if (g_str_equal(theme, "dark") || COLORS < 256) {
        init_pair(COLOR_NORMAL, COLOR_WHITE, COLOR_BLACK);
        init_pair(COLOR_MUTED, COLORS >= 16 ? 8 : COLOR_CYAN, COLOR_BLACK);
        init_pair(COLOR_ACCENT, COLOR_CYAN, COLOR_BLACK);
        init_pair(COLOR_ERROR, COLOR_RED, COLOR_BLACK);
        init_pair(COLOR_BORDER, COLOR_BLUE, COLOR_BLACK);
        init_pair(COLOR_SELECTED, COLOR_BLACK, COLOR_CYAN);
        init_pair(COLOR_TRACK, COLORS >= 16 ? 8 : COLOR_BLUE, COLOR_BLACK);
    } else {
        init_pair(COLOR_NORMAL, 189, -1);
        init_pair(COLOR_MUTED, 146, -1);
        init_pair(COLOR_ACCENT, 111, -1);
        init_pair(COLOR_ERROR, 211, -1);
        init_pair(COLOR_BORDER, 60, -1);
        init_pair(COLOR_SELECTED, 234, 111);
        init_pair(COLOR_TRACK, 60, -1);
    }
    bkgd(' ' | COLOR_PAIR(COLOR_NORMAL));
    attrset(COLOR_PAIR(COLOR_NORMAL));
}

static void draw_text(int y, int x, int max_width, const char *text) {
    if (text && max_width > 0) {
        mvaddnstr(y, x, text, max_width);
    }
}

static void draw_border(int y, int x, int height, int width) {
    attron(COLOR_PAIR(COLOR_BORDER));
    mvaddstr(y, x, "╭");
    mvaddstr(y, x + width - 1, "╮");
    mvaddstr(y + height - 1, x, "╰");
    mvaddstr(y + height - 1, x + width - 1, "╯");
    mvhline(y, x + 1, ACS_HLINE, width - 2);
    mvhline(y + height - 1, x + 1, ACS_HLINE, width - 2);
    mvvline(y + 1, x, ACS_VLINE, height - 2);
    mvvline(y + 1, x + width - 1, ACS_VLINE, height - 2);
    attroff(COLOR_PAIR(COLOR_BORDER));
}

static void draw_progress(int y, int x, int width, double used_percent) {
    int filled = (int)((used_percent / 100.0) * width + 0.5);
    for (int index = 0; index < width; index++) {
        attron(COLOR_PAIR(index < filled ? COLOR_ACCENT : COLOR_TRACK));
        mvaddch(y, x + index, index < filled ? ACS_CKBOARD : ACS_BULLET);
        attroff(COLOR_PAIR(index < filled ? COLOR_ACCENT : COLOR_TRACK));
    }
}

static int draw_rate_window(int y,
                            int x,
                            int width,
                            const char *name,
                            const CodexBarRateWindow *rate_window) {
    if (!rate_window->available) {
        return y;
    }

    const char *display_name = rate_window->label ? rate_window->label : name;
    attron(COLOR_PAIR(COLOR_MUTED));
    draw_text(y, x, MAX(1, width - 23), display_name);
    mvprintw(y,
             x + width - 22,
             "%3.0f%% used · %3.0f%% left",
             rate_window->used_percent,
             100.0 - rate_window->used_percent);
    attroff(COLOR_PAIR(COLOR_MUTED));
    y++;
    draw_progress(y++, x, width, rate_window->used_percent);
    if (rate_window->reset_description) {
        attron(COLOR_PAIR(COLOR_MUTED));
        draw_text(y++, x, width, rate_window->reset_description);
        attroff(COLOR_PAIR(COLOR_MUTED));
    }
    if (rate_window->resets_at) {
        attron(COLOR_PAIR(COLOR_MUTED));
        draw_text(y++, x, width, rate_window->resets_at);
        attroff(COLOR_PAIR(COLOR_MUTED));
    }
    return y + 1;
}

static int rate_window_height(const CodexBarRateWindow *window) {
    return window->available ? 3 + (window->reset_description ? 1 : 0) + (window->resets_at ? 1 : 0) : 0;
}

static int provider_content_height(const CodexBarProvider *provider) {
    return ((provider->source || provider->account || provider->plan) ? 2 : 0) +
           (provider->note ? 1 : 0) +
           rate_window_height(&provider->primary) +
           rate_window_height(&provider->secondary) +
           rate_window_height(&provider->tertiary) +
           (provider->has_credits ? 1 : 0) +
           (provider->error ? 1 : 0);
}

static void draw_provider(const CodexBarProvider *provider, int y, int x, int height, int width) {
    int bottom = y + height;
    if (provider->source || provider->account || provider->plan) {
        attron(COLOR_PAIR(COLOR_MUTED));
        GString *metadata = g_string_new(NULL);
        const char *values[] = {provider->account, provider->plan, provider->source};
        for (size_t index = 0; index < G_N_ELEMENTS(values); index++) {
            if (!values[index]) continue;
            if (metadata->len > 0) g_string_append(metadata, " · ");
            g_string_append(metadata, values[index]);
        }
        draw_text(y, x, width, metadata->str);
        g_string_free(metadata, TRUE);
        attroff(COLOR_PAIR(COLOR_MUTED));
        y += 2;
    }
    if (provider->note) {
        attron(COLOR_PAIR(COLOR_MUTED));
        draw_text(y++, x, width, provider->note);
        attroff(COLOR_PAIR(COLOR_MUTED));
    }

    y = draw_rate_window(y, x, width, "SESSION", &provider->primary);
    y = draw_rate_window(y, x, width, "WEEKLY", &provider->secondary);
    y = draw_rate_window(y, x, width, "EXTRA", &provider->tertiary);

    if (provider->has_credits && y < bottom) {
        attron(COLOR_PAIR(COLOR_MUTED));
        mvprintw(y++,
                 x,
                 "%s  %.2f left",
                 provider->credits_label ? provider->credits_label : "CREDITS",
                 provider->credits_remaining);
        attroff(COLOR_PAIR(COLOR_MUTED));
    }
    if (provider->error && y < bottom) {
        attron(COLOR_PAIR(COLOR_ERROR));
        draw_text(y, x, width, provider->error);
        attroff(COLOR_PAIR(COLOR_ERROR));
    }
}

static void draw_screen(const CodexBarSnapshot *snapshot, guint selected, const char *status) {
    erase();
    int rows = 0;
    int columns = 0;
    getmaxyx(stdscr, rows, columns);

    if (rows < 14 || columns < 48) {
        attron(COLOR_PAIR(COLOR_ERROR));
        mvprintw(0, 0, "CodexBar needs at least 48x14; current terminal is %dx%d", columns, rows);
        attroff(COLOR_PAIR(COLOR_ERROR));
        refresh();
        return;
    }

    int desired_height = 14;
    if (snapshot->providers->len > 0) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, selected);
        desired_height = provider_content_height(provider) + 7;
    }
    int panel_width = MIN(columns - 2, 72);
    int panel_height = MIN(rows - 2, CLAMP(desired_height, 14, 28));
    int panel_y = MAX((rows - panel_height) / 2, 0);
    int panel_x = MAX((columns - panel_width) / 2, 0);
    draw_border(panel_y, panel_x, panel_height, panel_width);

    int tab_x = panel_x + 3;
    for (guint index = 0; index < snapshot->providers->len; index++) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        int available = panel_x + panel_width - tab_x - 3;
        if (available <= 0) {
            break;
        }
        if (index == selected) {
            attron(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
        } else {
            attron(COLOR_PAIR(COLOR_MUTED));
        }
        mvaddnstr(panel_y + 1, tab_x, provider->provider, available);
        if (index == selected) {
            attroff(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
        } else {
            attroff(COLOR_PAIR(COLOR_MUTED));
        }
        tab_x += (int)strlen(provider->provider) + 3;
    }

    int card_y = panel_y + 3;
    int card_x = panel_x + 2;
    int card_height = panel_height - 5;
    int card_width = panel_width - 4;
    draw_border(card_y, card_x, card_height, card_width);
    if (snapshot->providers->len > 0) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, selected);
        draw_provider(provider, card_y + 1, card_x + 3, card_height - 2, card_width - 6);
    } else {
        attron(COLOR_PAIR(COLOR_MUTED));
        mvaddstr(card_y + 1, card_x + 3, "No enabled provider returned data.");
        attroff(COLOR_PAIR(COLOR_MUTED));
    }

    attron(COLOR_PAIR(COLOR_MUTED));
    draw_text(panel_y + panel_height - 2,
              panel_x + 3,
              panel_width - 31,
              status ? status : "h/l move  r refresh  ? help  q quit");
    char *version = g_strdup_printf("codexbar-linux %s", CODEXBAR_LINUX_VERSION);
    draw_text(panel_y + panel_height - 2,
              panel_x + panel_width - (int)strlen(version) - 3,
              (int)strlen(version),
              version);
    g_free(version);
    attroff(COLOR_PAIR(COLOR_MUTED));
    refresh();
}

static CodexBarSnapshot *fetch_snapshot(char **status) {
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_backend_fetch(&error);
    if (!snapshot) {
        *status = g_strdup_printf("backend error: %s", error->message);
        g_error_free(error);
        snapshot = g_new0(CodexBarSnapshot, 1);
        snapshot->providers = g_ptr_array_new();
    }
    return snapshot;
}

int codexbar_tui_run(void) {
    setlocale(LC_ALL, "");
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    keypad(stdscr, TRUE);
    if (has_colors()) {
        initialize_theme();
    }

    char *status = g_strdup("loading provider telemetry...");
    CodexBarSnapshot *empty = g_new0(CodexBarSnapshot, 1);
    empty->providers = g_ptr_array_new();
    draw_screen(empty, 0, status);
    codexbar_snapshot_free(empty);
    g_free(status);
    status = NULL;

    CodexBarSnapshot *snapshot = fetch_snapshot(&status);
    guint selected = 0;
    gboolean help_visible = FALSE;
    gboolean pending_z = FALSE;
    gboolean running = TRUE;
    while (running) {
        if (snapshot->providers->len > 0 && selected >= snapshot->providers->len) {
            selected = snapshot->providers->len - 1;
        }
        const char *visible_status = status;
        if (help_visible) {
            visible_status = "h/l move  g/G edge  1-9 select  r refresh  q/Esc/ZZ quit";
        }
        draw_screen(snapshot, selected, visible_status);
        int key = getch();
        if (pending_z) {
            pending_z = FALSE;
            if (key == 'Z') {
                running = FALSE;
                continue;
            }
        }
        switch (key) {
        case 'q':
        case 27:
            running = FALSE;
            break;
        case KEY_LEFT:
        case 'h':
        case '[':
        case KEY_BTAB:
            if (snapshot->providers->len > 0) {
                selected = selected == 0 ? snapshot->providers->len - 1 : selected - 1;
            }
            break;
        case KEY_RIGHT:
        case 'l':
        case ']':
        case '\t':
            if (snapshot->providers->len > 0) {
                selected = (selected + 1) % snapshot->providers->len;
            }
            break;
        case 'g':
            selected = 0;
            break;
        case 'G':
            if (snapshot->providers->len > 0) {
                selected = snapshot->providers->len - 1;
            }
            break;
        case '?':
            help_visible = !help_visible;
            break;
        case 'Z':
            pending_z = TRUE;
            break;
        case 'r':
        case 18: {
            g_free(status);
            status = g_strdup("refreshing provider telemetry...");
            draw_screen(snapshot, selected, status);
            codexbar_snapshot_free(snapshot);
            g_free(status);
            status = NULL;
            snapshot = fetch_snapshot(&status);
            break;
        }
        case KEY_RESIZE:
            break;
        default:
            if (key >= '1' && key <= '9') {
                guint requested = (guint)(key - '1');
                if (requested < snapshot->providers->len) {
                    selected = requested;
                }
            }
            break;
        }
    }

    codexbar_snapshot_free(snapshot);
    g_free(status);
    endwin();
    return 0;
}
