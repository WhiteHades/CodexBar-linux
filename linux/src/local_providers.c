#define _POSIX_C_SOURCE 200809L

#include "local_providers.h"

#include "process.h"

#include <dirent.h>
#include <errno.h>
#include <json-c/json.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define LOCAL_PROCESS_OUTPUT_LIMIT (1024U * 1024U)
#define ANTIGRAVITY_RESPONSE_LIMIT (1024U * 1024U)
#define ANTIGRAVITY_FILE_LIMIT (4U * 1024U * 1024U)

static gboolean valid_text(const char *text) {
    return text && g_utf8_validate(text, -1, NULL);
}

static gboolean regex_capture(const char *text, const char *pattern, guint capture, char **result) {
    *result = NULL;
    GRegex *regex = g_regex_new(pattern, G_REGEX_CASELESS | G_REGEX_OPTIMIZE, 0, NULL);
    if (!regex) return FALSE;
    GMatchInfo *match = NULL;
    gboolean found = g_regex_match(regex, text, 0, &match);
    if (found && g_match_info_get_match_count(match) > (gint)capture) {
        *result = g_match_info_fetch(match, (gint)capture);
        if (*result) g_strstrip(*result);
    }
    g_match_info_free(match);
    g_regex_unref(regex);
    return *result != NULL;
}

static char *strip_ansi(const char *text) {
    if (!valid_text(text)) return NULL;
    GRegex *regex = g_regex_new("\\x1b\\[[0-9;?]*[A-Za-z]|\\x1b\\][^\\x07]*\\x07", G_REGEX_OPTIMIZE, 0, NULL);
    if (!regex) return g_strdup(text);
    char *clean = g_regex_replace_literal(regex, text, -1, 0, "", 0, NULL);
    g_regex_unref(regex);
    return clean;
}

static gboolean parse_finite(const char *raw, double *result) {
    if (!raw || raw[0] == '\0') return FALSE;
    char *end = NULL;
    double value = g_ascii_strtod(raw, &end);
    if (!end || end == raw || *end != '\0' || !isfinite(value)) return FALSE;
    *result = value;
    return TRUE;
}

static gboolean parse_iso_ms(const char *raw, gint64 *result) {
    if (!raw) return FALSE;
    GDateTime *time = g_date_time_new_from_iso8601(raw, NULL);
    if (!time) {
        double seconds = 0;
        if (!parse_finite(raw, &seconds) || seconds < 0 || seconds > (double)G_MAXINT64 / 1000.0) return FALSE;
        *result = (gint64)(seconds * 1000.0);
        return TRUE;
    }
    *result = g_date_time_to_unix(time) * 1000 + g_date_time_get_microsecond(time) / 1000;
    g_date_time_unref(time);
    return TRUE;
}

static gboolean parse_local_date_ms(const char *raw, const char *format, gint64 now_ms, gint64 *result) {
    int first = 0;
    int second = 0;
    int third = 0;
    if (g_str_equal(format, "ymd")) {
        if (sscanf(raw, "%d-%d-%d", &first, &second, &third) != 3) return FALSE;
    } else if (sscanf(raw, "%d/%d/%d", &first, &second, &third) != 3) {
        return FALSE;
    }
    int year = g_str_equal(format, "ymd") ? first : third;
    int month = g_str_equal(format, "ymd") ? second : first;
    int day = g_str_equal(format, "ymd") ? third : second;
    GDateTime *date = g_date_time_new_local(year, month, day, 0, 0, 0);
    if (!date) return FALSE;
    *result = g_date_time_to_unix(date) * 1000;
    g_date_time_unref(date);
    (void)now_ms;
    return TRUE;
}

static gboolean parse_kiro_reset_ms(const char *text, gint64 now_ms, gint64 *result) {
    char *date = NULL;
    if (!regex_capture(text, "resets on[ \\t]+([0-9]{4}-[0-9]{2}-[0-9]{2}|[0-9]{2}/[0-9]{2})", 1, &date)) {
        return FALSE;
    }
    gboolean valid = FALSE;
    if (strchr(date, '-')) {
        valid = parse_local_date_ms(date, "ymd", now_ms, result);
    } else {
        int month = 0;
        int day = 0;
        if (sscanf(date, "%d/%d", &month, &day) == 2) {
            GDateTime *now = g_date_time_new_from_unix_local(now_ms / 1000);
            int year = now ? g_date_time_get_year(now) : 1970;
            GDateTime *candidate = g_date_time_new_local(year, month, day, 0, 0, 0);
            if (candidate && g_date_time_to_unix(candidate) * 1000 <= now_ms) {
                g_date_time_unref(candidate);
                candidate = g_date_time_new_local(year + 1, month, day, 0, 0, 0);
            }
            if (candidate) {
                *result = g_date_time_to_unix(candidate) * 1000;
                valid = TRUE;
                g_date_time_unref(candidate);
            }
            if (now) g_date_time_unref(now);
        }
    }
    g_free(date);
    return valid;
}

static char *kiro_plan_name(const char *text, gboolean *new_format) {
    *new_format = FALSE;
    char *name = NULL;
    if (regex_capture(text, "\\|[ \\t]*(KIRO[ \\t]+[A-Za-z0-9_]+)", 1, &name)) return name;
    if (regex_capture(text, "Estimated Usage[ \\t]*\\|[^\\n|]*\\|[ \\t]*([A-Z][A-Z0-9 ]+)", 1, &name)) {
        return name;
    }
    if (regex_capture(text, "Plan:[ \\t]*([^\\n\\r]+)", 1, &name)) {
        *new_format = TRUE;
        return name;
    }
    return g_strdup("Kiro");
}

static char *kiro_display_plan_name(const char *plan) {
    char **words = g_strsplit_set(plan, " \t", -1);
    GString *display = g_string_new(NULL);
    for (guint index = 0; words[index]; index++) {
        if (!*words[index]) continue;
        if (display->len) g_string_append_c(display, ' ');
        if (g_ascii_strcasecmp(words[index], "kiro") == 0) {
            g_string_append(display, "Kiro");
        } else if (g_ascii_strcasecmp(plan, "kiro") == 0 || g_ascii_strncasecmp(plan, "kiro ", 5) == 0) {
            char *lower = g_utf8_strdown(words[index], -1);
            const char *remainder = g_utf8_next_char(lower);
            char *first = g_utf8_strup(lower, remainder - lower);
            g_string_append(display, first);
            g_string_append(display, remainder);
            g_free(first);
            g_free(lower);
        } else {
            g_string_append(display, words[index]);
        }
    }
    g_strfreev(words);
    return g_string_free(display, FALSE);
}

static void kiro_account(const char *output, char **email, char **method) {
    *email = NULL;
    *method = NULL;
    char *clean = strip_ansi(output ? output : "");
    if (!clean) return;
    char **lines = g_strsplit(clean, "\n", -1);
    for (guint index = 0; lines[index]; index++) {
        char *line = g_strstrip(lines[index]);
        if (!*line) continue;
        if (!*method && g_ascii_strncasecmp(line, "Logged in with", 14) == 0) {
            char *candidate = g_strdup(line + 14);
            g_strstrip(candidate);
            if (*candidate) *method = candidate;
            else g_free(candidate);
        } else if (!*email && g_ascii_strncasecmp(line, "Email:", 6) == 0) {
            char *candidate = g_strdup(line + 6);
            g_strstrip(candidate);
            if (*candidate) *email = candidate;
            else g_free(candidate);
        } else if (!*email && strchr(line, '@') && !strchr(line, ' ') && !strchr(line, '\t')) {
            *email = g_strdup(line);
        }
    }
    g_strfreev(lines);
    g_free(clean);
}

static gboolean login_required(const char *text) {
    char *clean = strip_ansi(text ? text : "");
    if (!clean) return FALSE;
    char *lower = g_utf8_strdown(clean, -1);
    gboolean required = strstr(lower, "not logged in") || strstr(lower, "login required") ||
                        strstr(lower, "failed to initialize auth portal") || strstr(lower, "kiro-cli login") ||
                        strstr(lower, "oauth error") || strstr(lower, "authentication failed") ||
                        strstr(lower, "auggie login");
    g_free(lower);
    g_free(clean);
    return required;
}

static void add_extension_number(json_object *object, const char *key, gboolean present, double value) {
    if (present) json_object_object_add(object, key, json_object_new_double(value));
}

