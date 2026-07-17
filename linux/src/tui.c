#include "tui.h"

#include "backend.h"
#include "config.h"
#include "tui_actions.h"
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

typedef enum {
    CODEXBAR_TUI_MODE_USAGE,
    CODEXBAR_TUI_MODE_ACTIONS,
    CODEXBAR_TUI_MODE_ABOUT,
} CodexBarTuiMode;

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

static char *format_timestamp(gint64 timestamp_ms, const char *prefix) {
    GDateTime *utc = g_date_time_new_from_unix_utc(timestamp_ms / 1000);
    GDateTime *local = utc ? g_date_time_to_local(utc) : NULL;
    char *date = local ? g_date_time_format(local, "%a, %b %d %Y at %H:%M") : NULL;
    char *result = date ? g_strdup_printf("%s %s", prefix, date) : NULL;
    g_free(date);
    if (local) g_date_time_unref(local);
    if (utc) g_date_time_unref(utc);
    return result;
}

static char *format_freshness(gint64 timestamp_ms) {
    gint64 delta_seconds = (g_get_real_time() / 1000 - timestamp_ms) / 1000;
    if (delta_seconds >= -60 && delta_seconds < 60) return g_strdup("updated just now");
    if (delta_seconds < 0) return format_timestamp(timestamp_ms, "updated");
    if (delta_seconds < 3600) return g_strdup_printf("updated %" G_GINT64_FORMAT "m ago", delta_seconds / 60);
    if (delta_seconds < 86400) return g_strdup_printf("updated %" G_GINT64_FORMAT "h ago", delta_seconds / 3600);
    return format_timestamp(timestamp_ms, "updated");
}

static char *format_count(gint64 value) {
    if (value >= 1000000000) return g_strdup_printf("%.2fB", value / 1000000000.0);
    if (value >= 1000000) return g_strdup_printf("%.2fM", value / 1000000.0);
    if (value >= 1000) return g_strdup_printf("%.0fK", value / 1000.0);
    return g_strdup_printf("%" G_GINT64_FORMAT, value);
}

static char *format_money(double value, const char *currency) {
    return g_ascii_strcasecmp(currency, "USD") == 0 ? g_strdup_printf("$%.2f", value)
                                                     : g_strdup_printf("%.2f %s", value, currency);
}

static const char *status_label(CodexBarServiceStatusIndicator indicator) {
    switch (indicator) {
    case CODEXBAR_STATUS_NONE:
        return "Operational";
    case CODEXBAR_STATUS_MINOR:
        return "Partial outage";
    case CODEXBAR_STATUS_MAJOR:
        return "Major outage";
    case CODEXBAR_STATUS_CRITICAL:
        return "Critical issue";
    case CODEXBAR_STATUS_MAINTENANCE:
        return "Maintenance";
    case CODEXBAR_STATUS_UNKNOWN:
        return "Status unknown";
    }
    return "Status unknown";
}

static int draw_rate_window(int y,
                            int x,
                            int width,
                            const CodexBarQuotaWindow *rate_window) {
    attron(COLOR_PAIR(COLOR_MUTED));
    draw_text(y, x, MAX(1, width - 23), rate_window->title);
    if (rate_window->usage_known) {
        mvprintw(y,
                 x + width - 22,
                 "%3.0f%% used · %3.0f%% left",
                 rate_window->used_percent,
                 100.0 - rate_window->used_percent);
    }
    attroff(COLOR_PAIR(COLOR_MUTED));
    y++;
    if (rate_window->usage_known) draw_progress(y++, x, width, rate_window->used_percent);
    if (rate_window->detail) {
        attron(COLOR_PAIR(COLOR_MUTED));
        draw_text(y++, x, width, rate_window->detail);
        attroff(COLOR_PAIR(COLOR_MUTED));
    }
    if (rate_window->reset_description) {
        attron(COLOR_PAIR(COLOR_MUTED));
        draw_text(y++, x, width, rate_window->reset_description);
        attroff(COLOR_PAIR(COLOR_MUTED));
    }
    if (rate_window->has_resets_at) {
        char *reset = format_timestamp(rate_window->resets_at_ms, "resets");
        attron(COLOR_PAIR(COLOR_MUTED));
        draw_text(y++, x, width, reset);
        attroff(COLOR_PAIR(COLOR_MUTED));
        g_free(reset);
    }
    if (rate_window->pace && rate_window->pace->summary) {
        char *summary = g_strdup_printf("Pace: %s", rate_window->pace->summary);
        attron(COLOR_PAIR(COLOR_MUTED));
        draw_text(y++, x, width, summary);
        attroff(COLOR_PAIR(COLOR_MUTED));
        g_free(summary);
    }
    return y + 1;
}

