#include "config.h"

#include "hooks.h"
#include "provider_registry.h"
#include "token_accounts.h"

#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <json-c/json.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    CODEXBAR_CONFIG_VERSION = 1,
};

static GQuark config_error_quark(void) {
    return g_quark_from_static_string("codexbar-config-error");
}

static char *clean_string(json_object *object, const char *key) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    const char *string = json_object_get_string(value);
    size_t string_length = (size_t)json_object_get_string_len(value);
    if (memchr(string, '\0', string_length)) return NULL;
    char *clean = g_strdup(string);
    g_strstrip(clean);
    size_t length = strlen(clean);
    if (length >= 2 && ((clean[0] == '"' && clean[length - 1] == '"') ||
                        (clean[0] == '\'' && clean[length - 1] == '\''))) {
        clean[length - 1] = '\0';
        memmove(clean, clean + 1, length - 1);
        g_strstrip(clean);
    }
    if (clean[0] == '\0') {
        g_free(clean);
        return NULL;
    }
    return clean;
}

static void provider_config_free(gpointer data) {
    CodexBarProviderConfig *provider = data;
    if (!provider) return;
    g_free(provider->id);
    g_free(provider->source);
    g_free(provider->api_key);
    g_free(provider->secret_key);
    g_free(provider->region);
    g_free(provider->workspace_id);
    g_free(provider->enterprise_host);
    g_free(provider->aws_profile);
    g_free(provider->aws_auth_mode);
    g_free(provider->codex_active_account_id);
    g_free(provider->codex_active_home_path);
    if (provider->codex_profile_home_paths) g_ptr_array_unref(provider->codex_profile_home_paths);
    if (provider->raw) json_object_put(provider->raw);
    g_free(provider);
}

char *codexbar_config_resolve_path(void) {
    const char *override = g_getenv("CODEXBAR_CONFIG");
    if (override && override[0] != '\0') {
        char *clean = g_strdup(override);
        g_strstrip(clean);
        char *path = g_str_equal(clean, "~")
                         ? g_strdup(g_get_home_dir())
                         : g_str_has_prefix(clean, "~/") ? g_build_filename(g_get_home_dir(), clean + 2, NULL)
                                                          : g_canonicalize_filename(clean, NULL);
        g_free(clean);
        return path;
    }
    const char *xdg = g_getenv("XDG_CONFIG_HOME");
    char *clean_xdg = xdg ? g_strdup(xdg) : NULL;
    if (clean_xdg) g_strstrip(clean_xdg);
    char *expanded_xdg = clean_xdg && g_str_has_prefix(clean_xdg, "~/")
                             ? g_build_filename(g_get_home_dir(), clean_xdg + 2, NULL)
                             : g_strdup(clean_xdg);
    char *xdg_path = expanded_xdg && g_path_is_absolute(expanded_xdg)
                         ? g_build_filename(expanded_xdg, "codexbar", "config.json", NULL)
                         : g_build_filename(g_get_home_dir(), ".config", "codexbar", "config.json", NULL);
    g_free(expanded_xdg);
    g_free(clean_xdg);
    if (g_file_test(xdg_path, G_FILE_TEST_EXISTS)) return xdg_path;
    char *legacy = g_build_filename(g_get_home_dir(), ".codexbar", "config.json", NULL);
    if (g_file_test(legacy, G_FILE_TEST_EXISTS)) {
        g_free(xdg_path);
        return legacy;
    }
    g_free(legacy);
    return xdg_path;
}

static CodexBarProviderConfig *provider_config_new(const CodexBarProviderDescriptor *descriptor,
                                                   gboolean existing_config) {
    CodexBarProviderConfig *provider = g_new0(CodexBarProviderConfig, 1);
    provider->id = g_strdup(descriptor->id);
    provider->enabled = descriptor->default_enabled;
    provider->has_enabled = TRUE;
    provider->codex_profile_home_paths = g_ptr_array_new_with_free_func(g_free);
    if (g_str_equal(descriptor->id, "alibabatokenplan")) {
        provider->region = g_strdup(existing_config ? "cn" : "intl");
    }
    return provider;
}