CodexBarProvider *codexbar_kiro_parse_usage(const char *usage_output,
                                            const char *account_output,
                                            const char *context_output,
                                            gint64 now_ms,
                                            GError **error) {
    char *text = strip_ansi(usage_output);
    if (!text || !*g_strstrip(text)) {
        g_free(text);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Empty output from kiro-cli");
        return NULL;
    }
    char *lower = g_utf8_strdown(text, -1);
    if (login_required(text)) {
        g_free(lower);
        g_free(text);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Not logged in to Kiro. Run 'kiro-cli login' first.");
        return NULL;
    }
    if (strstr(lower, "could not retrieve usage information")) {
        g_free(lower);
        g_free(text);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Kiro CLI could not retrieve usage information");
        return NULL;
    }

    gboolean new_format = FALSE;
    char *plan = kiro_plan_name(text, &new_format);
    gboolean managed = strstr(lower, "managed by admin") || strstr(lower, "managed by organization");
    gboolean has_percent = FALSE;
    gboolean has_credits = FALSE;
    double percent = 0;
    double used = 0;
    double total = 50;
    char *first = NULL;
    char *second = NULL;
    if (regex_capture(text, "█+[ \\t]*([0-9]+(?:\\.[0-9]+)?)%", 1, &first)) {
        has_percent = parse_finite(first, &percent);
        g_free(first);
    }
    if (regex_capture(text,
                      "\\(([0-9]+(?:\\.[0-9]+)?)[ \\t]+of[ \\t]+([0-9]+(?:\\.[0-9]+)?)[ \\t]+covered",
                      1,
                      &first) &&
        regex_capture(text,
                      "\\([0-9]+(?:\\.[0-9]+)?[ \\t]+of[ \\t]+([0-9]+(?:\\.[0-9]+)?)[ \\t]+covered",
                      1,
                      &second)) {
        has_credits = parse_finite(first, &used) && parse_finite(second, &total);
        g_free(first);
        g_free(second);
    }
    if (!has_percent && has_credits && total > 0) percent = used / total * 100.0;
    if (!has_percent && !has_credits && !(new_format && managed)) {
        g_free(plan);
        g_free(lower);
        g_free(text);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "No recognizable Kiro usage patterns found");
        return NULL;
    }
    if (!has_percent && !has_credits) total = 0;

    gboolean has_bonus = FALSE;
    double bonus_used = 0;
    double bonus_total = 0;
    if (regex_capture(text, "Bonus credits:[ \\t]*([0-9]+(?:\\.[0-9]+)?)/([0-9]+(?:\\.[0-9]+)?)", 1, &first) &&
        regex_capture(text, "Bonus credits:[ \\t]*[0-9]+(?:\\.[0-9]+)?/([0-9]+(?:\\.[0-9]+)?)", 1, &second)) {
        has_bonus = parse_finite(first, &bonus_used) && parse_finite(second, &bonus_total);
        g_free(first);
        g_free(second);
    }
    int expiry_days = -1;
    if (regex_capture(text, "expires in[ \\t]+([0-9]+)[ \\t]+days?", 1, &first)) {
        char *end = NULL;
        long value = strtol(first, &end, 10);
        if (end && *end == '\0' && value >= 0 && value <= G_MAXINT) expiry_days = (int)value;
        g_free(first);
    }
    gint64 reset_ms = 0;
    gboolean has_reset = parse_kiro_reset_ms(text, now_ms, &reset_ms);

    char *email = NULL;
    char *method = NULL;
    kiro_account(account_output, &email, &method);
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("kiro");
    provider->source = g_strdup("cli");
    provider->plan = plan;
    provider->account = email;
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->login_method = method;
    provider->explicit_quota_slots = TRUE;
    CodexBarQuotaWindow *primary = codexbar_quota_window_new("primary", "credits");
    primary->usage_known = has_percent || has_credits || (new_format && managed);
    primary->used_percent = codexbar_usage_percent_display(codexbar_usage_percent_from_raw(percent));
    primary->has_resets_at = has_reset;
    primary->resets_at_ms = reset_ms;
    primary->detail = g_strdup_printf("%.2f / %.2f credits", used, total);
    codexbar_provider_add_quota_window(provider, primary);
    if (has_bonus && bonus_total > 0) {
        CodexBarQuotaWindow *bonus = codexbar_quota_window_new("secondary", "bonus");
        bonus->usage_known = TRUE;
        bonus->used_percent = codexbar_usage_percent_display(
            codexbar_usage_percent_from_ratio(bonus_used, bonus_total));
        if (expiry_days >= 0) {
            bonus->has_resets_at = TRUE;
            bonus->resets_at_ms = now_ms + (gint64)expiry_days * 24 * 60 * 60 * 1000;
            bonus->reset_description = g_strdup_printf("expires in %dd", expiry_days);
        }
        bonus->detail = g_strdup_printf("%.2f / %.2f bonus credits", bonus_used, bonus_total);
        codexbar_provider_add_quota_window(provider, bonus);
    }

    json_object *details = json_object_new_object();
    json_object_object_add(details, "planName", json_object_new_string(plan));
    char *display_plan = kiro_display_plan_name(plan);
    json_object_object_add(details, "displayPlanName", json_object_new_string(display_plan));
    g_free(display_plan);
    json_object_object_add(details, "creditsUsed", json_object_new_double(used));
    json_object_object_add(details, "creditsTotal", json_object_new_double(total));
    json_object_object_add(details, "creditsRemaining", json_object_new_double(MAX(0.0, total - used)));
    add_extension_number(details, "bonusCreditsUsed", has_bonus, bonus_used);
    add_extension_number(details, "bonusCreditsTotal", has_bonus, bonus_total);
    add_extension_number(details, "bonusCreditsRemaining", has_bonus, MAX(0.0, bonus_total - bonus_used));
    if (expiry_days >= 0) json_object_object_add(details, "bonusExpiryDays", json_object_new_int(expiry_days));
    if (regex_capture(text, "Overages:[ \\t]*([^\\n\\r]+)", 1, &first)) {
        json_object_object_add(details, "overagesStatus", json_object_new_string(first));
        g_free(first);
    }
    if (regex_capture(text, "Credits used:[ \\t]*([0-9]+(?:\\.[0-9]+)?)", 1, &first)) {
        double value = 0;
        if (parse_finite(first, &value)) {
            json_object_object_add(details, "overageCreditsUsed", json_object_new_double(value));
        }
        g_free(first);
    }
    if (regex_capture(text, "Est\\.[ \\t]*cost:[ \\t]*\\$?([0-9]+(?:\\.[0-9]+)?)[ \\t]*USD", 1, &first)) {
        double value = 0;
        if (parse_finite(first, &value)) {
            json_object_object_add(details, "estimatedOverageCostUSD", json_object_new_double(value));
        }
        g_free(first);
    }
    if (strstr(text, "https://app.kiro.dev/account/usage")) {
        json_object_object_add(details, "manageURL", json_object_new_string("https://app.kiro.dev/account/usage"));
    }
    char *context = strip_ansi(context_output ? context_output : "");
    if (context && regex_capture(context, "Context window:[ \\t]*([0-9]+(?:\\.[0-9]+)?)%[ \\t]+used", 1, &first)) {
        double value = 0;
        json_object *context_usage = json_object_new_object();
        if (parse_finite(first, &value)) {
            json_object_object_add(context_usage, "totalPercentUsed", json_object_new_double(value));
        }
        g_free(first);
        const char *labels[] = {"Context files", "Tools", "Kiro responses", "Your prompts"};
        const char *keys[] = {"contextFilesPercent", "toolsPercent", "kiroResponsesPercent", "promptsPercent"};
        for (guint index = 0; index < G_N_ELEMENTS(labels); index++) {
            char *escaped = g_regex_escape_string(labels[index], -1);
            char *pattern = g_strdup_printf("%s[ \\t]+([0-9]+(?:\\.[0-9]+)?)%%", escaped);
            if (regex_capture(context, pattern, 1, &first)) {
                if (parse_finite(first, &value)) {
                    json_object_object_add(context_usage, keys[index], json_object_new_double(value));
                }
                g_free(first);
            }
            g_free(pattern);
            g_free(escaped);
        }
        json_object_object_add(details, "contextUsage", context_usage);
    }
    g_free(context);
    provider->usage_extensions = json_object_new_object();
    json_object_object_add(provider->usage_extensions, "kiroUsage", details);
    g_free(lower);
    g_free(text);
    return provider;
}

static char *combined_process_output(const CodexBarProcessResult *result) {
    if (!result || !g_utf8_validate(result->standard_output, (gssize)result->standard_output_length, NULL) ||
        !g_utf8_validate(result->standard_error, (gssize)result->standard_error_length, NULL) ||
        memchr(result->standard_output, '\0', result->standard_output_length) ||
        memchr(result->standard_error, '\0', result->standard_error_length)) return NULL;
    return g_strdup_printf("%s%s%s",
                           result->standard_output,
                           result->standard_output_length && result->standard_error_length ? "\n" : "",
                           result->standard_error);
}

