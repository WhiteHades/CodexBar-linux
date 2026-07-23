#include "cli_cards.h"

#include "backend.h"
#include "provider_registry.h"

#include <glib.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum {
    CARD_WIDTH = 38,
    CARD_INNER_WIDTH = CARD_WIDTH - 4,
    BAR_WIDTH = 16,
};

typedef struct {
    const char *provider;
    const char *source;
    gboolean brief;
    gboolean include_credits;
} CardsOptions;

static const char *option_value(int argc, char **argv, int *index, GError **error) {
    if (*index + 1 >= argc || argv[*index + 1][0] == '-') {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Missing value for %s.", argv[*index]);
        return NULL;
    }
    (*index)++;
    return argv[*index];
}

static gboolean source_is_valid(const char *source) {
    return g_str_equal(source, "auto") || g_str_equal(source, "web") || g_str_equal(source, "cli") ||
           g_str_equal(source, "oauth") || g_str_equal(source, "api");
}

static gboolean parse_options(int argc, char **argv, CardsOptions *options, GError **error) {
    *options = (CardsOptions){.include_credits = TRUE};
    for (int index = 0; index < argc; index++) {
        const char *argument = argv[index];
        if (g_str_equal(argument, "--provider")) {
            options->provider = option_value(argc, argv, &index, error);
            if (!options->provider) return FALSE;
        } else if (g_str_equal(argument, "--source")) {
            options->source = option_value(argc, argv, &index, error);
            if (!options->source) return FALSE;
        } else if (g_str_equal(argument, "--web")) {
            options->source = "web";
        } else if (g_str_equal(argument, "--brief")) {
            options->brief = TRUE;
        } else if (g_str_equal(argument, "--no-credits")) {
            options->include_credits = FALSE;
        } else if (g_str_equal(argument, "--account") || g_str_equal(argument, "--account-index")) {
            (void)option_value(argc, argv, &index, error);
            if (*error) return FALSE;
            g_set_error_literal(error,
                                G_OPTION_ERROR,
                                G_OPTION_ERROR_FAILED,
                                "Account selection is not available in the native C command yet.");
            return FALSE;
        } else if (g_str_equal(argument, "--all-accounts")) {
            g_set_error_literal(error,
                                G_OPTION_ERROR,
                                G_OPTION_ERROR_FAILED,
                                "Account selection is not available in the native C command yet.");
            return FALSE;
        } else if (g_str_equal(argument, "--web-timeout") || g_str_equal(argument, "--log-level")) {
            if (!option_value(argc, argv, &index, error)) return FALSE;
        } else if (g_str_equal(argument, "--no-color") || g_str_equal(argument, "--status") ||
                   g_str_equal(argument, "--json-output") || g_str_equal(argument, "--verbose") ||
                   g_str_equal(argument, "-v") || g_str_equal(argument, "--web-debug-dump-html") ||
                   g_str_equal(argument, "--antigravity-plan-debug") || g_str_equal(argument, "--augment-debug")) {
            continue;
        } else if (g_str_equal(argument, "--help") || g_str_equal(argument, "-h")) {
            puts("Usage: codexbar-linux cards [--provider <name|both|all>] [--source <auto|web|cli|oauth|api>]\n"
                 "                            [--brief] [--no-credits] [--no-color] [--status]");
            return FALSE;
        } else {
            g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_UNKNOWN_OPTION, "Unknown argument: %s", argument);
            return FALSE;
        }
    }
    if (options->source && !source_is_valid(options->source)) {
        g_set_error_literal(error,
                            G_OPTION_ERROR,
                            G_OPTION_ERROR_BAD_VALUE,
                            "--source must be auto|web|cli|oauth|api.");
        return FALSE;
    }
    return TRUE;
}

static char *safe_text(const char *text, gsize limit) {
    if (!text) return g_strdup("");
    GString *output = g_string_sized_new(MIN(strlen(text), limit));
    const char *cursor = text;
    gsize characters = 0;
    while (*cursor != '\0' && characters < limit) {
        gunichar character = g_utf8_get_char_validated(cursor, -1);
        if (character == (gunichar)-1 || character == (gunichar)-2) {
            g_string_append_c(output, '?');
            cursor++;
            characters++;
            continue;
        }
        if (g_unichar_iscntrl(character)) {
            g_string_append_c(output, ' ');
        } else {
            char encoded[6];
            int length = g_unichar_to_utf8(character, encoded);
            g_string_append_len(output, encoded, length);
        }
        cursor = g_utf8_next_char(cursor);
        characters++;
    }
    return g_string_free(output, FALSE);
}

static char *title_case(const char *text) {
    char *value = safe_text(text, CARD_INNER_WIDTH);
    if (value[0] != '\0') value[0] = g_ascii_toupper(value[0]);
    return value;
}

static const char *provider_name(const CodexBarProvider *provider) {
    const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(provider->provider);
    return descriptor ? descriptor->display_name : provider->provider;
}

