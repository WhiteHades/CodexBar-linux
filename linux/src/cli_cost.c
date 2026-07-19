#include "cli_cost.h"

#include "cost.h"
#include "provider_registry.h"

#include <json-c/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *provider;
    gboolean json;
    gboolean pretty;
    gboolean group_projects;
    int days;
} CostOptions;

static const char *value_option(int argc, char **argv, int *index, GError **error) {
    if (*index + 1 >= argc || argv[*index + 1][0] == '-') {
        g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "Missing value for %s.", argv[*index]);
        return NULL;
    }
    (*index)++;
    return argv[*index];
}

static gboolean parse_options(int argc, char **argv, CostOptions *options, GError **error) {
    *options = (CostOptions){.days = 30};
    for (int index = 0; index < argc; index++) {
        const char *argument = argv[index];
        if (g_str_equal(argument, "--provider")) {
            options->provider = value_option(argc, argv, &index, error);
            if (!options->provider) return FALSE;
        } else if (g_str_equal(argument, "--format")) {
            const char *format = value_option(argc, argv, &index, error);
            if (!format) return FALSE;
            if (g_str_equal(format, "json")) {
                options->json = TRUE;
            } else if (g_str_equal(format, "text")) {
                options->json = FALSE;
            } else {
                g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "--format must be text or json.");
                return FALSE;
            }
        } else if (g_str_equal(argument, "--json") || g_str_equal(argument, "--json-only")) {
            options->json = TRUE;
        } else if (g_str_equal(argument, "--pretty")) {
            options->pretty = TRUE;
        } else if (g_str_equal(argument, "--days")) {
            const char *raw = value_option(argc, argv, &index, error);
            if (!raw) return FALSE;
            char *end = NULL;
            long days = strtol(raw, &end, 10);
            if (*end != '\0' || days < 1 || days > 365) {
                g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "--days must be from 1 through 365.");
                return FALSE;
            }
            options->days = (int)days;
        } else if (g_str_equal(argument, "--group-by")) {
            const char *group = value_option(argc, argv, &index, error);
            if (!group) return FALSE;
            if (!g_str_equal(group, "project")) {
                g_set_error_literal(error, G_OPTION_ERROR, G_OPTION_ERROR_BAD_VALUE, "--group-by must be project.");
                return FALSE;
            }
            options->group_projects = TRUE;
        } else if (g_str_equal(argument, "--log-level")) {
            if (!value_option(argc, argv, &index, error)) return FALSE;
        } else if (g_str_equal(argument, "--refresh") || g_str_equal(argument, "--no-color") ||
                   g_str_equal(argument, "--json-output") || g_str_equal(argument, "--verbose") ||
                   g_str_equal(argument, "-v")) {
            continue;
        } else if (g_str_equal(argument, "--help") || g_str_equal(argument, "-h")) {
            puts("Usage: codexbar-linux cost [--provider <codex|claude|both|all>] [--format <text|json>]\n"
                 "                           [--days <1..365>] [--group-by project] [--refresh]");
            return FALSE;
        } else {
            g_set_error(error, G_OPTION_ERROR, G_OPTION_ERROR_UNKNOWN_OPTION, "Unknown argument: %s", argument);
            return FALSE;
        }
    }
    return TRUE;
}

static const CodexBarCostDay *today_entry(const CodexBarCostReport *report) {
    for (guint index = 0; index < report->days->len; index++) {
        const CodexBarCostDay *day = g_ptr_array_index(report->days, index);
        if (g_str_equal(day->date, report->today)) return day;
    }
    return NULL;
}

static char *token_text(gint64 tokens) {
    if (tokens >= 1000000) return g_strdup_printf("%.1fM", (double)tokens / 1000000.0);
    if (tokens >= 1000) return g_strdup_printf("%.1fK", (double)tokens / 1000.0);
    return g_strdup_printf("%" G_GINT64_FORMAT, tokens);
}