static CodexBarProcessResult *run_local_command(const char *const *arguments,
                                                 guint timeout_ms,
                                                 GCancellable *cancellable,
                                                 GError **error) {
    CodexBarProcessRequest request = {
        .arguments = arguments,
        .timeout_milliseconds = timeout_ms,
        .termination_grace_milliseconds = 300,
        .maximum_output_bytes = LOCAL_PROCESS_OUTPUT_LIMIT,
        .new_session = TRUE,
    };
    return codexbar_process_run(&request, cancellable, error);
}

CodexBarProvider *codexbar_kiro_fetch_with_binary_and_cancellable(const char *binary,
                                                                  GCancellable *cancellable,
                                                                  GError **error) {
    if (!binary || !*binary) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "kiro-cli not found");
        return NULL;
    }
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) return NULL;
    const char *account_argv[] = {binary, "whoami", NULL};
    GError *account_error = NULL;
    CodexBarProcessResult *account_result = run_local_command(account_argv, 3000, cancellable, &account_error);
    char *account = combined_process_output(account_result);
    if (account && login_required(account)) {
        codexbar_process_result_free(account_result);
        g_free(account);
        g_clear_error(&account_error);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Not logged in to Kiro. Run 'kiro-cli login' first.");
        return NULL;
    }
    codexbar_process_result_free(account_result);
    g_clear_error(&account_error);

    const char *usage_argv[] = {binary, "chat", "--no-interactive", "/usage", NULL};
    CodexBarProcessResult *usage_result = run_local_command(usage_argv, 20000, cancellable, error);
    if (!usage_result) {
        g_free(account);
        return NULL;
    }
    char *usage = combined_process_output(usage_result);
    gboolean usage_ok = codexbar_process_result_succeeded(usage_result);
    codexbar_process_result_free(usage_result);
    if (!usage || !usage_ok) {
        g_free(usage);
        g_free(account);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Kiro usage command failed");
        return NULL;
    }

    const char *context_argv[] = {binary, "chat", "--no-interactive", "/context", NULL};
    GError *context_error = NULL;
    CodexBarProcessResult *context_result = run_local_command(context_argv, 8000, cancellable, &context_error);
    if (!context_result && context_error && g_error_matches(context_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_propagate_error(error, context_error);
        g_free(usage);
        g_free(account);
        return NULL;
    }
    char *context = combined_process_output(context_result);
    codexbar_process_result_free(context_result);
    g_clear_error(&context_error);
    CodexBarProvider *provider = codexbar_kiro_parse_usage(
        usage, account, context, g_get_real_time() / 1000, error);
    g_free(context);
    g_free(usage);
    g_free(account);
    return provider;
}

CodexBarProvider *codexbar_kiro_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                        GCancellable *cancellable,
                                                        GError **error) {
    (void)config;
    char *binary = g_find_program_in_path("kiro-cli");
    CodexBarProvider *provider = codexbar_kiro_fetch_with_binary_and_cancellable(binary, cancellable, error);
    g_free(binary);
    return provider;
}

CodexBarProvider *codexbar_kiro_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_kiro_fetch_with_cancellable(config, NULL, error);
}

static gboolean comma_number(const char *raw, double *result) {
    char *clean = g_strdup(raw);
    for (char *cursor = clean; *cursor; cursor++) {
        if (*cursor == ',') memmove(cursor, cursor + 1, strlen(cursor));
    }
    gboolean valid = parse_finite(clean, result);
    g_free(clean);
    return valid;
}

CodexBarProvider *codexbar_augment_parse_cli_usage(const char *output, gint64 now_ms, GError **error) {
    char *text = strip_ansi(output);
    if (!text || !*g_strstrip(text)) {
        g_free(text);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Auggie account status returned no output");
        return NULL;
    }
    if (login_required(text)) {
        g_free(text);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED,
                            "Auggie is not authenticated. Run 'auggie login'.");
        return NULL;
    }

    gboolean has_remaining = FALSE;
    gboolean has_used = FALSE;
    gboolean has_total = FALSE;
    gboolean has_monthly = FALSE;
    double remaining = 0;
    double used = 0;
    double total = 0;
    double monthly = 0;
    char *capture = NULL;
    if (regex_capture(text, "([0-9][0-9,]*)[ \\t]+credits[ \\t]+remaining", 1, &capture)) {
        has_remaining = comma_number(capture, &remaining);
        g_free(capture);
    }
    if (!has_remaining && regex_capture(text, "([0-9][0-9,]*)[ \\t]+remaining[^\\n]*credits[ \\t]+used", 1, &capture)) {
        has_remaining = comma_number(capture, &remaining);
        g_free(capture);
    }
    if (regex_capture(text, "([0-9][0-9,]*)[ \\t]*/[ \\t]*([0-9][0-9,]*)[ \\t]+credits[ \\t]+used", 1, &capture)) {
        has_used = comma_number(capture, &used);
        g_free(capture);
        if (regex_capture(text, "[0-9][0-9,]*[ \\t]*/[ \\t]*([0-9][0-9,]*)[ \\t]+credits[ \\t]+used", 1, &capture)) {
            has_total = comma_number(capture, &total);
            g_free(capture);
        }
    }
    if (regex_capture(text, "([0-9][0-9,]*)[ \\t]+credits[ \\t]*/[ \\t]*month", 1, &capture)) {
        has_monthly = comma_number(capture, &monthly);
        g_free(capture);
        if (!has_total) {
            total = monthly;
            has_total = TRUE;
        }
    }
    if (!has_remaining || !has_total) {
        g_free(text);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Could not extract credits from Auggie output");
        return NULL;
    }
    if (!has_used) used = MAX(0.0, total - remaining);
    gint64 reset_ms = 0;
    gboolean has_reset = FALSE;
    if (regex_capture(text, "ends[ \\t]+([0-9]{1,2}/[0-9]{1,2}/[0-9]{4})", 1, &capture)) {
        has_reset = parse_local_date_ms(capture, "mdy", now_ms, &reset_ms);
        g_free(capture);
    }

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("augment");
    provider->source = g_strdup("cli");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    if (has_monthly) provider->plan = g_strdup_printf("%.0f credits/month", monthly);
    CodexBarQuotaWindow *window = codexbar_quota_window_new("primary", "credits");
    window->usage_known = TRUE;
    window->used_percent = total > 0
                               ? codexbar_usage_percent_display(codexbar_usage_percent_from_ratio(used, total))
                               : 0;
    window->has_resets_at = has_reset;
    window->resets_at_ms = reset_ms;
    window->detail = g_strdup_printf("%.0f / %.0f credits", used, total);
    codexbar_provider_add_quota_window(provider, window);
    g_free(text);
    return provider;
}

CodexBarProvider *codexbar_augment_fetch_with_binary_and_cancellable(const char *binary,
                                                                     GCancellable *cancellable,
                                                                     GError **error) {
    if (!binary || !*binary) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "auggie not found");
        return NULL;
    }
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) return NULL;
    const char *argv[] = {binary, "account", "status", NULL};
    CodexBarProcessResult *result = run_local_command(argv, 15000, cancellable, error);
    if (!result) return NULL;
    char *output = combined_process_output(result);
    gboolean succeeded = codexbar_process_result_succeeded(result);
    codexbar_process_result_free(result);
    if (!output) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Auggie output is not valid UTF-8 text");
        return NULL;
    }
    if (!succeeded) {
        gboolean authentication_failure = login_required(output);
        g_free(output);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            authentication_failure ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_FAILED,
                            authentication_failure ? "Auggie is not authenticated. Run 'auggie login'."
                                                   : "Auggie account status failed");
        return NULL;
    }
    CodexBarProvider *provider = codexbar_augment_parse_cli_usage(output, g_get_real_time() / 1000, error);
    g_free(output);
    return provider;
}

CodexBarProvider *codexbar_augment_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                           GCancellable *cancellable,
                                                           GError **error) {
    (void)config;
    char *binary = g_find_program_in_path("auggie");
    CodexBarProvider *provider = codexbar_augment_fetch_with_binary_and_cancellable(binary, cancellable, error);
    g_free(binary);
    return provider;
}

CodexBarProvider *codexbar_augment_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_augment_fetch_with_cancellable(config, NULL, error);
}

typedef struct {
    char *group;
    char *bucket_id;
    char *title;
    char *description;
    gboolean known;
    double used_percent;
    gboolean has_reset;
    gint64 reset_ms;
    gboolean has_minutes;
    gint64 minutes;
    int group_rank;
    int bucket_rank;
    size_t order;
} AntigravityWindow;