static CodexBarProviderConfig *parse_provider(json_object *entry, GError **error) {
    char *id = clean_string(entry, "id");
    if (!id) {
        g_set_error_literal(error, config_error_quark(), 2, "Missing provider id");
        return NULL;
    }
    const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(id);
    if (!descriptor || !g_str_equal(id, descriptor->id)) {
        g_free(id);
        return NULL;
    }
    CodexBarProviderConfig *provider = g_new0(CodexBarProviderConfig, 1);
    provider->id = id;
    provider->codex_profile_home_paths = g_ptr_array_new_with_free_func(g_free);
    provider->enabled = descriptor->default_enabled;
    json_object *enabled = NULL;
    if (json_object_object_get_ex(entry, "enabled", &enabled) && json_object_is_type(enabled, json_type_boolean)) {
        provider->has_enabled = TRUE;
        provider->enabled = json_object_get_boolean(enabled);
    }
    provider->source = clean_string(entry, "source");
    json_object *extras_enabled = NULL;
    if (json_object_object_get_ex(entry, "extrasEnabled", &extras_enabled) &&
        json_object_is_type(extras_enabled, json_type_boolean)) {
        provider->has_extras_enabled = TRUE;
        provider->extras_enabled = json_object_get_boolean(extras_enabled);
    }
    provider->api_key = clean_string(entry, "apiKey");
    provider->secret_key = clean_string(entry, "secretKey");
    provider->region = clean_string(entry, "region");
    provider->workspace_id = clean_string(entry, "workspaceID");
    provider->enterprise_host = clean_string(entry, "enterpriseHost");
    provider->aws_profile = clean_string(entry, "awsProfile");
    provider->aws_auth_mode = clean_string(entry, "awsAuthMode");
    if (g_str_equal(provider->id, "codex")) {
        json_object *active = NULL;
        if (json_object_object_get_ex(entry, "codexActiveSource", &active)) {
            json_object *kind = NULL;
            json_object *account_id = NULL;
            if (!json_object_is_type(active, json_type_object) ||
                !json_object_object_get_ex(active, "kind", &kind) ||
                !json_object_is_type(kind, json_type_string)) {
                g_set_error_literal(error, config_error_quark(), 8, "codexActiveSource is malformed");
                provider_config_free(provider);
                return NULL;
            }
            const char *kind_name = json_object_get_string(kind);
            provider->has_codex_active_source = TRUE;
            if (g_str_equal(kind_name, "managedAccount")) {
                if (!json_object_object_get_ex(active, "accountID", &account_id) ||
                    !json_object_is_type(account_id, json_type_string) ||
                    !g_uuid_string_is_valid(json_object_get_string(account_id))) {
                    g_set_error_literal(error, config_error_quark(), 8,
                                        "codexActiveSource managedAccount requires a valid accountID");
                    provider_config_free(provider);
                    return NULL;
                }
                provider->codex_active_source = CODEXBAR_CODEX_SOURCE_MANAGED_ACCOUNT;
                provider->codex_active_account_id = g_ascii_strdown(json_object_get_string(account_id), -1);
            } else if (g_str_equal(kind_name, "liveSystem") || g_str_equal(kind_name, "profileHome")) {
                char *path = clean_string(active, "homePath");
                if (path) {
                    provider->codex_active_source = CODEXBAR_CODEX_SOURCE_PROFILE_HOME;
                    provider->codex_active_home_path = path;
                } else {
                    provider->codex_active_source = CODEXBAR_CODEX_SOURCE_LIVE_SYSTEM;
                }
            } else {
                g_set_error(error, config_error_quark(), 8, "Unknown codexActiveSource kind: %s", kind_name);
                provider_config_free(provider);
                return NULL;
            }
        }
        json_object *profile_paths = NULL;
        if (json_object_object_get_ex(entry, "codexProfileHomePaths", &profile_paths)) {
            if (!json_object_is_type(profile_paths, json_type_array)) {
                g_set_error_literal(error, config_error_quark(), 8, "codexProfileHomePaths must be an array");
                provider_config_free(provider);
                return NULL;
            }
            provider->has_codex_profile_home_paths = TRUE;
            for (size_t index = 0; index < json_object_array_length(profile_paths); index++) {
                json_object *path_value = json_object_array_get_idx(profile_paths, index);
                if (!json_object_is_type(path_value, json_type_string)) {
                    g_set_error_literal(error, config_error_quark(), 8,
                                        "codexProfileHomePaths entries must be strings");
                    provider_config_free(provider);
                    return NULL;
                }
                char *path = g_strstrip(g_strdup(json_object_get_string(path_value)));
                if (path[0] != '\0' && g_path_is_absolute(path)) {
                    char *canonical = g_canonicalize_filename(path, NULL);
                    if (!g_ptr_array_find_with_equal_func(provider->codex_profile_home_paths,
                                                          canonical,
                                                          (GEqualFunc)g_str_equal,
                                                          NULL)) {
                        g_ptr_array_add(provider->codex_profile_home_paths, canonical);
                    } else {
                        g_free(canonical);
                    }
                }
                g_free(path);
            }
        }
    }
    provider->raw = json_object_get(entry);
    return provider;
}

static CodexBarConfig *config_new(const char *path) {
    CodexBarConfig *config = g_new0(CodexBarConfig, 1);
    config->version = CODEXBAR_CONFIG_VERSION;
    config->path = g_strdup(path);
    config->providers = g_ptr_array_new_with_free_func(provider_config_free);
    config->refresh_frequency = CODEXBAR_REFRESH_ADAPTIVE;
    config->agent_sessions_enabled = FALSE;
    config->agent_sessions_manual_hosts = g_strdup("");
    config->quota_warning_notifications_enabled = FALSE;
    config->quota_warning_session_enabled = TRUE;
    config->quota_warning_weekly_enabled = TRUE;
    config->quota_warning_session_threshold_count = 2;
    config->quota_warning_weekly_threshold_count = 2;
    config->quota_warning_session_thresholds[0] = 50;
    config->quota_warning_session_thresholds[1] = 20;
    config->quota_warning_weekly_thresholds[0] = 50;
    config->quota_warning_weekly_thresholds[1] = 20;
    config->historical_tracking_enabled = FALSE;
    config->lock_fd = -1;
    return config;
}

static gboolean boolean_member(json_object *object, const char *key, gboolean fallback) {
    json_object *value = NULL;
    return object && json_object_object_get_ex(object, key, &value) &&
                   json_object_is_type(value, json_type_boolean)
               ? json_object_get_boolean(value)
               : fallback;
}

static gint descending_int(gconstpointer left, gconstpointer right) {
    int first = *(const int *)left;
    int second = *(const int *)right;
    return (second > first) - (second < first);
}

static void parse_thresholds(json_object *root, const char *key, int values[100], guint *count) {
    json_object *array = NULL;
    if (!json_object_object_get_ex(root, key, &array) || !json_object_is_type(array, json_type_array) ||
        json_object_array_length(array) == 0) {
        return;
    }
    gboolean seen[100] = {0};
    guint parsed_count = 0;
    for (size_t index = 0; index < json_object_array_length(array); index++) {
        json_object *entry = json_object_array_get_idx(array, index);
        if (!json_object_is_type(entry, json_type_int)) continue;
        int threshold = CLAMP(json_object_get_int(entry), 0, 99);
        if (seen[threshold]) continue;
        seen[threshold] = TRUE;
        values[parsed_count++] = threshold;
    }
    if (parsed_count == 0) return;
    qsort(values, parsed_count, sizeof(values[0]), descending_int);
    *count = parsed_count;
}

