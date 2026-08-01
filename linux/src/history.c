#include "history.h"

#include "provider_registry.h"

#include <errno.h>
#include <glib/gstdio.h>
#include <math.h>
#include <string.h>

#define HISTORY_SCHEMA_VERSION 1
#define HISTORY_MAX_SAMPLES (24 * 730)
#define HISTORY_HOUR_MS (60 * 60 * 1000)
#define HISTORY_RESET_TOLERANCE_MS (2 * 60 * 1000)

struct CodexBarHistoryStore {
    GMutex lock;
    char *directory;
};

typedef struct {
    const char *name;
    gint64 window_minutes;
    gint64 captured_at_ms;
    double used_percent;
    gboolean has_reset;
    gint64 reset_at_ms;
} HistorySample;

static char *default_directory(void) {
    const char *override = g_getenv("CODEXBAR_HISTORY_DIR");
    if (override && override[0] != '\0') return g_strdup(override);
    return g_build_filename(g_get_user_data_dir(), "CodexBar", "history", NULL);
}

CodexBarHistoryStore *codexbar_history_store_new(const char *directory) {
    CodexBarHistoryStore *store = g_new0(CodexBarHistoryStore, 1);
    g_mutex_init(&store->lock);
    store->directory = directory ? g_strdup(directory) : default_directory();
    return store;
}

void codexbar_history_store_free(CodexBarHistoryStore *store) {
    if (!store) return;
    g_free(store->directory);
    g_mutex_clear(&store->lock);
    g_free(store);
}

static gboolean valid_provider_id(const char *provider) {
    const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(provider);
    return descriptor && g_str_equal(descriptor->id, provider);
}

static char *provider_path(CodexBarHistoryStore *store, const char *provider) {
    if (!valid_provider_id(provider)) return NULL;
    char *filename = g_strdup_printf("%s.json", provider);
    char *path = g_build_filename(store->directory, filename, NULL);
    g_free(filename);
    return path;
}

static json_object *empty_document(void) {
    json_object *document = json_object_new_object();
    json_object_object_add(document, "version", json_object_new_int(HISTORY_SCHEMA_VERSION));
    json_object_object_add(document, "preferredAccountKey", NULL);
    json_object_object_add(document, "unscoped", json_object_new_array());
    json_object_object_add(document, "accounts", json_object_new_object());
    return document;
}

static gboolean valid_document(json_object *document) {
    json_object *version = NULL;
    json_object *unscoped = NULL;
    json_object *accounts = NULL;
    return document && json_object_is_type(document, json_type_object) &&
           json_object_object_get_ex(document, "version", &version) &&
           json_object_is_type(version, json_type_int) &&
           json_object_get_int(version) == HISTORY_SCHEMA_VERSION &&
           json_object_object_get_ex(document, "unscoped", &unscoped) &&
           json_object_is_type(unscoped, json_type_array) &&
           json_object_object_get_ex(document, "accounts", &accounts) &&
           json_object_is_type(accounts, json_type_object);
}

static json_object *load_path(const char *path, gboolean missing_is_empty, GError **error) {
    char *contents = NULL;
    gsize length = 0;
    GError *read_error = NULL;
    if (!g_file_get_contents(path, &contents, &length, &read_error)) {
        if (missing_is_empty && g_error_matches(read_error, G_FILE_ERROR, G_FILE_ERROR_NOENT)) {
            g_error_free(read_error);
            return empty_document();
        }
        if (error) g_propagate_error(error, read_error);
        else g_clear_error(&read_error);
        return NULL;
    }
    json_tokener *tokener = json_tokener_new_ex(32);
    json_object *document = json_tokener_parse_ex(tokener, contents, (int)length);
    enum json_tokener_error parse_error = json_tokener_get_error(tokener);
    size_t consumed = json_tokener_get_parse_end(tokener);
    while (consumed < length && g_ascii_isspace((guchar)contents[consumed])) consumed++;
    json_tokener_free(tokener);
    g_free(contents);
    if (parse_error != json_tokener_success || consumed != length || !valid_document(document)) {
        if (document) json_object_put(document);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA,
                            "Plan-utilization history has an unsupported or invalid schema");
        return NULL;
    }
    return document;
}