static int rate_window_height(const CodexBarQuotaWindow *window) {
    return 2 + (window->usage_known ? 1 : 0) + (window->detail ? 1 : 0) +
           (window->reset_description ? 1 : 0) + (window->has_resets_at ? 1 : 0) +
           ((window->pace && window->pace->summary) ? 1 : 0);
}

static int balance_height(const CodexBarBalance *balance) {
    return 1 + ((balance->has_used || balance->has_limit) ? 1 : 0) + (balance->has_expiry ? 1 : 0) +
           (balance->has_resets_at ? 1 : 0);
}

static int draw_balance(int y, int x, int width, const CodexBarBalance *balance) {
    attron(COLOR_PAIR(COLOR_MUTED));
    GString *summary = g_string_new(NULL);
    g_string_append_printf(summary, "%s  %.2f %s left", balance->title, balance->remaining, balance->unit);
    draw_text(y++, x, width, summary->str);
    g_string_free(summary, TRUE);
    if (balance->has_used || balance->has_limit) {
        GString *detail = g_string_new(NULL);
        if (balance->has_used) g_string_append_printf(detail, "%.2f used", balance->used);
        if (balance->has_used && balance->has_limit) g_string_append(detail, " · ");
        if (balance->has_limit) g_string_append_printf(detail, "%.2f limit", balance->limit);
        draw_text(y++, x, width, detail->str);
        g_string_free(detail, TRUE);
    }
    if (balance->has_expiry) {
        char *expiry = format_timestamp(balance->expiry_ms, "expires");
        draw_text(y++, x, width, expiry);
        g_free(expiry);
    }
    if (balance->has_resets_at) {
        char *reset = format_timestamp(balance->resets_at_ms, "resets");
        draw_text(y++, x, width, reset);
        g_free(reset);
    }
    attroff(COLOR_PAIR(COLOR_MUTED));
    return y;
}

static int provider_cost_height(const CodexBarProviderCost *cost) {
    return 2 + (cost->has_personal_used ? 1 : 0) + (cost->has_next_regen ? 1 : 0) +
           (cost->has_resets_at ? 1 : 0);
}

static int draw_provider_cost(int y, int x, int width, const CodexBarProviderCost *cost) {
    attron(COLOR_PAIR(COLOR_MUTED));
    GString *summary = g_string_new(NULL);
    char *used = format_money(cost->used, cost->currency);
    char *limit = format_money(cost->limit, cost->currency);
    g_string_append_printf(summary, "%s  %s", cost->limit > 0 ? "Extra usage" : "API spend", used);
    if (cost->limit > 0) g_string_append_printf(summary, " / %s", limit);
    g_free(used);
    g_free(limit);
    if (cost->period) g_string_append_printf(summary, " · %s", cost->period);
    draw_text(y++, x, width, summary->str);
    g_string_free(summary, TRUE);
    if (cost->has_personal_used) {
        char *money = format_money(cost->personal_used, cost->currency);
        char *personal = g_strdup_printf("%s personal used", money);
        g_free(money);
        draw_text(y++, x, width, personal);
        g_free(personal);
    }
    if (cost->has_next_regen) {
        char *amount = format_money(cost->next_regen, cost->currency);
        char *regen = g_strdup_printf("%s next regeneration", amount);
        g_free(amount);
        draw_text(y++, x, width, regen);
        g_free(regen);
    }
    if (cost->has_resets_at) {
        char *reset = format_timestamp(cost->resets_at_ms, "resets");
        draw_text(y++, x, width, reset);
        g_free(reset);
    }
    attroff(COLOR_PAIR(COLOR_MUTED));
    return y + 1;
}

static int token_cost_height(const CodexBarTokenCost *cost) {
    gboolean has_today = cost->has_today_tokens || cost->has_today_cost || cost->has_today_requests;
    gboolean has_history = cost->has_last_days_tokens || cost->has_last_days_cost || cost->has_last_days_requests;
    return 2 + (has_today ? 1 : 0) + (has_history ? 1 : 0);
}