static gboolean acquire_config_lock(CodexBarConfig *config, GError **error) {
    const char *runtime = g_get_user_runtime_dir();
    if (!runtime) runtime = g_get_user_cache_dir();
    char *directory = g_build_filename(runtime, "codexbar", NULL);
    gboolean existed = g_file_test(directory, G_FILE_TEST_IS_DIR);
    if (g_mkdir_with_parents(directory, 0700) != 0 || (!existed && g_chmod(directory, 0700) != 0)) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not create config lock directory: %s",
                    g_strerror(errno));
        g_free(directory);
        return FALSE;
    }
    char *digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, config->path, -1);
    char *filename = g_strdup_printf("%s.lock", digest);
    char *lock_path = g_build_filename(directory, filename, NULL);
    g_free(filename);
    g_free(digest);
    g_free(directory);
    config->lock_fd = g_open(lock_path, O_RDWR | O_CREAT, 0600);
    g_free(lock_path);
    if (config->lock_fd < 0 || flock(config->lock_fd, LOCK_EX) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not lock config: %s", g_strerror(errno));
        if (config->lock_fd >= 0) close(config->lock_fd);
        config->lock_fd = -1;
        return FALSE;
    }
    (void)fcntl(config->lock_fd, F_SETFD, FD_CLOEXEC);
    return TRUE;
}

static void append_missing_providers(CodexBarConfig *config, GHashTable *seen, gboolean existing_config) {
    for (guint index = 0; index < codexbar_provider_registry_count(); index++) {
        const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_at(index);
        if (g_hash_table_contains(seen, descriptor->id)) continue;
        g_ptr_array_add(config->providers, provider_config_new(descriptor, existing_config));
    }
}

static CodexBarConfig *config_load(gboolean for_update, GError **error) {
    char *path = codexbar_config_resolve_path();
    CodexBarConfig *config = config_new(path);
    g_free(path);
    if (for_update && !acquire_config_lock(config, error)) {
        codexbar_config_free(config);
        return NULL;
    }
    GHashTable *seen = g_hash_table_new(g_str_hash, g_str_equal);
    if (!g_file_test(config->path, G_FILE_TEST_EXISTS)) {
        append_missing_providers(config, seen, FALSE);
        g_hash_table_unref(seen);
        return config;
    }

    char *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(config->path, &contents, &length, error)) {
        g_hash_table_unref(seen);
        codexbar_config_free(config);
        return NULL;
    }
    json_tokener *tokener = json_tokener_new();
    json_object *root = json_tokener_parse_ex(tokener, contents, (int)length);
    enum json_tokener_error code = json_tokener_get_error(tokener);
    config->loaded_from_disk = TRUE;
    config->loaded_digest = g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)contents, length);
    g_free(contents);
    json_tokener_free(tokener);
    if (code != json_tokener_success || !root || !json_object_is_type(root, json_type_object)) {
        g_set_error(error, config_error_quark(), 1, "Invalid config JSON: %s", json_tokener_error_desc(code));
        if (root) json_object_put(root);
        g_hash_table_unref(seen);
        codexbar_config_free(config);
        return NULL;
    }
    config->raw = root;
    json_object *refresh_frequency = NULL;
    const char *refresh_raw = NULL;
    if (json_object_object_get_ex(root, "refreshFrequency", &refresh_frequency) &&
        json_object_is_type(refresh_frequency, json_type_string)) {
        refresh_raw = json_object_get_string(refresh_frequency);
    }
    config->refresh_frequency = codexbar_refresh_frequency_parse(refresh_raw, TRUE);
    json_object *sessions_enabled = NULL;
    if (json_object_object_get_ex(root, "agentSessionsEnabled", &sessions_enabled) &&
        json_object_is_type(sessions_enabled, json_type_boolean)) {
        config->agent_sessions_enabled = json_object_get_boolean(sessions_enabled);
    }
    char *manual_hosts = clean_string(root, "agentSessionsManualHosts");
    if (manual_hosts) {
        g_free(config->agent_sessions_manual_hosts);
        config->agent_sessions_manual_hosts = manual_hosts;
    }
    config->quota_warning_notifications_enabled = boolean_member(
        root, "quotaWarningNotificationsEnabled", config->quota_warning_notifications_enabled);
    config->quota_warning_session_enabled = boolean_member(
        root, "quotaWarningSessionEnabled", config->quota_warning_session_enabled);
    config->quota_warning_weekly_enabled = boolean_member(
        root, "quotaWarningWeeklyEnabled", config->quota_warning_weekly_enabled);
    parse_thresholds(root,
                     "quotaWarningSessionThresholds",
                     config->quota_warning_session_thresholds,
                     &config->quota_warning_session_threshold_count);
    parse_thresholds(root,
                     "quotaWarningWeeklyThresholds",
                     config->quota_warning_weekly_thresholds,
                     &config->quota_warning_weekly_threshold_count);
    config->historical_tracking_enabled = boolean_member(
        root, "historicalTrackingEnabled", config->historical_tracking_enabled);
    json_object *version = NULL;
    if (json_object_object_get_ex(root, "version", &version) && json_object_is_type(version, json_type_int)) {
        config->version = json_object_get_int(version);
    }

    json_object *providers = NULL;
    if (json_object_object_get_ex(root, "providers", &providers) && json_object_is_type(providers, json_type_array)) {
        size_t count = json_object_array_length(providers);
        for (size_t index = 0; index < count; index++) {
            json_object *entry = json_object_array_get_idx(providers, index);
            if (!json_object_is_type(entry, json_type_object)) continue;
            GError *provider_error = NULL;
            CodexBarProviderConfig *provider = parse_provider(entry, &provider_error);
            if (!provider) {
                if (provider_error) {
                    g_propagate_error(error, provider_error);
                    g_hash_table_unref(seen);
                    codexbar_config_free(config);
                    return NULL;
                }
                continue;
            }
            if (g_hash_table_contains(seen, provider->id)) {
                provider_config_free(provider);
                continue;
            }
            g_hash_table_add(seen, provider->id);
            g_ptr_array_add(config->providers, provider);
        }
    }
    append_missing_providers(config, seen, TRUE);
    g_hash_table_unref(seen);
    return config;
}

CodexBarConfig *codexbar_config_load(GError **error) {
    return config_load(FALSE, error);
}

CodexBarConfig *codexbar_config_load_for_update(GError **error) {
    return config_load(TRUE, error);
}

static json_object *clone_object(json_object *object) {
    if (!object) return json_object_new_object();
    return json_tokener_parse(json_object_to_json_string_ext(object, JSON_C_TO_STRING_PLAIN));
}