json_object *codexbar_history_store_load_provider(CodexBarHistoryStore *store,
                                                  const char *provider,
                                                  GError **error) {
    g_return_val_if_fail(store != NULL, NULL);
    char *path = provider_path(store, provider);
    if (!path) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT,
                    "Unknown provider: %s", provider ? provider : "(null)");
        return NULL;
    }
    g_mutex_lock(&store->lock);
    json_object *document = load_path(path, FALSE, error);
    g_mutex_unlock(&store->lock);
    g_free(path);
    return document;
}

static char *iso8601(gint64 milliseconds) {
    GDateTime *date = g_date_time_new_from_unix_utc(milliseconds / 1000);
    if (!date) return NULL;
    GDateTime *with_fraction = g_date_time_add(date, (milliseconds % 1000) * 1000);
    char *text = g_date_time_format(with_fraction, "%Y-%m-%dT%H:%M:%S.%fZ");
    g_date_time_unref(with_fraction);
    g_date_time_unref(date);
    return text;
}

static gboolean timestamp_ms(json_object *entry, const char *key, gint64 *milliseconds) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(entry, key, &value) || !json_object_is_type(value, json_type_string)) {
        return FALSE;
    }
    GDateTime *date = g_date_time_new_from_iso8601(json_object_get_string(value), NULL);
    if (!date) return FALSE;
    *milliseconds = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static gint entry_compare(gconstpointer first, gconstpointer second) {
    json_object *left = *(json_object *const *)first;
    json_object *right = *(json_object *const *)second;
    gint64 left_time = 0;
    gint64 right_time = 0;
    timestamp_ms(left, "capturedAt", &left_time);
    timestamp_ms(right, "capturedAt", &right_time);
    if (left_time < right_time) return -1;
    if (left_time > right_time) return 1;
    double left_used = json_object_get_double(json_object_object_get(left, "usedPercent"));
    double right_used = json_object_get_double(json_object_object_get(right, "usedPercent"));
    return (left_used > right_used) - (left_used < right_used);
}

static json_object *entry_new(const HistorySample *sample) {
    json_object *entry = json_object_new_object();
    char *captured = iso8601(sample->captured_at_ms);
    json_object_object_add(entry, "capturedAt", json_object_new_string(captured));
    json_object_object_add(entry, "usedPercent", json_object_new_double(CLAMP(sample->used_percent, 0, 100)));
    if (sample->has_reset) {
        char *reset = iso8601(sample->reset_at_ms);
        json_object_object_add(entry, "resetsAt", json_object_new_string(reset));
        g_free(reset);
    } else {
        json_object_object_add(entry, "resetsAt", NULL);
    }
    g_free(captured);
    return entry;
}

static gboolean entry_reset(json_object *entry, gint64 *reset) {
    json_object *value = NULL;
    return json_object_object_get_ex(entry, "resetsAt", &value) &&
           !json_object_is_type(value, json_type_null) && timestamp_ms(entry, "resetsAt", reset);
}

static gboolean starts_reset_segment(json_object *previous, json_object *current) {
    gint64 previous_reset = 0;
    gint64 current_reset = 0;
    return entry_reset(previous, &previous_reset) && entry_reset(current, &current_reset) &&
           llabs(previous_reset - current_reset) >= HISTORY_RESET_TOLERANCE_MS;
}

static json_object *segment_peak(json_object *first, json_object *second) {
    double first_used = json_object_get_double(json_object_object_get(first, "usedPercent"));
    double second_used = json_object_get_double(json_object_object_get(second, "usedPercent"));
    gint64 first_time = 0;
    gint64 second_time = 0;
    timestamp_ms(first, "capturedAt", &first_time);
    timestamp_ms(second, "capturedAt", &second_time);
    if (second_used > first_used || (second_used == first_used && second_time >= first_time)) return second;
    return first;
}

static void json_object_release(gpointer object) {
    json_object_put(object);
}

