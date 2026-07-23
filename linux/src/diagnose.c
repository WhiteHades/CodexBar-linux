#include "diagnose.h"

#include "aiand.h"
#include "backend.h"
#include "model.h"
#include "neuralwatt.h"
#include "version.h"

#include <glib.h>
#include <math.h>
#include <string.h>

static char *iso8601_now(void) {
    GDateTime *date = g_date_time_new_now_utc();
    char *text = g_date_time_format(date, "%Y-%m-%dT%H:%M:%SZ");
    g_date_time_unref(date);
    return text;
}

static char *iso8601_milliseconds(gint64 milliseconds) {
    GDateTime *date = g_date_time_new_from_unix_utc(milliseconds / 1000);
    if (!date) return NULL;
    char *text = g_date_time_format(date, "%Y-%m-%dT%H:%M:%SZ");
    g_date_time_unref(date);
    return text;
}

static const char *safe_source(const char *source) {
    if (!source) return "unknown";
    char *lower = g_ascii_strdown(source, -1);
    const char *safe = "unknown";
    if (strstr(lower, "oauth")) {
        safe = "oauth";
    } else if (strstr(lower, "web")) {
        safe = "web";
    } else if (strstr(lower, "api")) {
        safe = "api";
    } else if (strstr(lower, "cli")) {
        safe = "cli";
    } else if (strstr(lower, "local")) {
        safe = "local";
    }
    g_free(lower);
    return safe;
}

static const char *source_mode(const CodexBarProviderConfig *config) {
    if (!config || !config->source || config->source[0] == '\0') return "auto";
    if (g_str_equal(config->source, "web") || g_str_equal(config->source, "cli") ||
        g_str_equal(config->source, "oauth") || g_str_equal(config->source, "api")) {
        return config->source;
    }
    return "auto";
}

static gboolean valid_api_credential(const char *value) {
    if (!value) return FALSE;
    char *trimmed = g_strstrip(g_strdup(value));
    size_t length = strlen(trimmed);
    if (length >= 1 && ((trimmed[0] == '\'' && trimmed[length - 1] == '\'') ||
                        (trimmed[0] == '"' && trimmed[length - 1] == '"'))) {
        trimmed[length - 1] = '\0';
        memmove(trimmed, trimmed + 1, length - 1);
        g_strstrip(trimmed);
    }
    gboolean present = trimmed[0] != '\0';
    for (const unsigned char *cursor = (const unsigned char *)trimmed; present && *cursor; cursor++) {
        if (*cursor < 32 || *cursor == 127) present = FALSE;
    }
    g_free(trimmed);
    return present;
}

static gboolean nonempty_environment(const char *name) {
    return valid_api_credential(g_getenv(name));
}

static gboolean environment_api_auth(const char *provider) {
    if (g_str_equal(provider, "codebuff")) return nonempty_environment("CODEBUFF_API_KEY");
    if (g_str_equal(provider, "clinepass")) {
        return nonempty_environment("CLINE_API_KEY") || nonempty_environment("CLINEPASS_API_KEY");
    }
    if (g_str_equal(provider, "deepinfra")) {
        return nonempty_environment("DEEPINFRA_API_KEY") || nonempty_environment("DEEPINFRA_TOKEN");
    }
    if (g_str_equal(provider, "kimi")) return nonempty_environment("KIMI_CODE_API_KEY");
    if (g_str_equal(provider, "openrouter")) return nonempty_environment("OPENROUTER_API_KEY");
    if (g_str_equal(provider, "deepseek")) return nonempty_environment("DEEPSEEK_API_KEY");
    if (g_str_equal(provider, "moonshot")) return nonempty_environment("MOONSHOT_API_KEY");
    if (g_str_equal(provider, "elevenlabs")) return nonempty_environment("ELEVENLABS_API_KEY");
    if (g_str_equal(provider, "crof")) {
        return nonempty_environment("CROF_API_KEY") || nonempty_environment("CROFAI_API_KEY");
    }
    if (g_str_equal(provider, "venice")) {
        return nonempty_environment("VENICE_API_KEY") || nonempty_environment("VENICE_KEY");
    }
    if (g_str_equal(provider, "zenmux")) return nonempty_environment("ZENMUX_MANAGEMENT_API_KEY");
    if (g_str_equal(provider, "llmproxy")) return nonempty_environment("LLM_PROXY_API_KEY");
    if (g_str_equal(provider, "clawrouter")) return nonempty_environment("CLAWROUTER_API_KEY");
    return FALSE;
}