static void set_optional_string(json_object *object, const char *key, const char *value) {
    if (value) json_object_object_add(object, key, json_object_new_string(value));
}

static json_object *serialize_provider(const CodexBarProviderConfig *provider) {
    json_object *object = clone_object(provider->raw);
    json_object_object_add(object, "id", json_object_new_string(provider->id));
    if (provider->has_enabled) json_object_object_add(object, "enabled", json_object_new_boolean(provider->enabled));
    set_optional_string(object, "source", provider->source);
    if (provider->has_extras_enabled) {
        json_object_object_add(object, "extrasEnabled", json_object_new_boolean(provider->extras_enabled));
    }
    set_optional_string(object, "apiKey", provider->api_key);
    set_optional_string(object, "secretKey", provider->secret_key);
    set_optional_string(object, "region", provider->region);
    set_optional_string(object, "workspaceID", provider->workspace_id);
    set_optional_string(object, "enterpriseHost", provider->enterprise_host);
    set_optional_string(object, "awsProfile", provider->aws_profile);
    set_optional_string(object, "awsAuthMode", provider->aws_auth_mode);
    if (g_str_equal(provider->id, "codex") && provider->has_codex_active_source) {
        json_object *active = json_object_new_object();
        switch (provider->codex_active_source) {
        case CODEXBAR_CODEX_SOURCE_MANAGED_ACCOUNT:
            json_object_object_add(active, "kind", json_object_new_string("managedAccount"));
            json_object_object_add(active,
                                   "accountID",
                                   json_object_new_string(provider->codex_active_account_id));
            break;
        case CODEXBAR_CODEX_SOURCE_PROFILE_HOME:
            json_object_object_add(active, "kind", json_object_new_string("liveSystem"));
            json_object_object_add(active,
                                   "homePath",
                                   json_object_new_string(provider->codex_active_home_path));
            break;
        case CODEXBAR_CODEX_SOURCE_LIVE_SYSTEM:
            json_object_object_add(active, "kind", json_object_new_string("liveSystem"));
            break;
        }
        json_object_object_add(object, "codexActiveSource", active);
    }
    if (g_str_equal(provider->id, "codex") && provider->has_codex_profile_home_paths) {
        json_object *paths = json_object_new_array_ext((int)provider->codex_profile_home_paths->len);
        for (guint index = 0; index < provider->codex_profile_home_paths->len; index++) {
            json_object_array_add(paths,
                                  json_object_new_string(g_ptr_array_index(provider->codex_profile_home_paths, index)));
        }
        json_object_object_add(object, "codexProfileHomePaths", paths);
    }
    return object;
}

static json_object *serialize_config(const CodexBarConfig *config) {
    json_object *root = clone_object(config->raw);
    json_object_object_add(root, "version", json_object_new_int(CODEXBAR_CONFIG_VERSION));
    json_object_object_add(
        root, "refreshFrequency", json_object_new_string(codexbar_refresh_frequency_raw(config->refresh_frequency)));
    json_object_object_add(
        root, "agentSessionsEnabled", json_object_new_boolean(config->agent_sessions_enabled));
    json_object_object_add(
        root, "agentSessionsManualHosts", json_object_new_string(config->agent_sessions_manual_hosts));
    json_object_object_add(root,
                           "quotaWarningNotificationsEnabled",
                           json_object_new_boolean(config->quota_warning_notifications_enabled));
    json_object_object_add(root,
                           "quotaWarningSessionEnabled",
                           json_object_new_boolean(config->quota_warning_session_enabled));
    json_object_object_add(root,
                           "quotaWarningWeeklyEnabled",
                           json_object_new_boolean(config->quota_warning_weekly_enabled));
    json_object *session_thresholds = json_object_new_array_ext(
        (int)config->quota_warning_session_threshold_count);
    for (guint index = 0; index < config->quota_warning_session_threshold_count; index++) {
        json_object_array_add(session_thresholds,
                              json_object_new_int(config->quota_warning_session_thresholds[index]));
    }
    json_object_object_add(root, "quotaWarningSessionThresholds", session_thresholds);
    json_object *weekly_thresholds = json_object_new_array_ext(
        (int)config->quota_warning_weekly_threshold_count);
    for (guint index = 0; index < config->quota_warning_weekly_threshold_count; index++) {
        json_object_array_add(weekly_thresholds,
                              json_object_new_int(config->quota_warning_weekly_thresholds[index]));
    }
    json_object_object_add(root, "quotaWarningWeeklyThresholds", weekly_thresholds);
    json_object_object_add(root,
                           "historicalTrackingEnabled",
                           json_object_new_boolean(config->historical_tracking_enabled));
    json_object *providers = json_object_new_array_ext((int)config->providers->len);
    for (guint index = 0; index < config->providers->len; index++) {
        json_object_array_add(providers, serialize_provider(g_ptr_array_index(config->providers, index)));
    }
    json_object_object_add(root, "providers", providers);
    return root;
}

char *codexbar_config_render_json(const CodexBarConfig *config, gboolean pretty) {
    g_return_val_if_fail(config != NULL, NULL);
    json_object *root = serialize_config(config);
    char *result = g_strdup(json_object_to_json_string_ext(
        root, pretty ? JSON_C_TO_STRING_PRETTY : JSON_C_TO_STRING_PLAIN));
    json_object_put(root);
    return result;
}