static void replace_hour_entries(json_object *entries, const HistorySample *sample) {
    gint64 bucket = sample->captured_at_ms / HISTORY_HOUR_MS;
    GPtrArray *all = g_ptr_array_new_with_free_func(json_object_release);
    GPtrArray *hour = g_ptr_array_new_with_free_func(json_object_release);
    for (size_t index = 0; index < json_object_array_length(entries); index++) {
        json_object *entry = json_object_array_get_idx(entries, index);
        gint64 captured = 0;
        if (!timestamp_ms(entry, "capturedAt", &captured)) continue;
        if (captured / HISTORY_HOUR_MS == bucket) {
            g_ptr_array_add(hour, json_object_get(entry));
        } else {
            g_ptr_array_add(all, json_object_get(entry));
        }
    }
    g_ptr_array_add(hour, entry_new(sample));
    g_ptr_array_sort(hour, entry_compare);
    json_object *before_reset = NULL;
    json_object *active_peak = NULL;
    for (guint index = 0; index < hour->len; index++) {
        json_object *entry = g_ptr_array_index(hour, index);
        if (!active_peak) {
            active_peak = entry;
        } else if (starts_reset_segment(active_peak, entry)) {
            if (!before_reset) before_reset = active_peak;
            active_peak = entry;
        } else {
            active_peak = segment_peak(active_peak, entry);
        }
    }
    if (before_reset) g_ptr_array_add(all, json_object_get(before_reset));
    if (active_peak) g_ptr_array_add(all, json_object_get(active_peak));
    g_ptr_array_sort(all, entry_compare);
    while (json_object_array_length(entries) > 0) json_object_array_del_idx(entries, 0, 1);
    guint start = all->len > HISTORY_MAX_SAMPLES ? all->len - HISTORY_MAX_SAMPLES : 0;
    for (guint index = start; index < all->len; index++) {
        json_object_array_add(entries, json_object_get(g_ptr_array_index(all, index)));
    }
    g_ptr_array_unref(hour);
    g_ptr_array_unref(all);
}

static gint64 canonical_minutes(const char *name, gint64 minutes) {
    if (g_str_equal(name, "session") && minutes >= 295 && minutes <= 305) return 300;
    if (g_str_equal(name, "weekly") && minutes >= 10070 && minutes <= 10090) return 10080;
    return minutes;
}

static json_object *series_entries(json_object *histories, const HistorySample *sample) {
    gint64 minutes = canonical_minutes(sample->name, sample->window_minutes);
    json_object *selected = NULL;
    json_object *selected_entries = NULL;
    for (size_t index = 0; index < json_object_array_length(histories);) {
        json_object *series = json_object_array_get_idx(histories, index);
        json_object *name = NULL;
        json_object *window = NULL;
        json_object *entries = NULL;
        if (json_object_object_get_ex(series, "name", &name) &&
            json_object_is_type(name, json_type_string) &&
            g_str_equal(json_object_get_string(name), sample->name) &&
            json_object_object_get_ex(series, "windowMinutes", &window) &&
            json_object_is_type(window, json_type_int) &&
            canonical_minutes(sample->name, json_object_get_int64(window)) == minutes &&
            json_object_object_get_ex(series, "entries", &entries) &&
            json_object_is_type(entries, json_type_array)) {
            if (!selected) {
                selected = series;
                selected_entries = entries;
                json_object_object_add(selected, "windowMinutes", json_object_new_int64(minutes));
                index++;
                continue;
            }
            for (size_t entry_index = 0; entry_index < json_object_array_length(entries); entry_index++) {
                json_object *candidate = json_object_array_get_idx(entries, entry_index);
                gboolean duplicate = FALSE;
                for (size_t existing_index = 0;
                     existing_index < json_object_array_length(selected_entries);
                     existing_index++) {
                    if (json_object_equal(candidate,
                                          json_object_array_get_idx(selected_entries, existing_index))) {
                        duplicate = TRUE;
                        break;
                    }
                }
                if (!duplicate) json_object_array_add(selected_entries, json_object_get(candidate));
            }
            json_object_array_del_idx(histories, index, 1);
            continue;
        }
        index++;
    }
    if (selected_entries) return selected_entries;
    json_object *series = json_object_new_object();
    json_object *entries = json_object_new_array();
    json_object_object_add(series, "name", json_object_new_string(sample->name));
    json_object_object_add(series, "windowMinutes", json_object_new_int64(minutes));
    json_object_object_add(series, "entries", entries);
    json_object_array_add(histories, series);
    return entries;
}