static char *token_cost_line(const char *label,
                             gboolean has_tokens,
                             gint64 tokens,
                             gboolean has_cost,
                             double cost,
                             gboolean has_requests,
                             gint64 requests,
                             const char *currency) {
    GString *line = g_string_new(label);
    g_string_append(line, "  ");
    gboolean has_value = FALSE;
    if (has_cost) {
        char *money = format_money(cost, currency);
        g_string_append(line, money);
        g_free(money);
        has_value = TRUE;
    }
    if (has_tokens) {
        char *count = format_count(tokens);
        g_string_append_printf(line, "%s%s tokens", has_value ? " · " : "", count);
        g_free(count);
        has_value = TRUE;
    }
    if (has_requests) {
        g_string_append_printf(line, "%s%" G_GINT64_FORMAT " requests", has_value ? " · " : "", requests);
    }
    return g_string_free(line, FALSE);
}

static int draw_token_cost(int y, int x, int width, const CodexBarTokenCost *cost) {
    attron(COLOR_PAIR(COLOR_MUTED));
    draw_text(y++, x, width, "Cost");
    if (cost->has_today_tokens || cost->has_today_cost || cost->has_today_requests) {
        char *line = token_cost_line("Today",
                                     cost->has_today_tokens,
                                     cost->today_tokens,
                                     cost->has_today_cost,
                                     cost->today_cost,
                                     cost->has_today_requests,
                                     cost->today_requests,
                                     cost->currency);
        draw_text(y++, x, width, line);
        g_free(line);
    }
    if (cost->has_last_days_tokens || cost->has_last_days_cost || cost->has_last_days_requests) {
        char *fallback = cost->has_history_days
                             ? g_strdup_printf("Last %" G_GINT64_FORMAT " days", cost->history_days)
                             : g_strdup("Last 30 days");
        char *line = token_cost_line(cost->history_label ? cost->history_label : fallback,
                                     cost->has_last_days_tokens,
                                     cost->last_days_tokens,
                                     cost->has_last_days_cost,
                                     cost->last_days_cost,
                                     cost->has_last_days_requests,
                                     cost->last_days_requests,
                                     cost->currency);
        draw_text(y++, x, width, line);
        g_free(line);
        g_free(fallback);
    }
    attroff(COLOR_PAIR(COLOR_MUTED));
    return y + 1;
}

static int service_status_height(const CodexBarServiceStatus *status) {
    return 3 + (status->description ? 1 : 0) + (status->has_updated_at ? 1 : 0);
}

static int draw_service_status(int y, int x, int width, const CodexBarServiceStatus *status) {
    attron(COLOR_PAIR(COLOR_ERROR));
    char *summary = g_strdup_printf("Service status  %s", status_label(status->indicator));
    draw_text(y++, x, width, summary);
    g_free(summary);
    if (status->description) draw_text(y++, x, width, status->description);
    draw_text(y++, x, width, status->url);
    if (status->has_updated_at) {
        char *updated = format_freshness(status->updated_at_ms);
        draw_text(y++, x, width, updated);
        g_free(updated);
    }
    attroff(COLOR_PAIR(COLOR_ERROR));
    return y + 1;
}

static int provider_overview_height(const CodexBarProvider *provider) {
    int metadata_lines = (provider->source || provider->account || provider->plan) ? 1 : 0;
    if (provider->identity &&
        (provider->identity->organization || provider->identity->account_id ||
         (provider->identity->login_method &&
          (!provider->plan || g_strcmp0(provider->plan, provider->identity->login_method) != 0)))) {
        metadata_lines++;
    }
    metadata_lines += (provider->has_updated_at ? 1 : 0) + (provider->has_subscription_expires_at ? 1 : 0) +
                      (provider->has_subscription_renews_at ? 1 : 0);
    metadata_lines += provider->note ? 1 : 0;
    return metadata_lines > 0 ? metadata_lines + 1 : 0;
}