gboolean codexbar_config_save(CodexBarConfig *config, GError **error) {
    g_return_val_if_fail(config != NULL, FALSE);
    if (config->lock_fd < 0) {
        g_set_error_literal(error, config_error_quark(), 7, "Config must be loaded for update before saving");
        return FALSE;
    }
    json_object *root = serialize_config(config);
    const char *json = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
    char *contents = g_strconcat(json, "\n", NULL);

    char *current = NULL;
    gsize current_length = 0;
    gboolean exists = g_file_get_contents(config->path, &current, &current_length, NULL);
    char *current_digest = exists
                               ? g_compute_checksum_for_data(
                                     G_CHECKSUM_SHA256, (const guchar *)current, current_length)
                               : NULL;
    g_free(current);
    gboolean changed = exists != config->loaded_from_disk ||
                       (exists && g_strcmp0(current_digest, config->loaded_digest) != 0);
    g_free(current_digest);
    if (changed) {
        g_set_error(error,
                    config_error_quark(),
                    6,
                    "Config changed on disk while it was being edited: %s",
                    config->path);
        g_free(contents);
        json_object_put(root);
        return FALSE;
    }

    char *directory = g_path_get_dirname(config->path);
    gboolean directory_existed = g_file_test(directory, G_FILE_TEST_IS_DIR);
    if (g_mkdir_with_parents(directory, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not create %s: %s", directory,
                    g_strerror(errno));
        g_free(directory);
        g_free(contents);
        json_object_put(root);
        return FALSE;
    }
    if (!directory_existed && g_chmod(directory, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not secure %s: %s", directory,
                    g_strerror(errno));
        g_free(directory);
        g_free(contents);
        json_object_put(root);
        return FALSE;
    }
    gboolean saved = g_file_set_contents_full(config->path,
                                               contents,
                                               -1,
                                               G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
                                               0600,
                                               error);
    if (saved && g_chmod(config->path, 0600) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not secure %s: %s", config->path,
                    g_strerror(errno));
        saved = FALSE;
    }
    if (saved) {
        g_free(config->loaded_digest);
        config->loaded_digest = g_compute_checksum_for_string(G_CHECKSUM_SHA256, contents, -1);
        config->loaded_from_disk = TRUE;
    }
    g_free(directory);
    g_free(contents);
    json_object_put(root);
    return saved;
}

void codexbar_config_issue_free(CodexBarConfigIssue *issue) {
    if (!issue) return;
    g_free(issue->provider);
    g_free(issue->field);
    g_free(issue->code);
    g_free(issue->message);
    g_free(issue);
}

static void add_issue(GPtrArray *issues,
                      gboolean error,
                      const char *provider,
                      const char *field,
                      const char *code,
                      const char *format,
                      ...) G_GNUC_PRINTF(6, 7);

static void add_issue(GPtrArray *issues,
                      gboolean error,
                      const char *provider,
                      const char *field,
                      const char *code,
                      const char *format,
                      ...) {
    va_list arguments;
    va_start(arguments, format);
    CodexBarConfigIssue *issue = g_new0(CodexBarConfigIssue, 1);
    issue->error = error;
    issue->provider = g_strdup(provider);
    issue->field = g_strdup(field);
    issue->code = g_strdup(code);
    issue->message = g_strdup_vprintf(format, arguments);
    va_end(arguments);
    g_ptr_array_add(issues, issue);
}

static gboolean id_in(const char *id, const char *const *ids, guint count) {
    for (guint index = 0; index < count; index++) {
        if (g_str_equal(id, ids[index])) return TRUE;
    }
    return FALSE;
}

static gboolean value_in_csv(const char *values, const char *value) {
    char **parts = g_strsplit(values, ",", -1);
    gboolean found = FALSE;
    for (guint index = 0; parts[index]; index++) {
        if (g_str_equal(parts[index], value)) {
            found = TRUE;
            break;
        }
    }
    g_strfreev(parts);
    return found;
}

static gboolean hook_string(json_object *object, const char *key, const char **value, size_t *length) {
    json_object *entry = NULL;
    if (!json_object_object_get_ex(object, key, &entry) || !json_object_is_type(entry, json_type_string)) return FALSE;
    *value = json_object_get_string(entry);
    *length = (size_t)json_object_get_string_len(entry);
    return strlen(*value) == *length;
}