static char *cost_text(gboolean known, double cost) {
    return known ? g_strdup_printf("$%.4f", cost) : g_strdup("-");
}

static json_object *cost_json_value(double cost) {
    char text[G_ASCII_DTOSTR_BUF_SIZE];
    g_ascii_formatd(text, sizeof(text), "%.12g", cost);
    return json_object_new_double_s(cost, text);
}

static void print_report_text(const CodexBarCostReport *report, gboolean group_projects) {
    gboolean codex = g_str_equal(report->provider, "codex");
    printf("%s %s\n", codex ? "Codex" : "Claude", codex ? "API-equivalent estimate (not billed)" : "Cost (API-rate estimate)");
    if (group_projects && codex) {
        printf("Projects (Last %d days):\n", report->history_days);
        if (report->projects->len == 0) puts("-");
        for (guint index = 0; index < report->projects->len; index++) {
            const CodexBarCostProject *project = g_ptr_array_index(report->projects, index);
            char *tokens = token_text(project->total_tokens);
            char *cost = cost_text(project->cost_known, project->total_cost_usd);
            printf("%s: %s, %s tokens\n", project->name, cost, tokens);
            if (project->path && project->path[0] != '\0') printf("  %s\n", project->path);
            g_free(cost);
            g_free(tokens);
        }
    } else {
        const CodexBarCostDay *today = today_entry(report);
        char *today_tokens = token_text(today ? today->total_tokens : 0);
        char *today_cost = cost_text(today ? today->cost_known : TRUE, today ? today->cost_usd : 0);
        char *history_tokens = token_text(report->total_tokens);
        char *history_cost = cost_text(report->cost_known || report->days->len == 0, report->total_cost_usd);
        printf("Today: %s, %s tokens\n", today_cost, today_tokens);
        printf("Last %d days: %s, %s tokens\n", report->history_days, history_cost, history_tokens);
        g_free(history_cost);
        g_free(history_tokens);
        g_free(today_cost);
        g_free(today_tokens);
    }
    puts(codex ? "Not a subscription bill or plan value; local usage times public API prices"
               : "Estimate from local usage and public API prices");
    if (report->skipped_fork_files > 0) {
        fprintf(stderr,
                "Warning: skipped %u fork or subagent log files until lineage accounting is available in C.\n",
                report->skipped_fork_files);
    }
}

static json_object *day_json(const CodexBarCostDay *day) {
    json_object *object = json_object_new_object();
    json_object_object_add(object, "date", json_object_new_string(day->date));
    json_object_object_add(object, "inputTokens", json_object_new_int64(day->input_tokens));
    json_object_object_add(object, "outputTokens", json_object_new_int64(day->output_tokens));
    json_object_object_add(object, "cacheReadTokens", json_object_new_int64(day->cache_read_tokens));
    json_object_object_add(object, "cacheCreationTokens", json_object_new_int64(day->cache_creation_tokens));
    json_object_object_add(object, "totalTokens", json_object_new_int64(day->total_tokens));
    json_object_object_add(object, "totalCost", day->cost_known ? cost_json_value(day->cost_usd) : NULL);
    return object;
}

