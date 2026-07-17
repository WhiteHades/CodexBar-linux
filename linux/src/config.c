#include "config.h"

#include <json-c/json.h>
#include <string.h>

static GQuark config_error_quark(void) {
    return g_quark_from_static_string("codexbar-config-error");
}

static char *clean_string(json_object *object, const char *key) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    char *clean = g_strdup(json_object_get_string(value));
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
    if (!provider) {
        return;
    }
    g_free(provider->id);
    g_free(provider->source);
    g_free(provider->api_key);
    g_free(provider->secret_key);
    g_free(provider->region);
    g_free(provider->workspace_id);
    g_free(provider->enterprise_host);
    g_free(provider->aws_profile);
    g_free(provider->aws_auth_mode);
    g_free(provider);
}

static char *resolve_path(void) {
    const char *override = g_getenv("CODEXBAR_CONFIG");
    if (override && override[0] != '\0') {
        return g_canonicalize_filename(override, g_get_home_dir());
    }
    const char *xdg = g_getenv("XDG_CONFIG_HOME");
    char *xdg_path = xdg && g_path_is_absolute(xdg)
                         ? g_build_filename(xdg, "codexbar", "config.json", NULL)
                         : g_build_filename(g_get_home_dir(), ".config", "codexbar", "config.json", NULL);
    if (g_file_test(xdg_path, G_FILE_TEST_EXISTS)) {
        return xdg_path;
    }
    char *legacy = g_build_filename(g_get_home_dir(), ".codexbar", "config.json", NULL);
    if (g_file_test(legacy, G_FILE_TEST_EXISTS)) {
        g_free(xdg_path);
        return legacy;
    }
    g_free(legacy);
    return xdg_path;
}

CodexBarConfig *codexbar_config_load(GError **error) {
    CodexBarConfig *config = g_new0(CodexBarConfig, 1);
    config->path = resolve_path();
    config->providers = g_ptr_array_new_with_free_func(provider_config_free);
    if (!g_file_test(config->path, G_FILE_TEST_EXISTS)) {
        CodexBarProviderConfig *codex = g_new0(CodexBarProviderConfig, 1);
        codex->id = g_strdup("codex");
        codex->enabled = TRUE;
        codex->source = g_strdup("auto");
        g_ptr_array_add(config->providers, codex);
        return config;
    }

    char *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(config->path, &contents, &length, error)) {
        codexbar_config_free(config);
        return NULL;
    }
    json_tokener *tokener = json_tokener_new();
    json_object *root = json_tokener_parse_ex(tokener, contents, (int)length);
    enum json_tokener_error code = json_tokener_get_error(tokener);
    g_free(contents);
    json_tokener_free(tokener);
    if (code != json_tokener_success || !root || !json_object_is_type(root, json_type_object)) {
        g_set_error(error, config_error_quark(), 1, "Invalid config JSON: %s", json_tokener_error_desc(code));
        if (root) {
            json_object_put(root);
        }
        codexbar_config_free(config);
        return NULL;
    }

    json_object *providers = NULL;
    if (json_object_object_get_ex(root, "providers", &providers) && json_object_is_type(providers, json_type_array)) {
        size_t count = json_object_array_length(providers);
        for (size_t index = 0; index < count; index++) {
            json_object *entry = json_object_array_get_idx(providers, index);
            if (!json_object_is_type(entry, json_type_object)) {
                continue;
            }
            char *id = clean_string(entry, "id");
            if (!id) {
                continue;
            }
            CodexBarProviderConfig *provider = g_new0(CodexBarProviderConfig, 1);
            provider->id = id;
            provider->enabled = FALSE;
            json_object *enabled = NULL;
            if (json_object_object_get_ex(entry, "enabled", &enabled) &&
                json_object_is_type(enabled, json_type_boolean)) {
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
            g_ptr_array_add(config->providers, provider);
        }
    }
    json_object_put(root);
    return config;
}

void codexbar_config_free(CodexBarConfig *config) {
    if (!config) {
        return;
    }
    g_free(config->path);
    g_ptr_array_unref(config->providers);
    g_free(config);
}