static void validate_hooks(const CodexBarConfig *config, GPtrArray *issues) {
    json_object *hooks = NULL;
    if (!config->raw || !json_object_object_get_ex(config->raw, "hooks", &hooks)) return;
    if (!json_object_is_type(hooks, json_type_object)) {
        add_issue(issues, TRUE, NULL, "hooks", "invalid_hooks", "Hooks must be an object.");
        return;
    }
    json_object *events = NULL;
    if (!json_object_object_get_ex(hooks, "events", &events)) return;
    if (!json_object_is_type(events, json_type_array)) {
        add_issue(issues, TRUE, NULL, "hooks.events", "invalid_hook_rules", "Hook events must be an array.");
        return;
    }
    size_t count = json_object_array_length(events);
    if (count > CODEXBAR_MAX_HOOK_RULES) {
        add_issue(issues,
                  TRUE,
                  NULL,
                  "hooks.events",
                  "too_many_hook_rules",
                  "Hooks support at most %d rules.",
                  CODEXBAR_MAX_HOOK_RULES);
    }
    GHashTable *ids = g_hash_table_new(g_str_hash, g_str_equal);
    for (size_t index = 0; index < count; index++) {
        json_object *rule = json_object_array_get_idx(events, index);
        char *field = g_strdup_printf("hooks.events[%zu]", index);
        if (!json_object_is_type(rule, json_type_object)) {
            add_issue(issues, TRUE, NULL, field, "invalid_hook_rule", "Hook rules must be objects.");
            g_free(field);
            continue;
        }
        const char *id = NULL;
        size_t id_length = 0;
        json_object *id_value = NULL;
        if (json_object_object_get_ex(rule, "id", &id_value)) {
            if (!hook_string(rule, "id", &id, &id_length) || id_length == 0 ||
                id_length > CODEXBAR_MAX_HOOK_ID_BYTES) {
                add_issue(issues,
                          TRUE,
                          NULL,
                          field,
                          "invalid_hook_command_size",
                          "Hook IDs, arguments, or aggregate command size exceed supported limits.");
            } else if (g_hash_table_contains(ids, id)) {
                add_issue(issues, TRUE, NULL, field, "duplicate_hook_id", "Hook rule IDs must be unique.");
            } else {
                g_hash_table_add(ids, (gpointer)id);
            }
        }
        const char *event = NULL;
        size_t unused = 0;
        char *event_field = g_strdup_printf("%s.event", field);
        if (!hook_string(rule, "event", &event, &unused) || !codexbar_hook_event_is_known(event)) {
            add_issue(issues, TRUE, NULL, event_field, "invalid_hook_event", "Hook event is not recognized.");
        }
        g_free(event_field);
        const char *executable = NULL;
        size_t executable_length = 0;
        char *executable_field = g_strdup_printf("%s.executable", field);
        if (!hook_string(rule, "executable", &executable, &executable_length) || executable_length == 0 ||
            executable_length > CODEXBAR_MAX_HOOK_STRING_BYTES || !g_path_is_absolute(executable)) {
            add_issue(issues,
                      TRUE,
                      NULL,
                      executable_field,
                      "invalid_hook_executable",
                      "Hook executables must use a non-empty absolute path.");
        }
        g_free(executable_field);
        json_object *provider = NULL;
        if (json_object_object_get_ex(rule, "provider", &provider) && !json_object_is_type(provider, json_type_null)) {
            const char *provider_name = json_object_is_type(provider, json_type_string)
                                            ? json_object_get_string(provider)
                                            : NULL;
            const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(provider_name);
            if (!descriptor || !g_str_equal(descriptor->id, provider_name)) {
                char *provider_field = g_strdup_printf("%s.provider", field);
                add_issue(issues,
                          TRUE,
                          NULL,
                          provider_field,
                          "invalid_hook_provider",
                          "Hook provider '%s' is not recognized.",
                          provider_name ? provider_name : "");
                g_free(provider_field);
            }
        }
        json_object *threshold = NULL;
        if (json_object_object_get_ex(rule, "threshold", &threshold) &&
            !json_object_is_type(threshold, json_type_null)) {
            double value = (json_object_is_type(threshold, json_type_double) ||
                            json_object_is_type(threshold, json_type_int))
                               ? json_object_get_double(threshold)
                               : NAN;
            if (!isfinite(value) || value <= 0 || value > 1) {
                char *threshold_field = g_strdup_printf("%s.threshold", field);
                add_issue(issues,
                          TRUE,
                          NULL,
                          threshold_field,
                          "invalid_hook_threshold",
                          "Hook thresholds must be greater than 0 and at most 1.");
                g_free(threshold_field);
            }
        }
        json_object *timeout = NULL;
        if (json_object_object_get_ex(rule, "timeoutSeconds", &timeout)) {
            double value = (json_object_is_type(timeout, json_type_double) ||
                            json_object_is_type(timeout, json_type_int))
                               ? json_object_get_double(timeout)
                               : NAN;
            if (!isfinite(value) || value < 0.1 || value > 300) {
                char *timeout_field = g_strdup_printf("%s.timeoutSeconds", field);
                add_issue(issues,
                          TRUE,
                          NULL,
                          timeout_field,
                          "invalid_hook_timeout",
                          "Hook timeouts must be between 0.1 and 300 seconds.");
                g_free(timeout_field);
            }
        }
        json_object *arguments = NULL;
        size_t command_bytes = executable_length;
        gboolean invalid_shape = FALSE;
        if (json_object_object_get_ex(rule, "arguments", &arguments)) {
            invalid_shape = !json_object_is_type(arguments, json_type_array) ||
                            json_object_array_length(arguments) > CODEXBAR_MAX_HOOK_ARGUMENTS;
            for (size_t argument_index = 0;
                 !invalid_shape && argument_index < json_object_array_length(arguments);
                 argument_index++) {
                json_object *argument = json_object_array_get_idx(arguments, argument_index);
                size_t length = json_object_is_type(argument, json_type_string)
                                    ? (size_t)json_object_get_string_len(argument)
                                    : CODEXBAR_MAX_HOOK_STRING_BYTES + 1;
                invalid_shape = length > CODEXBAR_MAX_HOOK_STRING_BYTES ||
                                (json_object_is_type(argument, json_type_string) &&
                                 strlen(json_object_get_string(argument)) != length) ||
                                length > CODEXBAR_MAX_HOOK_COMMAND_BYTES -
                                             MIN((size_t)CODEXBAR_MAX_HOOK_COMMAND_BYTES, command_bytes);
                command_bytes += MIN(length, (size_t)CODEXBAR_MAX_HOOK_COMMAND_BYTES);
            }
        }
        if (invalid_shape || command_bytes > CODEXBAR_MAX_HOOK_COMMAND_BYTES) {
            add_issue(issues,
                      TRUE,
                      NULL,
                      field,
                      "invalid_hook_command_size",
                      "Hook IDs, arguments, or aggregate command size exceed supported limits.");
        }
        g_free(field);
    }
    g_hash_table_unref(ids);
}