static int draw_provider_overview(int y, int x, int width, const CodexBarProvider *provider) {
    attron(COLOR_PAIR(COLOR_MUTED));
    if (provider->source || provider->account || provider->plan) {
        GString *metadata = g_string_new(NULL);
        const char *values[] = {provider->account, provider->plan, provider->source};
        for (size_t index = 0; index < G_N_ELEMENTS(values); index++) {
            if (!values[index]) continue;
            if (metadata->len > 0) g_string_append(metadata, " · ");
            g_string_append(metadata, values[index]);
        }
        draw_text(y++, x, width, metadata->str);
        g_string_free(metadata, TRUE);
    }
    if (provider->identity &&
        (provider->identity->organization || provider->identity->account_id ||
         (provider->identity->login_method &&
          (!provider->plan || g_strcmp0(provider->plan, provider->identity->login_method) != 0)))) {
        const char *values[] = {
            provider->identity->organization,
            provider->identity->account_id,
            provider->plan && g_strcmp0(provider->plan, provider->identity->login_method) == 0
                ? NULL
                : provider->identity->login_method,
        };
        GString *identity = g_string_new(NULL);
        for (size_t index = 0; index < G_N_ELEMENTS(values); index++) {
            if (!values[index]) continue;
            if (identity->len > 0) g_string_append(identity, " · ");
            g_string_append(identity, values[index]);
        }
        draw_text(y++, x, width, identity->str);
        g_string_free(identity, TRUE);
    }
    if (provider->has_updated_at) {
        char *updated = format_freshness(provider->updated_at_ms);
        draw_text(y++, x, width, updated);
        g_free(updated);
    }
    if (provider->has_subscription_expires_at) {
        char *expires = format_timestamp(provider->subscription_expires_at_ms, "subscription expires");
        draw_text(y++, x, width, expires);
        g_free(expires);
    }
    if (provider->has_subscription_renews_at) {
        char *renews = format_timestamp(provider->subscription_renews_at_ms, "subscription renews");
        draw_text(y++, x, width, renews);
        g_free(renews);
    }
    if (provider->note) draw_text(y++, x, width, provider->note);
    attroff(COLOR_PAIR(COLOR_MUTED));
    return y + 1;
}

static int provider_content_height(const CodexBarProvider *provider) {
    int height = provider_overview_height(provider) + (provider->error ? 1 : 0);
    for (guint index = 0; index < provider->quota_windows->len; index++) {
        height += rate_window_height(codexbar_provider_quota_window(provider, index));
    }
    for (guint index = 0; index < provider->balances->len; index++) {
        height += balance_height(codexbar_provider_balance(provider, index));
    }
    if (provider->provider_cost) height += provider_cost_height(provider->provider_cost);
    if (provider->token_cost) height += token_cost_height(provider->token_cost);
    if (provider->status && provider->status->indicator != CODEXBAR_STATUS_NONE) {
        height += service_status_height(provider->status);
    }
    return height;
}

static guint provider_metric_count(const CodexBarProvider *provider) {
    return (provider_overview_height(provider) > 0 ? 1 : 0) + provider->quota_windows->len +
           provider->balances->len + (provider->provider_cost ? 1 : 0) + (provider->token_cost ? 1 : 0) +
           ((provider->status && provider->status->indicator != CODEXBAR_STATUS_NONE) ? 1 : 0);
}

static int provider_metric_height(const CodexBarProvider *provider, guint index) {
    int overview_height = provider_overview_height(provider);
    if (overview_height > 0) {
        if (index == 0) return overview_height;
        index--;
    }
    if (index < provider->quota_windows->len) {
        return rate_window_height(codexbar_provider_quota_window(provider, index));
    }
    index -= provider->quota_windows->len;
    if (index < provider->balances->len) return balance_height(codexbar_provider_balance(provider, index));
    index -= provider->balances->len;
    if (provider->provider_cost) {
        if (index == 0) return provider_cost_height(provider->provider_cost);
        index--;
    }
    if (provider->token_cost) {
        if (index == 0) return token_cost_height(provider->token_cost);
        index--;
    }
    if (provider->status && provider->status->indicator != CODEXBAR_STATUS_NONE && index == 0) {
        return service_status_height(provider->status);
    }
    return 0;
}