static gboolean config_web_auth(const CodexBarProviderConfig *config) {
    if (!config || !config->raw) return FALSE;
    json_object *cookie = NULL;
    return json_object_object_get_ex(config->raw, "cookieHeader", &cookie) &&
           json_object_is_type(cookie, json_type_string) && json_object_get_string(cookie)[0] != '\0';
}

static void add_mode(json_object *modes, const char *mode) {
    if (!mode || g_str_equal(mode, "unknown")) return;
    size_t count = json_object_array_length(modes);
    for (size_t index = 0; index < count; index++) {
        if (g_str_equal(json_object_get_string(json_object_array_get_idx(modes, index)), mode)) return;
    }
    json_object_array_add(modes, json_object_new_string(mode));
}

static json_object *auth_summary(const CodexBarProviderDescriptor *descriptor,
                                 const CodexBarProviderConfig *config,
                                 const CodexBarProvider *provider) {
    json_object *modes = json_object_new_array();
    gboolean api = FALSE;
    if (g_str_equal(descriptor->id, "aiand")) {
        api = codexbar_aiand_has_api_key(config);
    } else if (g_str_equal(descriptor->id, "neuralwatt")) {
        api = codexbar_neuralwatt_has_api_key(config);
    } else {
        api = config && (valid_api_credential(config->api_key) || valid_api_credential(config->secret_key));
    }
    if (g_str_equal(descriptor->id, "bedrock")) {
        api = config && valid_api_credential(config->api_key) && valid_api_credential(config->secret_key);
    }
    api = api || environment_api_auth(descriptor->id);
    gboolean web = config_web_auth(config);
    if (api) add_mode(modes, "api");
    if (web) add_mode(modes, "web");
    gboolean success = provider && !provider->error;
    if (success) add_mode(modes, safe_source(provider->source));

    json_object *object = json_object_new_object();
    json_object_object_add(object, "configured", json_object_new_boolean(api || web || success));
    json_object_object_add(object, "modes", modes);
    return object;
}

static const char *error_category(const char *description, gboolean auth_configured) {
    if (!description) return auth_configured ? "unknown" : "auth";
    char *lower = g_ascii_strdown(description, -1);
    const char *category = "unknown";
    if (strstr(lower, "endpoint override") || strstr(lower, "not implemented") ||
        strstr(lower, "not supported") || strstr(lower, "unsupported source")) {
        category = "configuration";
    } else if (strstr(lower, "network") || strstr(lower, "timeout") || strstr(lower, "connection") ||
               strstr(lower, "resolve host") || strstr(lower, "tls")) {
        category = "network";
    } else if (strstr(lower, "auth") || strstr(lower, "credential") || strstr(lower, "token") ||
               strstr(lower, "cookie") || strstr(lower, "api key") || strstr(lower, "missing key") ||
               strstr(lower, "401") || strstr(lower, "403") || strstr(lower, "unauthorized") ||
               strstr(lower, "forbidden")) {
        category = "auth";
    } else if (strstr(lower, "source") || strstr(lower, "unavailable")) {
        category = "configuration";
    } else if (strstr(lower, "parse") || strstr(lower, "format") || strstr(lower, "decode") ||
               strstr(lower, "json")) {
        category = "parse";
    } else if (strstr(lower, "api") || strstr(lower, "http") || strstr(lower, "404") ||
               strstr(lower, "rate limit") || strstr(lower, "out of credits")) {
        category = "api";
    } else if (!auth_configured) {
        category = "auth";
    }
    g_free(lower);
    return category;
}

