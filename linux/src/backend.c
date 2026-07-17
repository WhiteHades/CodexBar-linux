#include "backend.h"

#include "config.h"
#include "openrouter.h"
#include "simple_providers.h"

#include <gio/gio.h>

static CodexBarSnapshot *fetch_oracle(const char *backend, GError **error) {

    const char *argv[] = {
        backend,
        "usage",
        "--format",
        "json",
        NULL,
    };

    GSubprocess *process = g_subprocess_newv(argv,
                                             G_SUBPROCESS_FLAGS_STDOUT_PIPE | G_SUBPROCESS_FLAGS_STDERR_PIPE,
                                             error);
    if (!process) {
        return NULL;
    }

    char *stdout_text = NULL;
    char *stderr_text = NULL;
    gboolean communicated = g_subprocess_communicate_utf8(process, NULL, NULL, &stdout_text, &stderr_text, error);
    if (!communicated) {
        g_object_unref(process);
        g_free(stdout_text);
        g_free(stderr_text);
        return NULL;
    }

    if (!g_subprocess_get_successful(process)) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_FAILED,
                    "Backend exited with status %d: %s",
                    g_subprocess_get_exit_status(process),
                    stderr_text && stderr_text[0] != '\0' ? stderr_text : "no diagnostic output");
        g_object_unref(process);
        g_free(stdout_text);
        g_free(stderr_text);
        return NULL;
    }

    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(stdout_text, error);
    g_object_unref(process);
    g_free(stdout_text);
    g_free(stderr_text);
    return snapshot;
}

CodexBarSnapshot *codexbar_backend_fetch(GError **error) {
    const char *backend = g_getenv("CODEXBAR_BACKEND");
    if (backend && backend[0] != '\0') {
        return fetch_oracle(backend, error);
    }

    CodexBarConfig *config = codexbar_config_load(error);
    if (!config) {
        return NULL;
    }
    CodexBarSnapshot *snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    for (guint index = 0; index < config->providers->len; index++) {
        CodexBarProviderConfig *provider_config = g_ptr_array_index(config->providers, index);
        if (!provider_config->enabled) {
            continue;
        }
        if (g_str_equal(provider_config->id, "openrouter")) {
            GError *provider_error = NULL;
            CodexBarProvider *provider = codexbar_openrouter_fetch(provider_config, &provider_error);
            if (!provider) {
                provider = g_new0(CodexBarProvider, 1);
                provider->provider = g_strdup("openrouter");
                provider->source = g_strdup("api");
                provider->error = g_strdup(provider_error->message);
                g_error_free(provider_error);
            }
            g_ptr_array_add(snapshot->providers, provider);
        } else if (g_str_equal(provider_config->id, "deepseek") ||
                   g_str_equal(provider_config->id, "moonshot") ||
                   g_str_equal(provider_config->id, "elevenlabs")) {
            GError *provider_error = NULL;
            CodexBarProvider *provider = codexbar_simple_provider_fetch(provider_config, &provider_error);
            if (!provider) {
                provider = g_new0(CodexBarProvider, 1);
                provider->provider = g_strdup(provider_config->id);
                provider->source = g_strdup("api");
                provider->error = g_strdup(provider_error->message);
                g_error_free(provider_error);
            }
            g_ptr_array_add(snapshot->providers, provider);
        } else {
            CodexBarProvider *provider = g_new0(CodexBarProvider, 1);
            provider->provider = g_strdup(provider_config->id);
            provider->source = g_strdup(provider_config->source ? provider_config->source : "auto");
            provider->error = g_strdup_printf("%s is not migrated to the native C engine yet", provider_config->id);
            g_ptr_array_add(snapshot->providers, provider);
        }
    }
    codexbar_config_free(config);
    return snapshot;
}