static int draw_provider_metric(const CodexBarProvider *provider, guint index, int y, int x, int width) {
    if (provider_overview_height(provider) > 0) {
        if (index == 0) return draw_provider_overview(y, x, width, provider);
        index--;
    }
    if (index < provider->quota_windows->len) {
        return draw_rate_window(y, x, width, codexbar_provider_quota_window(provider, index));
    }
    index -= provider->quota_windows->len;
    if (index < provider->balances->len) {
        return draw_balance(y, x, width, codexbar_provider_balance(provider, index));
    }
    index -= provider->balances->len;
    if (provider->provider_cost) {
        if (index == 0) return draw_provider_cost(y, x, width, provider->provider_cost);
        index--;
    }
    if (provider->token_cost) {
        if (index == 0) return draw_token_cost(y, x, width, provider->token_cost);
        index--;
    }
    if (provider->status && provider->status->indicator != CODEXBAR_STATUS_NONE && index == 0) {
        return draw_service_status(y, x, width, provider->status);
    }
    return y;
}

static void draw_provider(
    const CodexBarProvider *provider, guint first_metric, int y, int x, int height, int width) {
    int bottom = y + height;
    guint metric_count = provider_metric_count(provider);
    if (first_metric > 0 && y + 1 + provider_metric_height(provider, first_metric) <= bottom) {
        attron(COLOR_PAIR(COLOR_MUTED));
        draw_text(y++, x, width, "↑ k earlier metrics");
        attroff(COLOR_PAIR(COLOR_MUTED));
    }
    gboolean has_more = FALSE;
    for (guint index = first_metric; index < metric_count; index++) {
        int metric_height = provider_metric_height(provider, index);
        if (y + metric_height > bottom) {
            has_more = TRUE;
            break;
        }
        y = draw_provider_metric(provider, index, y, x, width);
    }
    if (has_more && bottom > y) {
        attron(COLOR_PAIR(COLOR_MUTED));
        draw_text(bottom - 1, x, width, "↓ j more metrics");
        attroff(COLOR_PAIR(COLOR_MUTED));
    }
    if (!has_more && provider->error && y < bottom) {
        attron(COLOR_PAIR(COLOR_ERROR));
        draw_text(y, x, width, provider->error);
        attroff(COLOR_PAIR(COLOR_ERROR));
    }
}