static const char *safe_error_description(const char *category) {
    if (g_str_equal(category, "network")) return "Network error - check your connection";
    if (g_str_equal(category, "auth")) return "Authentication or setup issue - check provider credentials";
    if (g_str_equal(category, "api")) return "API error - service returned an unexpected response";
    if (g_str_equal(category, "parse")) return "Parse error - unexpected response format";
    if (g_str_equal(category, "configuration")) return "Configuration issue - check provider source and settings";
    return "An unexpected error occurred";
}

static json_object *diagnostic_error(const char *category) {
    json_object *object = json_object_new_object();
    json_object_object_add(object, "category", json_object_new_string(category));
    json_object_object_add(object, "safeDescription", json_object_new_string(safe_error_description(category)));
    return object;
}

static void add_optional_time(json_object *object, const char *key, gboolean present, gint64 milliseconds) {
    if (!present) return;
    char *timestamp = iso8601_milliseconds(milliseconds);
    if (timestamp) json_object_object_add(object, key, json_object_new_string(timestamp));
    g_free(timestamp);
}

static json_object *diagnostic_window(const CodexBarQuotaWindow *window, const char *label) {
    json_object *object = json_object_new_object();
    json_object_object_add(object, "label", json_object_new_string(label));
    double used = isfinite(window->used_percent) ? window->used_percent : 0;
    json_object_object_add(object, "usedPercent", json_object_new_double(used));
    if (window->has_window_minutes) {
        json_object_object_add(object, "windowMinutes", json_object_new_int64(window->window_minutes));
    }
    add_optional_time(object, "resetsAt", window->has_resets_at, window->resets_at_ms);
    json_object_object_add(object,
                           "hasResetDescription",
                           json_object_new_boolean((window->detail && window->detail[0] != '\0') ||
                                                   (window->reset_description &&
                                                    window->reset_description[0] != '\0')));
    if (!window->usage_known) json_object_object_add(object, "usageKnown", json_object_new_boolean(FALSE));
    return object;
}

static json_object *provider_specific_data(const CodexBarProvider *provider) {
    static const char *const names[] = {
        "kiroUsage",          "ampUsage",          "zaiUsage",       "minimaxUsage",
        "deepseekUsage",      "openRouterUsage",   "sakanaPayAsYouGo",
        "openAIAPIUsage",     "claudeAdminAPIUsage", "mistralUsage", "deepgramUsage",
        "cursorRequests",
    };
    json_object *array = json_object_new_array();
    if (!provider->usage_extensions || !json_object_is_type(provider->usage_extensions, json_type_object)) {
        return array;
    }
    for (size_t index = 0; index < G_N_ELEMENTS(names); index++) {
        json_object *value = NULL;
        if (json_object_object_get_ex(provider->usage_extensions, names[index], &value) && value) {
            json_object_array_add(array, json_object_new_string(names[index]));
        }
    }
    return array;
}

static json_object *usage_summary(const CodexBarProvider *provider) {
    json_object *object = json_object_new_object();
    char *updated = provider->has_updated_at ? iso8601_milliseconds(provider->updated_at_ms) : iso8601_now();
    json_object_object_add(object, "updatedAt", json_object_new_string(updated));
    g_free(updated);
    const char *confidence = "unknown";
    json_object *confidence_value = NULL;
    if (provider->usage_extensions &&
        json_object_object_get_ex(provider->usage_extensions, "dataConfidence", &confidence_value) &&
        json_object_is_type(confidence_value, json_type_string)) {
        const char *candidate = json_object_get_string(confidence_value);
        size_t length = (size_t)json_object_get_string_len(confidence_value);
        if (!memchr(candidate, '\0', length) &&
            (g_str_equal(candidate, "exact") || g_str_equal(candidate, "estimated") ||
             g_str_equal(candidate, "percentOnly"))) {
            confidence = candidate;
        }
    }
    json_object_object_add(object, "dataConfidence", json_object_new_string(confidence));
    json_object *windows = json_object_new_array();
    guint count = provider->quota_windows ? provider->quota_windows->len : 0;
    guint extra_count = 0;
    for (guint index = 0; index < count; index++) {
        const CodexBarQuotaWindow *window = g_ptr_array_index(provider->quota_windows, index);
        gboolean canonical = g_str_equal(window->id, "primary") || g_str_equal(window->id, "secondary") ||
                             g_str_equal(window->id, "tertiary");
        const char *extra_label = g_str_equal(provider->provider, "neuralwatt") &&
                                          g_str_equal(window->id, "key-allowance")
                                      ? "Key allowance"
                                      : "extra";
        const char *label = canonical ? window->id
                            : provider->explicit_quota_slots
                                ? extra_label
                                : index == 0 ? "primary" : index == 1 ? "secondary" : index == 2 ? "tertiary" : "extra";
        if ((provider->explicit_quota_slots && !canonical) || (!provider->explicit_quota_slots && index >= 3)) {
            extra_count++;
        }
        json_object_array_add(windows, diagnostic_window(window, label));
    }
    json_object_object_add(object, "windows", windows);
    json_object_object_add(object, "extraWindowCount", json_object_new_int64(extra_count));
    json_object_object_add(object, "providerCostPresent", json_object_new_boolean(provider->provider_cost != NULL));
    json_object_object_add(object, "providerSpecificData", provider_specific_data(provider));
    return object;
}