static void antigravity_window_free(gpointer data) {
    AntigravityWindow *window = data;
    if (!window) return;
    g_free(window->group);
    g_free(window->bucket_id);
    g_free(window->title);
    g_free(window->description);
    g_free(window);
}

static json_object *parse_json_object_document(const char *text) {
    if (!text || !g_utf8_validate(text, -1, NULL)) return NULL;
    size_t length = strlen(text);
    if (length > G_MAXINT) return NULL;
    json_tokener *tokener = json_tokener_new();
    json_tokener_set_flags(tokener, JSON_TOKENER_STRICT | JSON_TOKENER_VALIDATE_UTF8);
    json_object *root = json_tokener_parse_ex(tokener, text, (int)length);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && strchr(" \t\r\n", text[consumed])) consumed++;
    gboolean valid = json_tokener_get_error(tokener) == json_tokener_success && root &&
                     json_object_is_type(root, json_type_object) && consumed == length;
    json_tokener_free(tokener);
    if (valid) return root;
    if (root) json_object_put(root);
    return NULL;
}

static gboolean json_display_string(json_object *object, const char *key, const char **result, gboolean required) {
    json_object *value = NULL;
    *result = NULL;
    if (!json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) {
        return !required;
    }
    if (!json_object_is_type(value, json_type_string)) return FALSE;
    const char *raw = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (memchr(raw, '\0', length) || !g_utf8_validate(raw, (gssize)length, NULL)) return FALSE;
    for (const char *cursor = raw; *cursor; cursor = g_utf8_next_char(cursor)) {
        if (g_unichar_iscntrl(g_utf8_get_char(cursor))) return FALSE;
    }
    *result = raw;
    return TRUE;
}

static gboolean json_optional_number(json_object *object, const char *key, gboolean *present, double *result) {
    json_object *value = NULL;
    *present = FALSE;
    if (!json_object_object_get_ex(object, key, &value) || json_object_is_type(value, json_type_null)) return TRUE;
    if (!json_object_is_type(value, json_type_double) && !json_object_is_type(value, json_type_int)) return FALSE;
    double number = json_object_get_double(value);
    if (!isfinite(number)) return FALSE;
    *present = TRUE;
    *result = number;
    return TRUE;
}

static gboolean antigravity_code_ok(json_object *root) {
    json_object *code = NULL;
    if (!json_object_object_get_ex(root, "code", &code) || json_object_is_type(code, json_type_null)) return TRUE;
    if (json_object_is_type(code, json_type_int)) return json_object_get_int64(code) == 0;
    if (!json_object_is_type(code, json_type_string)) return FALSE;
    const char *value = json_object_get_string(code);
    return g_ascii_strcasecmp(value, "ok") == 0 || g_ascii_strcasecmp(value, "success") == 0 ||
           g_str_equal(value, "0");
}

static int antigravity_group_rank(const char *title) {
    char *lower = g_utf8_strdown(title ? title : "", -1);
    int rank = strstr(lower, "gemini") ? 0 : (strstr(lower, "claude") || strstr(lower, "gpt")) ? 1 : 2;
    g_free(lower);
    return rank;
}

static gboolean cadence_suffix(const char *candidate, const char *alias) {
    if (g_str_equal(candidate, alias)) return TRUE;
    size_t candidate_length = strlen(candidate);
    size_t alias_length = strlen(alias);
    return candidate_length > alias_length && candidate[candidate_length - alias_length - 1] == '-' &&
           g_str_has_suffix(candidate, alias);
}

static int antigravity_bucket_rank(const char *bucket_id, const char *display, gint64 *minutes) {
    const char *raw_values[] = {bucket_id, display};
    const char *session_aliases[] = {"session", "5h", "5-hour", "five hour", "five-hour"};
    for (guint raw_index = 0; raw_index < G_N_ELEMENTS(raw_values); raw_index++) {
        char *candidate = g_utf8_strdown(raw_values[raw_index] ? raw_values[raw_index] : "", -1);
        g_strstrip(candidate);
        for (char *cursor = candidate; *cursor; cursor++) {
            if (*cursor == '_') *cursor = '-';
        }
        if (g_str_has_suffix(candidate, " limit")) candidate[strlen(candidate) - strlen(" limit")] = '\0';
        for (guint alias = 0; alias < G_N_ELEMENTS(session_aliases); alias++) {
            if (cadence_suffix(candidate, session_aliases[alias])) {
                *minutes = 300;
                g_free(candidate);
                return 0;
            }
        }
        if (cadence_suffix(candidate, "weekly")) {
            *minutes = 10080;
            g_free(candidate);
            return 1;
        }
        g_free(candidate);
    }
    return 2;
}

static gint antigravity_window_compare(gconstpointer left, gconstpointer right) {
    const AntigravityWindow *a = *(AntigravityWindow *const *)left;
    const AntigravityWindow *b = *(AntigravityWindow *const *)right;
    if (a->group_rank != b->group_rank) return a->group_rank - b->group_rank;
    if (a->bucket_rank != b->bucket_rank) return a->bucket_rank - b->bucket_rank;
    return a->order < b->order ? -1 : a->order > b->order ? 1 : 0;
}

static char *antigravity_group_title(const char *display) {
    int rank = antigravity_group_rank(display);
    if (rank == 0) return g_strdup("Gemini");
    if (rank == 1) return g_strdup("Claude/GPT");
    char *title = g_strdup(display && *display ? display : "Quota");
    g_strstrip(title);
    if (!*title) {
        g_free(title);
        return g_strdup("Quota");
    }
    return title;
}

static gboolean antigravity_collect_summary(json_object *root, GPtrArray *windows, GError **error) {
    if (!antigravity_code_ok(root)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Antigravity quota API returned an error");
        return FALSE;
    }
    json_object *payload = NULL;
    if (!json_object_object_get_ex(root, "response", &payload) || json_object_is_type(payload, json_type_null)) {
        if (!json_object_object_get_ex(root, "summary", &payload) || json_object_is_type(payload, json_type_null)) {
            payload = root;
        }
    }
    json_object *groups = NULL;
    if (!json_object_is_type(payload, json_type_object) || !json_object_object_get_ex(payload, "groups", &groups) ||
        !json_object_is_type(groups, json_type_array)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Antigravity quota summary is missing groups");
        return FALSE;
    }
    size_t order = 0;
    for (size_t group_index = 0; group_index < json_object_array_length(groups); group_index++) {
        json_object *group = json_object_array_get_idx(groups, group_index);
        const char *display = NULL;
        json_object *buckets = NULL;
        if (!json_object_is_type(group, json_type_object) ||
            !json_display_string(group, "displayName", &display, FALSE)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Antigravity quota group is malformed");
            return FALSE;
        }
        if (!json_object_object_get_ex(group, "buckets", &buckets) || json_object_is_type(buckets, json_type_null)) {
            continue;
        }
        if (!json_object_is_type(buckets, json_type_array)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Antigravity quota buckets are malformed");
            return FALSE;
        }
        char *group_title = antigravity_group_title(display);
        for (size_t bucket_index = 0; bucket_index < json_object_array_length(buckets); bucket_index++) {
            json_object *bucket = json_object_array_get_idx(buckets, bucket_index);
            const char *bucket_id = NULL;
            const char *bucket_display = NULL;
            const char *description = NULL;
            if (!json_object_is_type(bucket, json_type_object) ||
                !json_display_string(bucket, "bucketId", &bucket_id, TRUE) || !bucket_id || !*bucket_id ||
                !json_display_string(bucket, "displayName", &bucket_display, FALSE) ||
                !json_display_string(bucket, "description", &description, FALSE)) {
                g_free(group_title);
                g_set_error_literal(
                    error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Antigravity quota bucket is malformed");
                return FALSE;
            }
            gboolean disabled = FALSE;
            json_object *disabled_value = NULL;
            if (json_object_object_get_ex(bucket, "disabled", &disabled_value) &&
                !json_object_is_type(disabled_value, json_type_null)) {
                if (!json_object_is_type(disabled_value, json_type_boolean)) {
                    g_free(group_title);
                    g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                        "Antigravity quota disabled marker is malformed");
                    return FALSE;
                }
                disabled = json_object_get_boolean(disabled_value);
            }
            gboolean has_remaining = FALSE;
            double remaining = 0;
            if (!json_optional_number(bucket, "remainingFraction", &has_remaining, &remaining)) {
                g_free(group_title);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                    "Antigravity remaining fraction is malformed");
                return FALSE;
            }
            if (!has_remaining) {
                json_object *remaining_object = NULL;
                if (json_object_object_get_ex(bucket, "remaining", &remaining_object) &&
                    !json_object_is_type(remaining_object, json_type_null)) {
                    if (!json_object_is_type(remaining_object, json_type_object) ||
                        !json_optional_number(remaining_object, "remainingFraction", &has_remaining, &remaining)) {
                        g_free(group_title);
                        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                            "Antigravity remaining value is malformed");
                        return FALSE;
                    }
                    if (!has_remaining) {
                        const char *oneof = NULL;
                        gboolean oneof_valid = json_display_string(remaining_object, "case", &oneof, FALSE);
                        gboolean has_value = FALSE;
                        double value = 0;
                        if (!oneof_valid || !json_optional_number(remaining_object, "value", &has_value, &value)) {
                            g_free(group_title);
                            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                                "Antigravity remaining value is malformed");
                            return FALSE;
                        }
                        if (oneof && g_str_equal(oneof, "remainingFraction") && has_value) {
                            has_remaining = TRUE;
                            remaining = value;
                        }
                    }
                }
            }
            gint64 minutes = 0;
            int bucket_rank = antigravity_bucket_rank(bucket_id, bucket_display, &minutes);
            const char *cadence = bucket_rank == 0 ? "5-hour" : bucket_rank == 1 ? "weekly" : bucket_display;
            char *title = g_strdup_printf("%s %s", group_title, cadence && *cadence ? cadence : bucket_id);
            AntigravityWindow *window = g_new0(AntigravityWindow, 1);
            window->group = g_strdup(group_title);
            window->bucket_id = g_strdup(bucket_id);
            window->title = title;
            window->description = g_strdup(description);
            window->known = has_remaining && !disabled;
            window->used_percent = has_remaining ? 100.0 - CLAMP(remaining, 0.0, 1.0) * 100.0 : 0;
            window->has_minutes = bucket_rank < 2;
            window->minutes = minutes;
            window->group_rank = antigravity_group_rank(display);
            window->bucket_rank = bucket_rank;
            window->order = order++;
            const char *reset = NULL;
            if (!json_display_string(bucket, "resetTime", &reset, FALSE)) {
                antigravity_window_free(window);
                g_free(group_title);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                    "Antigravity quota reset is malformed");
                return FALSE;
            }
            window->has_reset = parse_iso_ms(reset, &window->reset_ms);
            g_ptr_array_add(windows, window);
        }
        g_free(group_title);
    }
    if (windows->len == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Antigravity quota summary has no buckets");
        return FALSE;
    }
    g_ptr_array_sort(windows, antigravity_window_compare);
    return TRUE;
}