static char *normalized_value(const char *value, gboolean casefold) {
    if (!value) return NULL;
    char *normalized = casefold ? g_utf8_casefold(value, -1) : g_strdup(value);
    g_strstrip(normalized);
    if (normalized[0] == '\0') {
        g_free(normalized);
        return NULL;
    }
    return normalized;
}

static char *hashed_account_key(const char *material) {
    return g_compute_checksum_for_string(G_CHECKSUM_SHA256, material, -1);
}

static char *account_key(const CodexBarProvider *provider) {
    char *provider_account = normalized_value(
        provider->identity ? provider->identity->account_id : NULL, FALSE);
    char *email = normalized_value(provider->account, TRUE);
    if (g_str_equal(provider->provider, "codex")) {
        char *key = provider_account
                        ? g_strdup_printf("codex:v1:provider-account:%s", provider_account)
                        : NULL;
        if (!key && email) {
            char *digest = hashed_account_key(email);
            key = g_strdup_printf("codex:v1:email-hash:%s", digest);
            g_free(digest);
        }
        g_free(provider_account);
        g_free(email);
        return key;
    }

    char *material = NULL;
    if (email && g_str_equal(provider->provider, "claude")) {
        char *organization = normalized_value(
            provider->identity ? provider->identity->organization : NULL, TRUE);
        char *login_method = normalized_value(
            provider->identity ? provider->identity->login_method : NULL, TRUE);
        const char *discriminator = organization ? organization : login_method;
        const char *kind = organization ? "org" : "plan";
        material = discriminator
                       ? g_strdup_printf("claude:email:%s:%s:%s", email, kind, discriminator)
                       : g_strdup_printf("claude:email:%s", email);
        g_free(login_method);
        g_free(organization);
    } else if (email) {
        material = g_strdup_printf("%s:email:%s", provider->provider, email);
    } else {
        char *organization = normalized_value(
            provider->identity ? provider->identity->organization : NULL, TRUE);
        if (organization && !g_str_equal(provider->provider, "claude")) {
            material = g_strdup_printf("%s:organization:%s", provider->provider, organization);
        }
        g_free(organization);
    }
    char *key = material ? hashed_account_key(material) : NULL;
    g_free(material);
    g_free(provider_account);
    g_free(email);
    return key;
}

static json_object *history_bucket(json_object *document, const char *key) {
    if (!key) return json_object_object_get(document, "unscoped");
    json_object *accounts = json_object_object_get(document, "accounts");
    json_object *histories = NULL;
    if (!json_object_object_get_ex(accounts, key, &histories) ||
        !json_object_is_type(histories, json_type_array)) {
        histories = json_object_new_array();
        json_object_object_add(accounts, key, histories);
    }
    json_object_object_add(document, "preferredAccountKey", json_object_new_string(key));
    return histories;
}

static const char *series_name(const CodexBarProvider *provider,
                               const CodexBarQuotaWindow *window,
                               guint index) {
    if (g_str_equal(provider->provider, "claude")) {
        const char *names[] = {"session", "weekly", "opus"};
        return index < G_N_ELEMENTS(names) ? names[index] : NULL;
    }
    if (g_str_equal(provider->provider, "opencodego")) {
        const char *names[] = {"session", "weekly", "monthly"};
        return index < G_N_ELEMENTS(names) ? names[index] : NULL;
    }
    if (g_str_equal(provider->provider, "codex")) {
        const char *names[] = {"session", "weekly"};
        return index < G_N_ELEMENTS(names) ? names[index] : NULL;
    }
    if (!window->has_window_minutes) return NULL;
    if (window->window_minutes > 0 && window->window_minutes <= 360) return "session";
    if (window->window_minutes >= 10070 && window->window_minutes <= 10090) return "weekly";
    return NULL;
}