static gboolean safe_region(const char *region) {
    if (!region || region[0] == '\0' || strlen(region) > 32) return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)region; *cursor; cursor++) {
        if (!g_ascii_isalnum(*cursor) && *cursor != '-' && *cursor != '_' && *cursor != '.') return FALSE;
    }
    return TRUE;
}

json_object *codexbar_diagnose_provider(const CodexBarProviderDescriptor *descriptor,
                                        const CodexBarProviderConfig *config) {
    GError *fetch_error = NULL;
    CodexBarProvider *provider = codexbar_backend_fetch_one(descriptor->id, source_mode(config), &fetch_error);
    json_object *auth = auth_summary(descriptor, config, provider);
    json_object *configured_value = NULL;
    json_object_object_get_ex(auth, "configured", &configured_value);
    gboolean configured = json_object_get_boolean(configured_value);
    const char *raw_error = provider && provider->error ? provider->error : fetch_error ? fetch_error->message : NULL;
    gboolean success = provider && !raw_error;

    json_object *object = json_object_new_object();
    char *timestamp = iso8601_now();
    json_object_object_add(object, "schemaVersion", json_object_new_string("1.0"));
    json_object_object_add(object, "timestamp", json_object_new_string(timestamp));
    g_free(timestamp);
    json_object_object_add(object, "platform", json_object_new_string("Linux"));
    json_object_object_add(object, "appVersion", json_object_new_string(CODEXBAR_LINUX_VERSION));
    json_object_object_add(object, "provider", json_object_new_string(descriptor->id));
    json_object_object_add(object, "displayName", json_object_new_string(descriptor->display_name));
    json_object_object_add(object, "source", json_object_new_string(success ? safe_source(provider->source) : "failed"));
    json_object_object_add(object, "sourceMode", json_object_new_string(source_mode(config)));
    json_object_object_add(object, "auth", auth);
    if (success) json_object_object_add(object, "usage", usage_summary(provider));

    json_object *attempts = json_object_new_array();
    if (provider && provider->source) {
        json_object *attempt = json_object_new_object();
        json_object_object_add(attempt, "kind", json_object_new_string(safe_source(provider->source)));
        json_object_object_add(attempt, "wasAvailable", json_object_new_boolean(TRUE));
        if (!success) {
            json_object_object_add(attempt,
                                   "errorCategory",
                                   json_object_new_string(error_category(raw_error, configured)));
        }
        json_object_array_add(attempts, attempt);
    }
    json_object_object_add(object, "fetchAttempts", attempts);
    if (!success) {
        const char *category = error_category(raw_error, configured);
        json_object_object_add(object, "error", diagnostic_error(category));
    }

    json_object *settings = json_object_new_object();
    json_object_object_add(settings, "sourceMode", json_object_new_string(source_mode(config)));
    if (config && safe_region(config->region)) {
        json_object_object_add(settings, "apiRegion", json_object_new_string(config->region));
    }
    json_object_object_add(object, "settings", settings);

    codexbar_provider_free(provider);
    g_clear_error(&fetch_error);
    return object;
}