static json_object *report_json(const CodexBarCostReport *report) {
    json_object *object = json_object_new_object();
    json_object_object_add(object, "provider", json_object_new_string(report->provider));
    json_object_object_add(object, "source", json_object_new_string("local"));
    json_object_object_add(object, "currencyCode", json_object_new_string("USD"));
    json_object_object_add(object, "historyDays", json_object_new_int(report->history_days));
    const CodexBarCostDay *today = today_entry(report);
    json_object_object_add(object, "sessionTokens", today ? json_object_new_int64(today->total_tokens) : NULL);
    json_object_object_add(object,
                           "sessionCostUSD",
                           today && today->cost_known ? cost_json_value(today->cost_usd) : NULL);
    json_object_object_add(object,
                           "last30DaysTokens",
                           report->days->len > 0 ? json_object_new_int64(report->total_tokens) : NULL);
    json_object_object_add(object,
                           "last30DaysCostUSD",
                           report->cost_known ? cost_json_value(report->total_cost_usd) : NULL);
    json_object *daily = json_object_new_array_ext((int)report->days->len);
    for (guint index = 0; index < report->days->len; index++) {
        json_object_array_add(daily, day_json(g_ptr_array_index(report->days, index)));
    }
    json_object_object_add(object, "daily", daily);
    json_object *projects = json_object_new_array_ext((int)report->projects->len);
    for (guint index = 0; index < report->projects->len; index++) {
        const CodexBarCostProject *project = g_ptr_array_index(report->projects, index);
        json_object *item = json_object_new_object();
        json_object_object_add(item, "name", json_object_new_string(project->name));
        json_object_object_add(item,
                               "path",
                               project->path && project->path[0] != '\0' ? json_object_new_string(project->path) : NULL);
        json_object_object_add(item, "totalTokens", json_object_new_int64(project->total_tokens));
        json_object_object_add(item,
                               "totalCost",
                               project->cost_known ? cost_json_value(project->total_cost_usd) : NULL);
        json_object_array_add(projects, item);
    }
    json_object_object_add(object, "projects", projects);
    json_object *totals = json_object_new_object();
    json_object_object_add(totals, "totalTokens", json_object_new_int64(report->total_tokens));
    json_object_object_add(totals,
                           "totalCost",
                           report->cost_known ? cost_json_value(report->total_cost_usd) : NULL);
    json_object_object_add(object, "totals", totals);
    json_object_object_add(object, "skippedForkFiles", json_object_new_int64(report->skipped_fork_files));
    return object;
}

static int run_reports(const CostOptions *options, const char *const *providers, guint count) {
    json_object *array = options->json ? json_object_new_array_ext((int)count) : NULL;
    for (guint index = 0; index < count; index++) {
        GError *error = NULL;
        CodexBarCostReport *report = codexbar_cost_scan(providers[index], options->days, &error);
        if (!report) {
            if (array) json_object_put(array);
            fprintf(stderr, "Error: %s\n", error ? error->message : "Cost scan failed.");
            g_clear_error(&error);
            return 1;
        }
        if (array) {
            json_object_array_add(array, report_json(report));
        } else {
            if (index > 0) putchar('\n');
            print_report_text(report, options->group_projects);
        }
        codexbar_cost_report_free(report);
    }
    if (array) {
        puts(json_object_to_json_string_ext(
            array, options->pretty ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
        json_object_put(array);
    }
    return 0;
}

int codexbar_cli_cost_run(int argc, char **argv) {
    CostOptions options;
    GError *error = NULL;
    if (!parse_options(argc, argv, &options, &error)) {
        if (error) {
            fprintf(stderr, "Error: %s\n", error->message);
            g_clear_error(&error);
            return 1;
        }
        return 0;
    }
    const char *providers[] = {"codex", "claude"};
    if (!options.provider || g_ascii_strcasecmp(options.provider, "all") == 0 ||
        g_ascii_strcasecmp(options.provider, "both") == 0) {
        return run_reports(&options, providers, G_N_ELEMENTS(providers));
    }
    char *lowercase = g_ascii_strdown(options.provider, -1);
    const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(lowercase);
    g_free(lowercase);
    if (!descriptor) {
        fprintf(stderr, "Error: Unknown provider: %s\n", options.provider);
        return 1;
    }
    if (!g_str_equal(descriptor->id, "codex") && !g_str_equal(descriptor->id, "claude")) {
        fprintf(stderr, "Error: cost is only supported for Claude and Codex.\n");
        return 1;
    }
    const char *selected[] = {descriptor->id};
    return run_reports(&options, selected, 1);
}