static CodexBarQuotaWindow *antigravity_window_model(const char *id, const AntigravityWindow *source) {
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, source->title);
    window->usage_known = source->known;
    window->used_percent = source->used_percent;
    window->has_window_minutes = source->has_minutes;
    window->window_minutes = source->minutes;
    window->has_resets_at = source->has_reset;
    window->resets_at_ms = source->reset_ms;
    window->reset_description = g_strdup(source->description);
    return window;
}

static gboolean antigravity_identity(json_object *root, char **email, char **plan) {
    *email = NULL;
    *plan = NULL;
    if (!root || !antigravity_code_ok(root)) return FALSE;
    json_object *status = NULL;
    if (!json_object_object_get_ex(root, "userStatus", &status) || !json_object_is_type(status, json_type_object)) {
        return FALSE;
    }
    const char *value = NULL;
    if (!json_display_string(status, "email", &value, FALSE)) return FALSE;
    *email = g_strdup(value);
    json_object *tier = NULL;
    if (json_object_object_get_ex(status, "userTier", &tier) && !json_object_is_type(tier, json_type_null)) {
        if (!json_object_is_type(tier, json_type_object) || !json_display_string(tier, "name", &value, FALSE)) {
            g_free(*email);
            *email = NULL;
            return FALSE;
        }
        if (value && *value) *plan = g_strdup(value);
    }
    if (!*plan) {
        json_object *plan_status = NULL;
        json_object *plan_info = NULL;
        if (json_object_object_get_ex(status, "planStatus", &plan_status) &&
            json_object_is_type(plan_status, json_type_object) &&
            json_object_object_get_ex(plan_status, "planInfo", &plan_info) &&
            json_object_is_type(plan_info, json_type_object)) {
            const char *keys[] = {"planDisplayName", "displayName", "productName", "planName", "planShortName"};
            for (guint index = 0; index < G_N_ELEMENTS(keys) && !*plan; index++) {
                if (!json_display_string(plan_info, keys[index], &value, FALSE)) {
                    g_free(*email);
                    *email = NULL;
                    return FALSE;
                }
                if (value && *value) *plan = g_strdup(value);
            }
        }
    }
    return TRUE;
}

static CodexBarProvider *antigravity_from_summary(GPtrArray *windows,
                                                   json_object *identity_root,
                                                   gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("antigravity");
    provider->source = g_strdup("cli");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    antigravity_identity(identity_root, &provider->account, &provider->plan);
    if (provider->account || provider->plan) {
        provider->identity = g_new0(CodexBarProviderIdentity, 1);
        provider->identity->login_method = g_strdup(provider->plan);
    }
    const AntigravityWindow *representatives[2] = {NULL, NULL};
    for (guint index = 0; index < windows->len; index++) {
        AntigravityWindow *window = g_ptr_array_index(windows, index);
        if (window->known && window->group_rank < 2 &&
            (!representatives[window->group_rank] ||
             window->used_percent > representatives[window->group_rank]->used_percent)) {
            representatives[window->group_rank] = window;
        }
    }
    if (representatives[0]) {
        codexbar_provider_add_quota_window(provider, antigravity_window_model("primary", representatives[0]));
    }
    if (representatives[1]) {
        codexbar_provider_add_quota_window(provider, antigravity_window_model("secondary", representatives[1]));
    }
    for (guint index = 0; index < windows->len; index++) {
        AntigravityWindow *source = g_ptr_array_index(windows, index);
        char *internal_id = g_strdup_printf("antigravity-extra-%u", index);
        CodexBarQuotaWindow *window = antigravity_window_model(internal_id, source);
        g_free(internal_id);
        window->output_id = g_strdup_printf("antigravity-quota-summary-%s", source->bucket_id);
        codexbar_provider_add_quota_window(provider, window);
    }
    return provider;
}

static gboolean antigravity_model_family(const char *label, const char *model, int *rank, gboolean *summary) {
    char *combined = g_strdup_printf("%s %s", label ? label : "", model ? model : "");
    char *lower = g_utf8_strdown(combined, -1);
    g_free(combined);
    gboolean image = strstr(lower, "image") != NULL;
    gboolean lite = strstr(lower, "lite") != NULL;
    gboolean autocomplete = strstr(lower, "autocomplete") != NULL || g_str_has_prefix(lower, "tab_");
    if (strstr(lower, "claude") || strstr(lower, "gpt") || strstr(lower, "openai")) {
        *rank = 1;
    } else if (strstr(lower, "gemini") && (strstr(lower, "pro") || strstr(lower, "flash"))) {
        *rank = 0;
    } else {
        *rank = 2;
    }
    *summary = *rank < 2 && !image && !lite && !autocomplete;
    g_free(lower);
    return TRUE;
}

static json_object *antigravity_model_array(json_object *root, gboolean user_shape, GError **error) {
    if (!antigravity_code_ok(root)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Antigravity model API returned an error");
        return NULL;
    }
    json_object *container = root;
    if (user_shape) {
        if (!json_object_object_get_ex(root, "userStatus", &container) ||
            !json_object_is_type(container, json_type_object)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Antigravity user status is missing userStatus");
            return NULL;
        }
        json_object *cascade = NULL;
        if (!json_object_object_get_ex(container, "cascadeModelConfigData", &cascade) ||
            json_object_is_type(cascade, json_type_null)) {
            return json_object_new_array();
        }
        if (!json_object_is_type(cascade, json_type_object)) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Antigravity user model data is malformed");
            return NULL;
        }
        container = cascade;
    }
    json_object *models = NULL;
    if (!json_object_object_get_ex(container, "clientModelConfigs", &models) ||
        json_object_is_type(models, json_type_null)) {
        return json_object_new_array();
    }
    if (!json_object_is_type(models, json_type_array)) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Antigravity model list is malformed");
        return NULL;
    }
    return json_object_get(models);
}