static void draw_actions(const GPtrArray *actions, guint selected, int y, int x, int height, int width) {
    attron(COLOR_PAIR(COLOR_ACCENT) | A_BOLD);
    draw_text(y++, x, width, "Actions");
    attroff(COLOR_PAIR(COLOR_ACCENT) | A_BOLD);
    y++;
    for (guint index = 0; actions && index < actions->len && y < height; index++, y++) {
        const CodexBarTuiAction *action = g_ptr_array_index((GPtrArray *)actions, index);
        if (index == selected) {
            attron(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
        } else {
            attron(COLOR_PAIR(COLOR_NORMAL));
        }
        char *label = g_strdup_printf("%s %s", index == selected ? ">" : " ", action->label);
        draw_text(y, x, width, label);
        g_free(label);
        if (index == selected) {
            attroff(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
        } else {
            attroff(COLOR_PAIR(COLOR_NORMAL));
        }
    }
}

static void draw_about(int y, int x, int height, int width) {
    attron(COLOR_PAIR(COLOR_ACCENT) | A_BOLD);
    draw_text(y++, x, width, "CodexBar");
    attroff(COLOR_PAIR(COLOR_ACCENT) | A_BOLD);
    attron(COLOR_PAIR(COLOR_MUTED));
    char *version = g_strdup_printf("Version %s", CODEXBAR_LINUX_VERSION);
    draw_text(y++, x, width, version);
    g_free(version);
    draw_text(y++, x, width, "Native C23 terminal usage monitor for Linux");
    if (y < height) {
        char *path = codexbar_config_resolve_path();
        char *config = g_strdup_printf("Config: %s", path);
        draw_text(y++, x, width, config);
        g_free(config);
        g_free(path);
    }
    if (y + 1 < height) draw_text(y + 1, x, width, "Enter, Space, or Esc to return");
    attroff(COLOR_PAIR(COLOR_MUTED));
}

static void draw_screen(const CodexBarSnapshot *snapshot,
                        guint selected,
                        guint first_metric,
                        const char *status,
                        CodexBarTuiMode mode,
                        const GPtrArray *actions,
                        guint selected_action) {
    erase();
    int rows = 0;
    int columns = 0;
    getmaxyx(stdscr, rows, columns);

    if (rows < 16 || columns < 48) {
        attron(COLOR_PAIR(COLOR_ERROR));
        mvprintw(0, 0, "CodexBar needs at least 48x16; current terminal is %dx%d", columns, rows);
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
        double highest = codexbar_provider_highest_used(provider);
        char *tab = provider->error ? g_strdup_printf("%s !", provider->provider)
                                    : g_strdup_printf("%s %.0f%%", provider->provider, highest);
        if (index == selected) {
            attron(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
        } else {
            attron(COLOR_PAIR(COLOR_MUTED));
        }
        mvaddnstr(panel_y + 1, tab_x, tab, available);
        if (index == selected) {
            attroff(COLOR_PAIR(COLOR_SELECTED) | A_BOLD);
        } else {
            attroff(COLOR_PAIR(COLOR_MUTED));
        }
        int drawn_width = MIN((int)g_utf8_strlen(tab, -1), available);
        tab_x += drawn_width + 3;
        g_free(tab);
    }

    int card_y = panel_y + 3;
    int card_x = panel_x + 2;
    int card_height = panel_height - 5;
    int card_width = panel_width - 4;
    draw_border(card_y, card_x, card_height, card_width);
    if (mode == CODEXBAR_TUI_MODE_ACTIONS) {
        draw_actions(actions, selected_action, card_y + 1, card_x + 3, card_y + card_height - 1, card_width - 6);
    } else if (mode == CODEXBAR_TUI_MODE_ABOUT) {
        draw_about(card_y + 1, card_x + 3, card_y + card_height - 1, card_width - 6);
    } else if (snapshot->providers->len > 0) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, selected);
        draw_provider(provider, first_metric, card_y + 1, card_x + 3, card_height - 2, card_width - 6);
    } else {
        attron(COLOR_PAIR(COLOR_MUTED));
        mvaddstr(card_y + 1, card_x + 3, "No enabled provider returned data.");
        attroff(COLOR_PAIR(COLOR_MUTED));
    }

    attron(COLOR_PAIR(COLOR_MUTED));
    draw_text(panel_y + panel_height - 2,
              panel_x + 3,
               panel_width - 31,
               status ? status
                      : mode == CODEXBAR_TUI_MODE_ACTIONS ? "j/k choose  Enter run  Esc back  q quit"
                      : mode == CODEXBAR_TUI_MODE_ABOUT   ? "Enter/Esc back  q quit"
                                                         : "Enter actions  h/l tabs  j/k metrics  r refresh  q quit");
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

static gboolean ensure_config_file(GError **error) {
    char *path = codexbar_config_resolve_path();
    gboolean exists = g_file_test(path, G_FILE_TEST_EXISTS);
    g_free(path);
    if (exists) return TRUE;
    CodexBarConfig *config = codexbar_config_load_for_update(error);
    if (!config) return FALSE;
    gboolean result = g_file_test(config->path, G_FILE_TEST_EXISTS) || codexbar_config_save(config, error);
    codexbar_config_free(config);
    return result;
}

static void execute_action(const GPtrArray *actions,
                           guint selected,
                           CodexBarTuiMode *mode,
                           gboolean *running,
                           char **status) {
    if (!actions || selected >= actions->len) return;
    const CodexBarTuiAction *action = g_ptr_array_index((GPtrArray *)actions, selected);
    GError *error = NULL;
    if (action->kind == CODEXBAR_TUI_ACTION_SETTINGS && !ensure_config_file(&error)) {
        g_free(*status);
        *status = g_strdup_printf("could not create config: %s", error ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }
    CodexBarTuiEffect effect = CODEXBAR_TUI_EFFECT_OPENED;
    if (!codexbar_tui_action_execute_default(action, &effect, &error)) {
        g_free(*status);
        *status = g_strdup_printf(
            "could not open %s: %s", action->label, error ? error->message : "unknown error");
        g_clear_error(&error);
        return;
    }
    switch (effect) {
    case CODEXBAR_TUI_EFFECT_OPENED:
        g_free(*status);
        *status = g_strdup_printf("opened %s", action->label);
        *mode = CODEXBAR_TUI_MODE_USAGE;
        break;
    case CODEXBAR_TUI_EFFECT_SHOW_ABOUT:
        *mode = CODEXBAR_TUI_MODE_ABOUT;
        break;
    case CODEXBAR_TUI_EFFECT_QUIT:
        *running = FALSE;
        break;
    }
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
    draw_screen(empty, 0, 0, status, CODEXBAR_TUI_MODE_USAGE, NULL, 0);
    codexbar_snapshot_free(empty);
    g_free(status);
    status = NULL;

    CodexBarSnapshot *snapshot = fetch_snapshot(&status);
    guint selected = 0;
    guint first_metric = 0;
    CodexBarTuiMode mode = CODEXBAR_TUI_MODE_USAGE;
    GPtrArray *actions = NULL;
    guint selected_action = 0;
    gboolean help_visible = FALSE;
    gboolean pending_z = FALSE;
    gboolean running = TRUE;
    while (running) {
        if (snapshot->providers->len > 0 && selected >= snapshot->providers->len) {
            selected = snapshot->providers->len - 1;
        }
        const char *visible_status = status;
        if (help_visible && mode == CODEXBAR_TUI_MODE_USAGE) {
            visible_status = "h/l provider  j/k metrics  1-9 select  r refresh  q/Esc/ZZ quit";
        }
        draw_screen(snapshot, selected, first_metric, visible_status, mode, actions, selected_action);
        int key = getch();
        if (pending_z) {
            pending_z = FALSE;
            if (key == 'Z') {
                running = FALSE;
                continue;
            }
        }
        if (mode == CODEXBAR_TUI_MODE_ABOUT) {
            if (key == 'q') {
                running = FALSE;
            } else if (key == 27 || key == '\n' || key == KEY_ENTER || key == ' ') {
                mode = CODEXBAR_TUI_MODE_USAGE;
            }
            continue;
        }
        if (mode == CODEXBAR_TUI_MODE_ACTIONS) {
            if (key == 'q') {
                running = FALSE;
            } else if (key == 27) {
                mode = CODEXBAR_TUI_MODE_USAGE;
            } else if (key == KEY_DOWN || key == 'j') {
                if (actions && selected_action + 1 < actions->len) selected_action++;
            } else if (key == KEY_UP || key == 'k') {
                if (selected_action > 0) selected_action--;
            } else if (key == '\n' || key == KEY_ENTER) {
                execute_action(actions, selected_action, &mode, &running, &status);
            }
            continue;
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
                first_metric = 0;
            }
            break;
        case KEY_RIGHT:
        case 'l':
        case ']':
        case '\t':
            if (snapshot->providers->len > 0) {
                selected = (selected + 1) % snapshot->providers->len;
                first_metric = 0;
            }
            break;
        case 'g':
            selected = 0;
            first_metric = 0;
            break;
        case 'G':
            if (snapshot->providers->len > 0) {
                selected = snapshot->providers->len - 1;
                first_metric = 0;
            }
            break;
        case KEY_UP:
        case 'k':
            if (first_metric > 0) first_metric--;
            break;
        case KEY_DOWN:
        case 'j':
            if (snapshot->providers->len > 0) {
                const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, selected);
                guint metric_count = provider_metric_count(provider);
                if (first_metric + 1 < metric_count) first_metric++;
            }
            break;
        case '?':
            help_visible = !help_visible;
            break;
        case '\n':
        case KEY_ENTER:
            g_clear_pointer(&actions, g_ptr_array_unref);
            if (snapshot->providers->len > 0) {
                const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, selected);
                char *path = codexbar_config_resolve_path();
                actions = codexbar_tui_actions_new(provider->provider, path);
                g_free(path);
            } else {
                char *path = codexbar_config_resolve_path();
                actions = codexbar_tui_actions_new(NULL, path);
                g_free(path);
            }
            selected_action = 0;
            mode = CODEXBAR_TUI_MODE_ACTIONS;
            help_visible = FALSE;
            g_clear_pointer(&status, g_free);
            break;
        case 'Z':
            pending_z = TRUE;
            break;
        case 'r':
        case 18: {
            g_free(status);
            status = g_strdup("refreshing provider telemetry...");
            draw_screen(snapshot, selected, first_metric, status, mode, actions, selected_action);
            codexbar_snapshot_free(snapshot);
            g_free(status);
            status = NULL;
            snapshot = fetch_snapshot(&status);
            first_metric = 0;
            break;
        }
        case KEY_RESIZE:
            break;
        default:
            if (key >= '1' && key <= '9') {
                guint requested = (guint)(key - '1');
                if (requested < snapshot->providers->len) {
                    selected = requested;
                    first_metric = 0;
                }
            }
            break;
        }
    }

    codexbar_snapshot_free(snapshot);
    g_clear_pointer(&actions, g_ptr_array_unref);
    g_free(status);
    endwin();
    return 0;
}