GPtrArray *codexbar_config_validate(const CodexBarConfig *config) {
    g_return_val_if_fail(config != NULL, NULL);
    GPtrArray *issues = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_config_issue_free);
    const char *workspace_providers[] = {
        "azureopenai", "openai", "opencode", "opencodego", "devin", "deepgram", "xai"};
    const char *host_providers[] = {
        "azureopenai", "clawrouter", "copilot", "kimi", "litellm", "llmproxy", "sub2api", "wayfinder"};
    if (config->version != CODEXBAR_CONFIG_VERSION) {
        add_issue(issues,
                  TRUE,
                  NULL,
                  "version",
                  "version_mismatch",
                  "Unsupported config version %d.",
                  config->version);
    }
    validate_hooks(config, issues);
    for (guint index = 0; index < config->providers->len; index++) {
        const CodexBarProviderConfig *provider = g_ptr_array_index(config->providers, index);
        const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(provider->id);
        if (provider->source && !codexbar_provider_supports_source(descriptor, provider->source)) {
            char *supported = codexbar_provider_supported_sources(descriptor);
            add_issue(issues,
                      TRUE,
                      provider->id,
                      "source",
                      "unsupported_source",
                      "Source %s is unavailable for %s on Linux. Supported sources: %s.",
                      provider->source,
                      provider->id,
                      supported);
            g_free(supported);
        }
        if (provider->api_key && !codexbar_provider_supports_source(descriptor, "api")) {
            add_issue(issues,
                      FALSE,
                      provider->id,
                      "apiKey",
                      "api_key_unused",
                      "apiKey is set but %s does not support api source.",
                      provider->id);
        }
        gboolean profile_backed_bedrock =
            g_str_equal(provider->id, "bedrock") &&
            ((provider->aws_profile && *provider->aws_profile) ||
             (provider->aws_auth_mode && g_ascii_strcasecmp(provider->aws_auth_mode, "profile") == 0));
        if (provider->source && g_str_equal(provider->source, "api") && !provider->api_key &&
            !g_str_equal(provider->id, "wayfinder") && !profile_backed_bedrock) {
            add_issue(issues,
                      FALSE,
                      provider->id,
                      "apiKey",
                      "api_key_missing",
                      "Source api is selected but apiKey is missing for %s.",
                      provider->id);
        }
        if (provider->secret_key && !g_str_equal(provider->id, "bedrock") && !g_str_equal(provider->id, "doubao")) {
            add_issue(issues,
                      FALSE,
                      provider->id,
                      "secretKey",
                      "secret_key_unused",
                      "secretKey is set but only bedrock and doubao use secretKey.");
        }
        if (provider->workspace_id && !id_in(provider->id, workspace_providers, G_N_ELEMENTS(workspace_providers))) {
            add_issue(issues,
                      FALSE,
                      provider->id,
                      "workspaceID",
                      "workspace_unused",
                      "workspaceID is not supported for %s.",
                      provider->id);
        }
        if (provider->enterprise_host && !id_in(provider->id, host_providers, G_N_ELEMENTS(host_providers))) {
            add_issue(issues,
                      FALSE,
                      provider->id,
                      "enterpriseHost",
                      "enterprise_host_unused",
                      "enterpriseHost is not supported for %s.",
                      provider->id);
        }
        if (provider->region) {
            const char *valid_regions = NULL;
            if (g_str_equal(provider->id, "minimax")) valid_regions = "global,cn";
            if (g_str_equal(provider->id, "zai")) valid_regions = "global,bigmodel-cn";
            if (g_str_equal(provider->id, "alibaba") || g_str_equal(provider->id, "alibabatokenplan")) {
                valid_regions = "intl,cn";
            }
            if (g_str_equal(provider->id, "moonshot")) valid_regions = "international,china";
            if (valid_regions && !value_in_csv(valid_regions, provider->region)) {
                add_issue(issues,
                          TRUE,
                          provider->id,
                          "region",
                          "invalid_region",
                          "Region %s is not valid for %s.",
                          provider->region,
                          provider->id);
            } else if (!valid_regions && !g_str_equal(provider->id, "bedrock") && !g_str_equal(provider->id, "doubao")) {
                add_issue(issues,
                          FALSE,
                          provider->id,
                          "region",
                          "region_unused",
                          "region is set but %s does not use regions.",
                          provider->id);
            }
        }
        if (g_str_equal(provider->id, "sub2api") && provider->enterprise_host) {
            GError *uri_error = NULL;
            GUri *uri = g_uri_parse(provider->enterprise_host, G_URI_FLAGS_NONE, &uri_error);
            const char *scheme = uri ? g_uri_get_scheme(uri) : NULL;
            const char *host = uri ? g_uri_get_host(uri) : NULL;
            gboolean loopback = host && (g_str_equal(host, "localhost") || g_str_equal(host, "127.0.0.1") ||
                                          g_str_equal(host, "::1"));
            gboolean valid = uri && host && g_uri_get_userinfo(uri) == NULL && g_uri_get_query(uri) == NULL &&
                             g_uri_get_fragment(uri) == NULL &&
                             (g_strcmp0(scheme, "https") == 0 || (g_strcmp0(scheme, "http") == 0 && loopback));
            if (!valid) {
                add_issue(issues,
                          TRUE,
                          provider->id,
                          "enterpriseHost",
                          "invalid_enterprise_host",
                          "sub2api base URL must use HTTPS, or loopback HTTP, without credentials, query, or fragment.");
            }
            if (uri) g_uri_unref(uri);
            g_clear_error(&uri_error);
        }
        json_object *cookie_source = NULL;
        json_object *cookie_header = NULL;
        gboolean manual_cookie = provider->raw &&
                                 json_object_object_get_ex(provider->raw, "cookieSource", &cookie_source) &&
                                 json_object_is_type(cookie_source, json_type_string) &&
                                 g_str_equal(json_object_get_string(cookie_source), "manual");
        gboolean has_cookie_header = provider->raw &&
                                     json_object_object_get_ex(provider->raw, "cookieHeader", &cookie_header) &&
                                     json_object_is_type(cookie_header, json_type_string) &&
                                     json_object_get_string(cookie_header)[0] != '\0';
        if (manual_cookie && !has_cookie_header) {
            add_issue(issues,
                      FALSE,
                      provider->id,
                      "cookieHeader",
                      "cookie_header_missing",
                      "cookieSource manual is set but cookieHeader is missing for %s.",
                      provider->id);
        }
        json_object *token_accounts = NULL;
        json_object *accounts = NULL;
        gboolean has_accounts = provider->raw &&
                                json_object_object_get_ex(provider->raw, "tokenAccounts", &token_accounts) &&
                                json_object_is_type(token_accounts, json_type_object) &&
                                json_object_object_get_ex(token_accounts, "accounts", &accounts) &&
                                json_object_is_type(accounts, json_type_array) && json_object_array_length(accounts) > 0;
        if (has_accounts && !codexbar_token_accounts_supported(provider->id)) {
            add_issue(issues,
                      FALSE,
                      provider->id,
                      "tokenAccounts",
                      "token_accounts_unused",
                      "tokenAccounts are set but %s does not support token accounts.",
                      provider->id);
        }
        if (has_accounts && g_str_equal(provider->id, "zai")) {
            size_t account_count = json_object_array_length(accounts);
            for (size_t account_index = 0; account_index < account_count; account_index++) {
                json_object *account = json_object_array_get_idx(accounts, account_index);
                json_object *scope = NULL;
                if (!json_object_is_type(account, json_type_object) ||
                    !json_object_object_get_ex(account, "usageScope", &scope) ||
                    !json_object_is_type(scope, json_type_string) ||
                    !g_str_equal(json_object_get_string(scope), "team")) {
                    continue;
                }
                json_object *organization = NULL;
                json_object *workspace = NULL;
                gboolean has_organization = json_object_object_get_ex(account, "organizationId", &organization) &&
                                            json_object_is_type(organization, json_type_string) &&
                                            json_object_get_string(organization)[0] != '\0';
                gboolean has_workspace = json_object_object_get_ex(account, "workspaceID", &workspace) &&
                                         json_object_is_type(workspace, json_type_string) &&
                                         json_object_get_string(workspace)[0] != '\0';
                if (!has_organization || !has_workspace) {
                    add_issue(issues,
                              FALSE,
                              provider->id,
                              "tokenAccounts",
                              "zai_team_context_missing",
                              "z.ai Team mode requires both organizationId and workspaceID.");
                    break;
                }
            }
        }
    }
    return issues;
}