static void card_line(GPtrArray *lines, const char *text) {
    char *safe = safe_text(text, CARD_INNER_WIDTH);
    int visible = (int)g_utf8_strlen(safe, -1);
    if (visible > CARD_INNER_WIDTH) {
        char *end = g_utf8_offset_to_pointer(safe, CARD_INNER_WIDTH);
        *end = '\0';
        visible = CARD_INNER_WIDTH;
    }
    g_ptr_array_add(lines, g_strdup_printf("| %-*s |", CARD_INNER_WIDTH + (int)strlen(safe) - visible, safe));
    g_free(safe);
}

static void add_quota_lines(GPtrArray *lines, const CodexBarQuotaWindow *window) {
    char *title = title_case(window->title ? window->title : window->id);
    card_line(lines, title);
    g_free(title);

    if (!window->usage_known) {
        card_line(lines, "Usage unavailable");
        return;
    }

    int used = (int)(codexbar_usage_percent_display(codexbar_usage_percent_from_raw(window->used_percent)) + 0.5);
    int filled = (used * BAR_WIDTH + 50) / 100;
    GString *bar = g_string_new("[");
    for (int index = 0; index < BAR_WIDTH; index++) g_string_append_c(bar, index < filled ? '#' : '-');
    g_string_append_printf(bar, "] %d%% left", 100 - used);
    card_line(lines, bar->str);
    g_string_free(bar, TRUE);

    if (window->reset_description && window->reset_description[0] != '\0') {
        char *reset = g_strdup_printf("Reset: %s", window->reset_description);
        card_line(lines, reset);
        g_free(reset);
    } else if (window->detail && window->detail[0] != '\0') {
        card_line(lines, window->detail);
    }
}

static char *provider_cost_text(const CodexBarProviderCost *cost) {
    GString *text = g_string_new(NULL);
    g_string_printf(text,
                    "%s: %.2f %s",
                    cost->limit > 0 ? "Extra usage" : "API spend",
                    cost->used,
                    cost->currency);
    if (cost->limit > 0) g_string_append_printf(text, " / %.2f %s", cost->limit, cost->currency);
    if (cost->period) g_string_append_printf(text, " · %s", cost->period);
    return g_string_free(text, FALSE);
}

static GPtrArray *make_card(const CodexBarProvider *provider, gboolean include_credits) {
    GPtrArray *lines = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(lines, g_strdup("+------------------------------------+"));

    GString *heading = g_string_new(provider_name(provider));
    if (provider->source && provider->source[0] != '\0') g_string_append_printf(heading, " [%s]", provider->source);
    if (provider->plan && provider->plan[0] != '\0') g_string_append_printf(heading, " PLAN %s", provider->plan);
    card_line(lines, heading->str);
    g_string_free(heading, TRUE);

    if (provider->account && provider->account[0] != '\0') {
        char *account = g_strdup_printf("@ %s", provider->account);
        card_line(lines, account);
        g_free(account);
    }
    if (provider->note && provider->note[0] != '\0') card_line(lines, provider->note);

    for (guint index = 0; index < provider->quota_windows->len; index++) {
        add_quota_lines(lines, g_ptr_array_index(provider->quota_windows, index));
    }
    if (provider->provider_cost) {
        char *line = provider_cost_text(provider->provider_cost);
        card_line(lines, line);
        g_free(line);
    }
    if (include_credits) {
        for (guint index = 0; index < provider->balances->len; index++) {
            const CodexBarBalance *balance = g_ptr_array_index(provider->balances, index);
            char *amount = NULL;
            if (balance->unit && g_str_equal(balance->unit, "%")) {
                int rounded = (int)(CLAMP(balance->remaining, 0.0, 100.0) + 0.5);
                amount = g_strdup_printf("%d%%", rounded);
            } else {
                amount = g_strdup_printf("%.2f", balance->remaining);
            }
            char *title = title_case(balance->title ? balance->title : balance->id);
            char *line = g_strdup_printf("%s: %s left", title, amount);
            card_line(lines, line);
            g_free(line);
            g_free(title);
            g_free(amount);
        }
    }
    if (provider->status && provider->status->description) {
        char *line = g_strdup_printf("Status: %s", provider->status->description);
        card_line(lines, line);
        g_free(line);
    }

    g_ptr_array_add(lines, g_strdup("+------------------------------------+"));
    return lines;
}

static int terminal_columns(void) {
    const char *raw = g_getenv("COLUMNS");
    if (!raw || raw[0] == '\0') return 80;
    char *end = NULL;
    long value = strtol(raw, &end, 10);
    if (*end != '\0' || value < CARD_WIDTH || value > 1000) return 80;
    return (int)value;
}