static gboolean record_provider(json_object *document,
                                const CodexBarProvider *provider,
                                gint64 captured_at_ms) {
    if (provider->error || !provider->quota_windows || provider->quota_windows->len == 0) return FALSE;
    char *key = account_key(provider);
    json_object *histories = history_bucket(document, key);
    gboolean changed = FALSE;
    for (guint index = 0; index < provider->quota_windows->len; index++) {
        CodexBarQuotaWindow *window = g_ptr_array_index(provider->quota_windows, index);
        const char *name = series_name(provider, window, index);
        if (!name || !window->usage_known || !window->has_window_minutes || window->window_minutes <= 0 ||
            !isfinite(window->used_percent)) {
            continue;
        }
        HistorySample sample = {
            .name = name,
            .window_minutes = window->window_minutes,
            .captured_at_ms = captured_at_ms,
            .used_percent = window->used_percent,
            .has_reset = window->has_resets_at,
            .reset_at_ms = window->resets_at_ms,
        };
        replace_hour_entries(series_entries(histories, &sample), &sample);
        changed = TRUE;
    }
    g_free(key);
    return changed;
}

static gboolean atomic_write(const char *path, json_object *document, GError **error) {
    const char *contents = json_object_to_json_string_ext(document, JSON_C_TO_STRING_PLAIN);
    GFile *file = g_file_new_for_path(path);
    gboolean written = g_file_replace_contents(file,
                                               contents,
                                               strlen(contents),
                                               NULL,
                                               FALSE,
                                               G_FILE_CREATE_PRIVATE | G_FILE_CREATE_REPLACE_DESTINATION,
                                               NULL,
                                               NULL,
                                               error);
    g_object_unref(file);
    return written;
}

gboolean codexbar_history_store_record(CodexBarHistoryStore *store,
                                       const CodexBarSnapshot *snapshot,
                                       gint64 captured_at_ms,
                                       gboolean historical_tracking_enabled,
                                       GError **error) {
    g_return_val_if_fail(store != NULL, FALSE);
    g_return_val_if_fail(snapshot != NULL, FALSE);
    if (g_mkdir_with_parents(store->directory, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno),
                    "Cannot create history directory: %s", g_strerror(errno));
        return FALSE;
    }
    gboolean success = TRUE;
    g_mutex_lock(&store->lock);
    for (guint index = 0; index < snapshot->providers->len; index++) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        if (!valid_provider_id(provider->provider)) continue;
        if (!historical_tracking_enabled && !g_str_equal(provider->provider, "codex") &&
            !g_str_equal(provider->provider, "claude")) {
            continue;
        }
        char *path = provider_path(store, provider->provider);
        GError *provider_error = NULL;
        json_object *document = load_path(path, TRUE, &provider_error);
        if (!document) {
            g_clear_error(&provider_error);
            document = empty_document();
        }
        if (record_provider(document, provider, captured_at_ms) &&
            !atomic_write(path, document, &provider_error)) {
            success = FALSE;
            if (error && !*error) *error = provider_error;
            else g_clear_error(&provider_error);
        }
        json_object_put(document);
        g_free(path);
    }
    g_mutex_unlock(&store->lock);
    return success;
}

json_object *codexbar_history_store_load_all(CodexBarHistoryStore *store) {
    g_return_val_if_fail(store != NULL, NULL);
    json_object *providers = json_object_new_object();
    g_mutex_lock(&store->lock);
    guint count = codexbar_provider_registry_count();
    for (guint index = 0; index < count; index++) {
        const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_at(index);
        char *path = provider_path(store, descriptor->id);
        json_object *document = load_path(path, FALSE, NULL);
        if (document) json_object_object_add(providers, descriptor->id, document);
        g_free(path);
    }
    g_mutex_unlock(&store->lock);
    json_object *output = json_object_new_object();
    json_object_object_add(output, "version", json_object_new_int(HISTORY_SCHEMA_VERSION));
    json_object_object_add(output, "providers", providers);
    return output;
}