CodexBarProviderConfig *codexbar_config_provider(CodexBarConfig *config, const char *id) {
    if (!config || !id) return NULL;
    const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(id);
    if (!descriptor) return NULL;
    for (guint index = 0; index < config->providers->len; index++) {
        CodexBarProviderConfig *provider = g_ptr_array_index(config->providers, index);
        if (g_str_equal(provider->id, descriptor->id)) return provider;
    }
    return NULL;
}

gboolean codexbar_config_set_enabled(CodexBarConfig *config, const char *id, gboolean enabled, GError **error) {
    CodexBarProviderConfig *provider = codexbar_config_provider(config, id);
    if (!provider) {
        g_set_error(error, config_error_quark(), 3, "Unknown provider: %s", id ? id : "<missing>");
        return FALSE;
    }
    provider->enabled = enabled;
    provider->has_enabled = TRUE;
    return TRUE;
}

gboolean codexbar_config_set_api_key(CodexBarConfig *config,
                                      const char *id,
                                      const char *api_key,
                                      size_t api_key_length,
                                      gboolean enable,
                                      GError **error) {
    CodexBarProviderConfig *provider = codexbar_config_provider(config, id);
    const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(id);
    gboolean embedded_nul = api_key && memchr(api_key, '\0', api_key_length);
    char *clean = api_key && !embedded_nul ? g_strndup(api_key, api_key_length) : NULL;
    if (clean) g_strstrip(clean);
    if (!provider || !descriptor) {
        g_set_error(error, config_error_quark(), 3, "Unknown provider: %s", id ? id : "<missing>");
        g_free(clean);
        return FALSE;
    }
    if (!codexbar_provider_supports_config_api_key(descriptor)) {
        g_set_error(error, config_error_quark(), 4, "%s does not support config API keys", descriptor->display_name);
        g_free(clean);
        return FALSE;
    }
    gboolean valid = clean && clean[0] != '\0';
    for (const unsigned char *cursor = (const unsigned char *)clean; valid && *cursor; cursor++) {
        if (*cursor < 32 || *cursor == 127) valid = FALSE;
    }
    if (!valid) {
        g_set_error_literal(
            error, config_error_quark(), 5, "API key must be non-empty and contain no control characters");
        g_free(clean);
        return FALSE;
    }
    g_free(provider->api_key);
    provider->api_key = clean;
    if (enable) {
        provider->enabled = TRUE;
        provider->has_enabled = TRUE;
    }
    return TRUE;
}

gboolean codexbar_config_set_codex_active_source(CodexBarConfig *config,
                                                  CodexBarCodexActiveSourceKind source,
                                                  const char *value,
                                                  GError **error) {
    CodexBarProviderConfig *provider = codexbar_config_provider(config, "codex");
    if (!provider) {
        g_set_error_literal(error, config_error_quark(), 9, "Codex provider config is unavailable");
        return FALSE;
    }
    char *clean = value ? g_strstrip(g_strdup(value)) : NULL;
    if (source == CODEXBAR_CODEX_SOURCE_MANAGED_ACCOUNT &&
        (!clean || !g_uuid_string_is_valid(clean))) {
        g_set_error_literal(error, config_error_quark(), 9, "Managed Codex source requires a valid account ID");
        g_free(clean);
        return FALSE;
    }
    if (source == CODEXBAR_CODEX_SOURCE_PROFILE_HOME &&
        (!clean || !g_path_is_absolute(clean))) {
        g_set_error_literal(error, config_error_quark(), 9, "Codex profile source requires an absolute home path");
        g_free(clean);
        return FALSE;
    }
    g_free(provider->codex_active_account_id);
    g_free(provider->codex_active_home_path);
    provider->codex_active_account_id = NULL;
    provider->codex_active_home_path = NULL;
    provider->has_codex_active_source = TRUE;
    provider->codex_active_source = source;
    if (source == CODEXBAR_CODEX_SOURCE_MANAGED_ACCOUNT) {
        provider->codex_active_account_id = g_ascii_strdown(clean, -1);
    } else if (source == CODEXBAR_CODEX_SOURCE_PROFILE_HOME) {
        provider->codex_active_home_path = g_canonicalize_filename(clean, NULL);
        provider->has_codex_profile_home_paths = TRUE;
        if (!g_ptr_array_find_with_equal_func(provider->codex_profile_home_paths,
                                              provider->codex_active_home_path,
                                              (GEqualFunc)g_str_equal,
                                              NULL)) {
            g_ptr_array_add(provider->codex_profile_home_paths,
                            g_strdup(provider->codex_active_home_path));
        }
    }
    g_free(clean);
    return TRUE;
}

void codexbar_config_free(CodexBarConfig *config) {
    if (!config) return;
    g_free(config->path);
    g_free(config->agent_sessions_manual_hosts);
    g_ptr_array_unref(config->providers);
    if (config->raw) json_object_put(config->raw);
    g_free(config->loaded_digest);
    if (config->lock_fd >= 0) {
        flock(config->lock_fd, LOCK_UN);
        close(config->lock_fd);
    }
    g_free(config);
}