static void render_cards(const GPtrArray *cards) {
    guint columns = MAX(1, (guint)((terminal_columns() + 2) / (CARD_WIDTH + 2)));
    columns = MIN(columns, cards->len);
    for (guint start = 0; start < cards->len; start += columns) {
        guint end = MIN(start + columns, cards->len);
        guint height = 0;
        for (guint index = start; index < end; index++) {
            const GPtrArray *card = g_ptr_array_index(cards, index);
            height = MAX(height, card->len);
        }
        for (guint line = 0; line < height; line++) {
            for (guint index = start; index < end; index++) {
                const GPtrArray *card = g_ptr_array_index(cards, index);
                printf("%s", line < card->len ? (const char *)g_ptr_array_index(card, line)
                                              : "                                      ");
                if (index + 1 < end) fputs("  ", stdout);
            }
            putchar('\n');
        }
    }
}

static void render_brief(const CodexBarSnapshot *snapshot) {
    puts("codexbar AI Usage & Limits");
    puts("Provider             Usage                    Reset");
    puts("--------------------  -----------------------  ----------------");
    for (guint index = 0; index < snapshot->providers->len; index++) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        if (provider->error) continue;
        const CodexBarQuotaWindow *window = provider->quota_windows->len > 0
            ? g_ptr_array_index(provider->quota_windows, 0)
            : NULL;
        char *name = safe_text(provider_name(provider), 20);
        double display_percent = window
            ? codexbar_usage_percent_display(codexbar_usage_percent_from_raw(window->used_percent))
            : 0.0;
        char *usage = window && window->usage_known
            ? g_strdup_printf("%s %.0f%% used", window->title ? window->title : "Usage", display_percent)
            : provider->provider_cost ? provider_cost_text(provider->provider_cost) : g_strdup("Usage unavailable");
        const char *reset_text = window
            ? (window->reset_description ? window->reset_description : window->detail)
            : NULL;
        char *reset = reset_text ? safe_text(reset_text, 16) : g_strdup("-");
        printf("%-20s  %-23s  %-16s\n", name, usage, reset);
        g_free(reset);
        g_free(usage);
        g_free(name);
    }
}

static guint render_failures(const CodexBarSnapshot *snapshot) {
    guint count = 0;
    for (guint index = 0; index < snapshot->providers->len; index++) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        if (provider->error) count++;
    }
    if (count == 0) return 0;
    puts("Failed providers:");
    for (guint index = 0; index < snapshot->providers->len; index++) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        if (!provider->error) continue;
        char *message = safe_text(provider->error, 300);
        printf("- %s: %s\n", provider_name(provider), message);
        g_free(message);
    }
    return count;
}

static CodexBarSnapshot *fetch_both(const char *source, GError **error) {
    CodexBarSnapshot *snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    const char *names[] = {"codex", "claude"};
    for (guint index = 0; index < G_N_ELEMENTS(names); index++) {
        CodexBarProvider *provider = codexbar_backend_fetch_one(names[index], source, error);
        if (!provider) {
            codexbar_snapshot_free(snapshot);
            return NULL;
        }
        g_ptr_array_add(snapshot->providers, provider);
    }
    return snapshot;
}

static CodexBarSnapshot *fetch_snapshot(const CardsOptions *options, GError **error) {
    if (!options->provider) return codexbar_backend_fetch(error);
    char *provider = g_ascii_strdown(options->provider, -1);
    CodexBarSnapshot *snapshot = NULL;
    if (g_str_equal(provider, "all")) {
        snapshot = codexbar_backend_fetch_all(error);
    } else if (g_str_equal(provider, "both")) {
        snapshot = fetch_both(options->source, error);
    } else {
        const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(provider);
        if (!descriptor) {
            g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Unknown provider: %s", options->provider);
        } else {
            CodexBarProvider *result = codexbar_backend_fetch_one(descriptor->id, options->source, error);
            if (result) {
                snapshot = g_new0(CodexBarSnapshot, 1);
                snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
                g_ptr_array_add(snapshot->providers, result);
            }
        }
    }
    g_free(provider);
    return snapshot;
}

int codexbar_cli_cards_run(int argc, char **argv) {
    CardsOptions options;
    GError *error = NULL;
    if (!parse_options(argc, argv, &options, &error)) {
        if (error) {
            fprintf(stderr, "Error: %s\n", error->message);
            g_clear_error(&error);
            return 1;
        }
        return 0;
    }

    CodexBarSnapshot *snapshot = fetch_snapshot(&options, &error);
    if (!snapshot) {
        fprintf(stderr, "Error: %s\n", error ? error->message : "Could not load usage data.");
        g_clear_error(&error);
        return 1;
    }

    if (options.brief) {
        render_brief(snapshot);
    } else {
        GPtrArray *cards = g_ptr_array_new_with_free_func((GDestroyNotify)g_ptr_array_unref);
        for (guint index = 0; index < snapshot->providers->len; index++) {
            const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
            if (!provider->error) g_ptr_array_add(cards, make_card(provider, options.include_credits));
        }
        render_cards(cards);
        g_ptr_array_unref(cards);
    }
    guint failures = render_failures(snapshot);
    codexbar_snapshot_free(snapshot);
    return failures > 0 ? 1 : 0;
}