static gboolean antigravity_collect_models(json_object *root,
                                            gboolean user_shape,
                                            GPtrArray *windows,
                                            GError **error) {
    json_object *models = antigravity_model_array(root, user_shape, error);
    if (!models) return FALSE;
    for (size_t index = 0; index < json_object_array_length(models); index++) {
        json_object *model_config = json_object_array_get_idx(models, index);
        const char *label = NULL;
        const char *model = NULL;
        json_object *alias = NULL;
        json_object *quota = NULL;
        if (!json_object_is_type(model_config, json_type_object) ||
            !json_display_string(model_config, "label", &label, TRUE) ||
            !json_object_object_get_ex(model_config, "modelOrAlias", &alias) ||
            !json_object_is_type(alias, json_type_object) || !json_display_string(alias, "model", &model, TRUE)) {
            json_object_put(models);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                "Antigravity model configuration is malformed");
            return FALSE;
        }
        if (!json_object_object_get_ex(model_config, "quotaInfo", &quota) ||
            json_object_is_type(quota, json_type_null)) {
            continue;
        }
        if (!json_object_is_type(quota, json_type_object)) {
            json_object_put(models);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Antigravity model quota is malformed");
            return FALSE;
        }
        gboolean has_remaining = FALSE;
        double remaining = 0;
        const char *reset = NULL;
        if (!json_optional_number(quota, "remainingFraction", &has_remaining, &remaining) ||
            !json_display_string(quota, "resetTime", &reset, FALSE)) {
            json_object_put(models);
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Antigravity model quota is malformed");
            return FALSE;
        }
        AntigravityWindow *window = g_new0(AntigravityWindow, 1);
        window->bucket_id = g_strdup(model);
        window->title = g_strdup(label && *label ? label : model);
        window->known = has_remaining;
        window->used_percent = has_remaining ? 100.0 - CLAMP(remaining, 0.0, 1.0) * 100.0 : 0;
        window->has_reset = parse_iso_ms(reset, &window->reset_ms);
        gboolean summary = FALSE;
        antigravity_model_family(label, model, &window->group_rank, &summary);
        window->bucket_rank = summary ? 0 : 2;
        window->order = index;
        g_ptr_array_add(windows, window);
    }
    json_object_put(models);
    if (windows->len == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Antigravity has no quota models");
        return FALSE;
    }
    return TRUE;
}

static CodexBarProvider *antigravity_from_models(GPtrArray *windows,
                                                  json_object *identity_root,
                                                  gint64 now_ms) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("antigravity");
    provider->source = g_strdup("cli");
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = now_ms;
    provider->explicit_quota_slots = TRUE;
    antigravity_identity(identity_root, &provider->account, &provider->plan);
    if (provider->account || provider->plan) {
        provider->identity = g_new0(CodexBarProviderIdentity, 1);
        provider->identity->login_method = g_strdup(provider->plan);
    }
    const AntigravityWindow *representatives[2] = {NULL, NULL};
    for (guint index = 0; index < windows->len; index++) {
        AntigravityWindow *window = g_ptr_array_index(windows, index);
        gboolean summary = window->bucket_rank == 0;
        if (summary && window->known &&
            (!representatives[window->group_rank] ||
             window->used_percent > representatives[window->group_rank]->used_percent)) {
            representatives[window->group_rank] = window;
        }
    }
    if (representatives[0]) {
        codexbar_provider_add_quota_window(provider, antigravity_window_model("primary", representatives[0]));
    }
    if (representatives[1]) {
        codexbar_provider_add_quota_window(provider, antigravity_window_model("secondary", representatives[1]));
    }
    for (guint index = 0; index < windows->len; index++) {
        AntigravityWindow *source = g_ptr_array_index(windows, index);
        gboolean reset_only_pool = source->bucket_rank == 0 && !source->known && source->has_reset &&
                                   !representatives[source->group_rank];
        if (source->bucket_rank == 0 && !reset_only_pool) continue;
        char *internal_id = g_strdup_printf("antigravity-extra-%u", index);
        CodexBarQuotaWindow *window = antigravity_window_model(internal_id, source);
        g_free(internal_id);
        if (reset_only_pool) {
            window->output_id = g_strdup(source->group_rank == 0 ? "antigravity-gemini"
                                                                 : "antigravity-claude-gpt");
        } else if (source->group_rank == 2 && source->known && !representatives[0] && !representatives[1]) {
            window->output_id = g_strdup_printf("antigravity-compact-fallback-%s", source->bucket_id);
        } else {
            window->output_id = g_strdup(source->bucket_id);
        }
        codexbar_provider_add_quota_window(provider, window);
    }
    return provider;
}

CodexBarProvider *codexbar_antigravity_parse_usage(const char *quota_summary_json,
                                                    const char *user_status_json,
                                                    const char *command_model_json,
                                                    gint64 now_ms,
                                                    GError **error) {
    json_object *identity = parse_json_object_document(user_status_json);
    if (quota_summary_json) {
        json_object *summary = parse_json_object_document(quota_summary_json);
        if (summary) {
            GPtrArray *windows = g_ptr_array_new_with_free_func(antigravity_window_free);
            GError *summary_error = NULL;
            gboolean parsed = antigravity_collect_summary(summary, windows, &summary_error);
            gboolean usable = FALSE;
            for (guint index = 0; parsed && index < windows->len; index++) {
                usable = usable || ((AntigravityWindow *)g_ptr_array_index(windows, index))->known;
            }
            if (parsed && usable) {
                CodexBarProvider *provider = antigravity_from_summary(windows, identity, now_ms);
                g_ptr_array_unref(windows);
                json_object_put(summary);
                if (identity) json_object_put(identity);
                return provider;
            }
            g_clear_error(&summary_error);
            g_ptr_array_unref(windows);
            json_object_put(summary);
        }
    }

    if (identity) {
        GPtrArray *windows = g_ptr_array_new_with_free_func(antigravity_window_free);
        GError *user_error = NULL;
        if (antigravity_collect_models(identity, TRUE, windows, &user_error)) {
            CodexBarProvider *provider = antigravity_from_models(windows, identity, now_ms);
            g_ptr_array_unref(windows);
            json_object_put(identity);
            return provider;
        }
        g_clear_error(&user_error);
        g_ptr_array_unref(windows);
    }

    json_object *command = parse_json_object_document(command_model_json);
    if (command) {
        GPtrArray *windows = g_ptr_array_new_with_free_func(antigravity_window_free);
        if (antigravity_collect_models(command, FALSE, windows, error)) {
            CodexBarProvider *provider = antigravity_from_models(windows, identity, now_ms);
            g_ptr_array_unref(windows);
            json_object_put(command);
            if (identity) json_object_put(identity);
            return provider;
        }
        g_ptr_array_unref(windows);
        json_object_put(command);
    } else if (!error || !*error) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Could not parse Antigravity quota");
    }
    if (identity) json_object_put(identity);
    return NULL;
}

static gboolean string_array_contains(const char *const *values, size_t count, const char *candidate) {
    for (size_t index = 0; index < count; index++) {
        if (g_str_equal(values[index], candidate)) return TRUE;
    }
    return FALSE;
}

static gint uint16_compare(gconstpointer left, gconstpointer right) {
    guint16 a = *(const guint16 *)left;
    guint16 b = *(const guint16 *)right;
    return a < b ? -1 : a > b ? 1 : 0;
}

GArray *codexbar_antigravity_parse_listening_ports(const char *proc_net_tcp,
                                                    const char *const *socket_inodes,
                                                    size_t socket_inode_count) {
    GArray *ports = g_array_new(FALSE, FALSE, sizeof(guint16));
    if (!proc_net_tcp || strlen(proc_net_tcp) > ANTIGRAVITY_FILE_LIMIT ||
        !g_utf8_validate(proc_net_tcp, -1, NULL) ||
        (socket_inode_count > 0 && !socket_inodes)) return ports;
    char **lines = g_strsplit(proc_net_tcp, "\n", -1);
    for (guint line_index = 0; lines[line_index]; line_index++) {
        char **columns = g_strsplit_set(g_strstrip(lines[line_index]), " \t", -1);
        GPtrArray *nonempty = g_ptr_array_new();
        for (guint column = 0; columns[column]; column++) {
            if (*columns[column]) g_ptr_array_add(nonempty, columns[column]);
        }
        if (nonempty->len > 9 && g_str_equal(g_ptr_array_index(nonempty, 3), "0A") &&
            string_array_contains(socket_inodes, socket_inode_count, g_ptr_array_index(nonempty, 9))) {
            const char *address = g_ptr_array_index(nonempty, 1);
            const char *colon = strrchr(address, ':');
            char *end = NULL;
            guint64 value = colon ? g_ascii_strtoull(colon + 1, &end, 16) : 0;
            if (colon && end && *end == '\0' && value > 0 && value <= G_MAXUINT16) {
                guint16 port = (guint16)value;
                gboolean duplicate = FALSE;
                for (guint index = 0; index < ports->len; index++) {
                    duplicate = duplicate || g_array_index(ports, guint16, index) == port;
                }
                if (!duplicate) g_array_append_val(ports, port);
            }
        }
        g_ptr_array_unref(nonempty);
        g_strfreev(columns);
    }
    g_strfreev(lines);
    g_array_sort(ports, uint16_compare);
    return ports;
}

