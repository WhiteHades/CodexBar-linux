#include "backend.h"

#include "config.h"
#include "codex.h"
#include "openrouter.h"
#include "provider_registry.h"
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

static CodexBarProvider *provider_error(const CodexBarProviderConfig *config, const char *source, GError *error) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup(config->id);
    provider->source = g_strdup(source);
    provider->error = g_strdup(error ? error->message : "Provider fetch failed without a diagnostic");
    g_clear_error(&error);
    return provider;
}

static CodexBarProvider *fetch_provider(const CodexBarProviderConfig *config) {
    const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(config->id);
    if (!descriptor) {
        GError *error = g_error_new(G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unknown provider: %s", config->id);
        return provider_error(config, config->source ? config->source : "auto", error);
    }

    const char *configured_source = config->source ? config->source : "auto";
    const char *native_source = descriptor->native_provider == CODEXBAR_NATIVE_CODEX
                                    ? "cli"
                                    : descriptor->native_provider == CODEXBAR_NATIVE_UNAVAILABLE ? NULL : "api";
    if (!codexbar_provider_supports_source(descriptor, configured_source)) {
        GError *error = g_error_new(G_IO_ERROR,
                                    G_IO_ERROR_NOT_SUPPORTED,
                                    "%s does not support source '%s'",
                                    descriptor->display_name,
                                    configured_source);
        return provider_error(config, configured_source, error);
    }
    if (native_source && !g_str_equal(configured_source, "auto") && !g_str_equal(configured_source, native_source)) {
        GError *error = g_error_new(G_IO_ERROR,
                                    G_IO_ERROR_NOT_SUPPORTED,
                                    "%s source '%s' has no native Linux implementation yet",
                                    descriptor->display_name,
                                    configured_source);
        return provider_error(config, configured_source, error);
    }

    GError *error = NULL;
    CodexBarProvider *provider = NULL;
    switch (descriptor->native_provider) {
    case CODEXBAR_NATIVE_CODEX:
        provider = codexbar_codex_fetch(&error);
        break;
    case CODEXBAR_NATIVE_OPENROUTER:
        provider = codexbar_openrouter_fetch(config, &error);
        break;
    case CODEXBAR_NATIVE_SIMPLE:
        provider = codexbar_simple_provider_fetch(config, &error);
        break;
    case CODEXBAR_NATIVE_UNAVAILABLE:
        error = g_error_new(
            G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "%s has no native Linux source yet", descriptor->display_name);
        break;
    }
    return provider ? provider : provider_error(config, native_source ? native_source : configured_source, error);
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
        g_ptr_array_add(snapshot->providers, fetch_provider(provider_config));
    }
    codexbar_config_free(config);
    return snapshot;
}