typedef struct {
    guint16 port;
    char *csrf_token;
} AntigravityEndpoint;

static void antigravity_endpoint_free(gpointer data) {
    AntigravityEndpoint *endpoint = data;
    if (!endpoint) return;
    g_free(endpoint->csrf_token);
    g_free(endpoint);
}

static gboolean read_bounded_file(const char *path, size_t maximum, GByteArray **result) {
    *result = NULL;
    FILE *file = fopen(path, "rb");
    if (!file) return FALSE;
    GByteArray *bytes = g_byte_array_sized_new(4096);
    guint8 buffer[4096];
    gboolean valid = TRUE;
    while (!feof(file)) {
        size_t count = fread(buffer, 1, sizeof(buffer), file);
        if (count > maximum - MIN(maximum, bytes->len)) {
            valid = FALSE;
            break;
        }
        if (count > 0) g_byte_array_append(bytes, buffer, (guint)count);
        if (ferror(file)) {
            valid = FALSE;
            break;
        }
    }
    fclose(file);
    if (!valid) {
        g_byte_array_unref(bytes);
        return FALSE;
    }
    g_byte_array_append(bytes, (const guint8 *)"", 1);
    *result = bytes;
    return TRUE;
}

static char *process_command_line(pid_t pid) {
    char *path = g_strdup_printf("/proc/%ld/cmdline", (long)pid);
    GByteArray *bytes = NULL;
    gboolean loaded = read_bounded_file(path, 64U * 1024U, &bytes);
    g_free(path);
    if (!loaded || bytes->len <= 1) {
        if (bytes) g_byte_array_unref(bytes);
        return NULL;
    }
    size_t payload_length = bytes->len - 1;
    for (size_t index = 0; index < payload_length; index++) {
        if (bytes->data[index] == '\0') bytes->data[index] = ' ';
    }
    bytes->data[payload_length] = '\0';
    if (!g_utf8_validate((char *)bytes->data, (gssize)payload_length, NULL)) {
        g_byte_array_unref(bytes);
        return NULL;
    }
    char *result = g_strdup((char *)bytes->data);
    g_byte_array_unref(bytes);
    g_strstrip(result);
    return result;
}

static gboolean command_segment(const char *lower, const char *name) {
    size_t name_length = strlen(name);
    const char *match = lower;
    while ((match = strstr(match, name))) {
        char before = match == lower ? '/' : match[-1];
        char after = match[name_length];
        gboolean boundary_before = match == lower || before == '/' || before == '\\' || g_ascii_isspace(before);
        gboolean boundary_after = after == '\0' || after == '/' || after == '\\' || g_ascii_isspace(after);
        if (boundary_before && boundary_after) return TRUE;
        match += name_length;
    }
    return FALSE;
}

static int antigravity_process_kind(const char *command) {
    char *lower = g_utf8_strdown(command, -1);
    gboolean language_server = command_segment(lower, "language_server") || command_segment(lower, "language-server") ||
                               strstr(lower, "/language_server_") || strstr(lower, "/language-server-");
    gboolean antigravity = strstr(lower, "/antigravity/") || strstr(lower, "--app_data_dir antigravity") ||
                           strstr(lower, "--app_data_dir=antigravity");
    int kind = language_server && antigravity ? 1
               : (command_segment(lower, "agy") || command_segment(lower, "antigravity-cli") ||
                  command_segment(lower, "antigravity_cli"))
                   ? 2
                   : 0;
    g_free(lower);
    return kind;
}

static char *command_flag(const char *command, const char *flag) {
    char *escaped = g_regex_escape_string(flag, -1);
    char *pattern = g_strdup_printf("%s(?:=|[ \\t]+)([^ \\t]+)", escaped);
    char *result = NULL;
    regex_capture(command, pattern, 1, &result);
    g_free(pattern);
    g_free(escaped);
    if (!result) return NULL;
    for (const unsigned char *cursor = (const unsigned char *)result; *cursor; cursor++) {
        if (*cursor < 33 || *cursor == 127) {
            g_free(result);
            return NULL;
        }
    }
    return result;
}

static GPtrArray *process_socket_inodes(pid_t pid) {
    GPtrArray *inodes = g_ptr_array_new_with_free_func(g_free);
    char *directory_path = g_strdup_printf("/proc/%ld/fd", (long)pid);
    DIR *directory = opendir(directory_path);
    if (!directory) {
        g_free(directory_path);
        return inodes;
    }
    struct dirent *entry = NULL;
    guint examined = 0;
    while (examined++ < 4096 && (entry = readdir(directory))) {
        if (!g_ascii_isdigit(entry->d_name[0])) continue;
        char *path = g_build_filename(directory_path, entry->d_name, NULL);
        char target[256];
        ssize_t length = readlink(path, target, sizeof(target) - 1);
        g_free(path);
        if (length <= 9 || (size_t)length >= sizeof(target)) continue;
        target[length] = '\0';
        if (g_str_has_prefix(target, "socket:[") && target[length - 1] == ']') {
            target[length - 1] = '\0';
            const char *inode = target + strlen("socket:[");
            gboolean digits = *inode != '\0';
            for (const char *cursor = inode; digits && *cursor; cursor++) digits = g_ascii_isdigit(*cursor);
            if (digits) g_ptr_array_add(inodes, g_strdup(inode));
        }
    }
    closedir(directory);
    g_free(directory_path);
    return inodes;
}

static void append_process_ports(GPtrArray *endpoints, pid_t pid, const char *csrf_token) {
    GPtrArray *inodes = process_socket_inodes(pid);
    if (inodes->len == 0) {
        g_ptr_array_unref(inodes);
        return;
    }
    for (const char *file = "tcp"; file; file = g_str_equal(file, "tcp") ? "tcp6" : NULL) {
        char *path = g_strdup_printf("/proc/%ld/net/%s", (long)pid, file);
        GByteArray *bytes = NULL;
        gboolean loaded = read_bounded_file(path, ANTIGRAVITY_FILE_LIMIT, &bytes);
        g_free(path);
        if (!loaded) continue;
        GArray *ports = codexbar_antigravity_parse_listening_ports(
            (char *)bytes->data, (const char *const *)inodes->pdata, inodes->len);
        for (guint index = 0; index < ports->len && endpoints->len < 16; index++) {
            guint16 port = g_array_index(ports, guint16, index);
            gboolean duplicate = FALSE;
            for (guint existing = 0; existing < endpoints->len; existing++) {
                AntigravityEndpoint *endpoint = g_ptr_array_index(endpoints, existing);
                duplicate = duplicate || (endpoint->port == port && g_strcmp0(endpoint->csrf_token, csrf_token) == 0);
            }
            if (!duplicate) {
                AntigravityEndpoint *endpoint = g_new0(AntigravityEndpoint, 1);
                endpoint->port = port;
                endpoint->csrf_token = g_strdup(csrf_token);
                g_ptr_array_add(endpoints, endpoint);
            }
        }
        g_array_unref(ports);
        g_byte_array_unref(bytes);
    }
    g_ptr_array_unref(inodes);
}

static GPtrArray *antigravity_discover_endpoints(GCancellable *cancellable, GError **error) {
    GPtrArray *endpoints = g_ptr_array_new_with_free_func(antigravity_endpoint_free);
    DIR *directory = opendir("/proc");
    if (!directory) {
        g_set_error(error, G_IO_ERROR, g_io_error_from_errno(errno), "Could not scan /proc: %s", g_strerror(errno));
        return endpoints;
    }
    uid_t current_uid = getuid();
    struct dirent *entry = NULL;
    guint examined = 0;
    gboolean missing_token = FALSE;
    while (endpoints->len < 16 && examined++ < 65536 && (entry = readdir(directory))) {
        if (cancellable && g_cancellable_is_cancelled(cancellable)) {
            closedir(directory);
            g_ptr_array_unref(endpoints);
            g_cancellable_set_error_if_cancelled(cancellable, error);
            return NULL;
        }
        char *end = NULL;
        long raw_pid = strtol(entry->d_name, &end, 10);
        if (!end || *end != '\0' || raw_pid <= 0 || raw_pid > G_MAXINT) continue;
        char *process_path = g_build_filename("/proc", entry->d_name, NULL);
        struct stat information = {0};
        gboolean owned = stat(process_path, &information) == 0 && information.st_uid == current_uid;
        g_free(process_path);
        if (!owned) continue;
        char *command = process_command_line((pid_t)raw_pid);
        if (!command) continue;
        int kind = antigravity_process_kind(command);
        if (!kind) {
            g_free(command);
            continue;
        }
        char *token = command_flag(command, "--csrf_token");
        if (kind == 1 && !token) {
            missing_token = TRUE;
            g_free(command);
            continue;
        }
        append_process_ports(endpoints, (pid_t)raw_pid, token);
        g_free(token);
        g_free(command);
    }
    closedir(directory);
    if (endpoints->len == 0 && (!error || !*error)) {
        g_set_error_literal(error,
                            G_IO_ERROR,
                            missing_token ? G_IO_ERROR_PERMISSION_DENIED : G_IO_ERROR_NOT_FOUND,
                            missing_token ? "Antigravity CSRF token not found" : "Antigravity is not running");
    }
    return endpoints;
}

static char *antigravity_request(GPtrArray *endpoints,
                                 const char *path,
                                 const char *body,
                                 CodexBarLocalProviderTransport transport,
                                 GCancellable *cancellable,
                                 gint64 deadline_us,
                                 GError **error) {
    const char *schemes[] = {"https", "http"};
    GError *last_error = NULL;
    for (guint endpoint_index = 0; endpoint_index < endpoints->len; endpoint_index++) {
        AntigravityEndpoint *endpoint = g_ptr_array_index(endpoints, endpoint_index);
        for (guint scheme_index = 0; scheme_index < G_N_ELEMENTS(schemes); scheme_index++) {
            if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) {
                g_clear_error(&last_error);
                return NULL;
            }
            if (g_get_monotonic_time() >= deadline_us) {
                g_clear_error(&last_error);
                g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_TIMED_OUT, "Antigravity local probe timed out");
                return NULL;
            }
            char *url = g_strdup_printf("%s://127.0.0.1:%u%s", schemes[scheme_index], endpoint->port, path);
            char content_length[32];
            g_snprintf(content_length, sizeof(content_length), "%zu", strlen(body));
            CodexBarHttpRequestHeader headers[4] = {
                {"Content-Type", "application/json"},
                {"Content-Length", content_length},
                {"Connect-Protocol-Version", "1"},
                {"X-Codeium-Csrf-Token", endpoint->csrf_token},
            };
            CodexBarHttpRequest request = {
                .url = url,
                .method = "POST",
                .headers = headers,
                .header_count = endpoint->csrf_token && *endpoint->csrf_token ? 4 : 3,
                .body = body,
                .body_length = strlen(body),
                .timeout_seconds = 1,
                .maximum_response_bytes = ANTIGRAVITY_RESPONSE_LIMIT,
                .protocol_policy = CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP,
                .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
                .cancellable = cancellable,
            };
            GError *request_error = NULL;
            CodexBarHttpResponse *response = transport(&request, &request_error);
            g_free(url);
            if (!response) {
                if (request_error && g_error_matches(request_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
                    g_clear_error(&last_error);
                    g_propagate_error(error, request_error);
                    return NULL;
                }
                g_clear_error(&last_error);
                last_error = request_error;
                continue;
            }
            gboolean valid_body = response->body && response->body_length <= ANTIGRAVITY_RESPONSE_LIMIT &&
                                  !memchr(response->body, '\0', response->body_length) &&
                                  g_utf8_validate(response->body, (gssize)response->body_length, NULL);
            if (response->status == 200 && valid_body) {
                char *result = g_strndup(response->body, response->body_length);
                codexbar_http_response_free(response);
                g_clear_error(&request_error);
                g_clear_error(&last_error);
                return result;
            }
            long status = response->status;
            codexbar_http_response_free(response);
            g_clear_error(&request_error);
            g_clear_error(&last_error);
            if (!valid_body) {
                last_error = g_error_new_literal(G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                                                 "Antigravity returned an invalid response body");
            } else {
                last_error = g_error_new(G_IO_ERROR, G_IO_ERROR_FAILED,
                                         "Antigravity local API returned HTTP %ld", status);
            }
        }
    }
    if (last_error) g_propagate_error(error, last_error);
    else g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No Antigravity local endpoint responded");
    return NULL;
}

static CodexBarProvider *antigravity_fetch_endpoints(GPtrArray *endpoints,
                                                      CodexBarLocalProviderTransport transport,
                                                      GCancellable *cancellable,
                                                      gint64 now_ms,
                                                      GError **error) {
    if (endpoints->len == 0) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No Antigravity local ports were found");
        return NULL;
    }
    gint64 deadline_us = g_get_monotonic_time() + 8 * G_TIME_SPAN_SECOND;
    GError *quota_error = NULL;
    char *quota = antigravity_request(
        endpoints,
        "/exa.language_server_pb.LanguageServerService/RetrieveUserQuotaSummary",
        "{\"forceRefresh\":true}",
        transport,
        cancellable,
        deadline_us,
        &quota_error);
    if (quota_error && g_error_matches(quota_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_propagate_error(error, quota_error);
        return NULL;
    }
    g_clear_error(&quota_error);

    const char *default_body =
        "{\"metadata\":{\"ideName\":\"antigravity\",\"extensionName\":\"antigravity\","
        "\"ideVersion\":\"unknown\",\"locale\":\"en\"}}";
    GError *user_error = NULL;
    char *user = antigravity_request(
        endpoints,
        "/exa.language_server_pb.LanguageServerService/GetUserStatus",
        default_body,
        transport,
        cancellable,
        deadline_us,
        &user_error);
    if (user_error && g_error_matches(user_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_free(quota);
        g_propagate_error(error, user_error);
        return NULL;
    }
    g_clear_error(&user_error);

    GError *parse_error = NULL;
    CodexBarProvider *provider = codexbar_antigravity_parse_usage(quota, user, NULL, now_ms, &parse_error);
    if (provider) {
        g_free(user);
        g_free(quota);
        return provider;
    }
    g_clear_error(&parse_error);

    GError *command_error = NULL;
    char *command = antigravity_request(
        endpoints,
        "/exa.language_server_pb.LanguageServerService/GetCommandModelConfigs",
        default_body,
        transport,
        cancellable,
        deadline_us,
        &command_error);
    if (command_error && g_error_matches(command_error, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
        g_free(user);
        g_free(quota);
        g_propagate_error(error, command_error);
        return NULL;
    }
    g_clear_error(&command_error);
    provider = codexbar_antigravity_parse_usage(quota, user, command, now_ms, error);
    g_free(command);
    g_free(user);
    g_free(quota);
    return provider;
}

CodexBarProvider *codexbar_antigravity_fetch_ports_with_transport_and_cancellable(
    const guint16 *ports,
    size_t port_count,
    const char *csrf_token,
    CodexBarLocalProviderTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    if ((!ports && port_count > 0) || port_count > 16 || !transport ||
        (csrf_token && (!g_utf8_validate(csrf_token, -1, NULL) || strchr(csrf_token, '\n') ||
                        strchr(csrf_token, '\r')))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                            "Antigravity local endpoint configuration is invalid");
        return NULL;
    }
    if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) return NULL;
    GPtrArray *endpoints = g_ptr_array_new_with_free_func(antigravity_endpoint_free);
    for (size_t index = 0; index < port_count; index++) {
        if (ports[index] == 0) continue;
        AntigravityEndpoint *endpoint = g_new0(AntigravityEndpoint, 1);
        endpoint->port = ports[index];
        endpoint->csrf_token = g_strdup(csrf_token);
        g_ptr_array_add(endpoints, endpoint);
    }
    CodexBarProvider *provider = antigravity_fetch_endpoints(
        endpoints, transport, cancellable, now_ms, error);
    g_ptr_array_unref(endpoints);
    return provider;
}

CodexBarProvider *codexbar_antigravity_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarLocalProviderTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error) {
    (void)config;
    if (!transport) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Antigravity transport is required");
        return NULL;
    }
    GPtrArray *endpoints = antigravity_discover_endpoints(cancellable, error);
    if (!endpoints) return NULL;
    if (endpoints->len == 0) {
        g_ptr_array_unref(endpoints);
        return NULL;
    }
    CodexBarProvider *provider = antigravity_fetch_endpoints(
        endpoints, transport, cancellable, now_ms, error);
    g_ptr_array_unref(endpoints);
    return provider;
}

CodexBarProvider *codexbar_antigravity_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                               GCancellable *cancellable,
                                                               GError **error) {
    return codexbar_antigravity_fetch_with_transport_and_cancellable(
        config, codexbar_http_send, cancellable, g_get_real_time() / 1000, error);
}

CodexBarProvider *codexbar_antigravity_fetch(const CodexBarProviderConfig *config, GError **error) {
    return codexbar_antigravity_fetch_with_cancellable(config, NULL, error);
}
