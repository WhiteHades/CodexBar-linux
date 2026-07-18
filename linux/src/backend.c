#include "backend.h"

#include "config.h"
#include "codebuff.h"
#include "codex.h"
#include "jetbrains.h"
#include "kimi.h"
#include "openrouter.h"
#include "process.h"
#include "provider_registry.h"
#include "proxy_providers.h"
#include "simple_providers.h"

#include <gio/gio.h>
#include <string.h>

#define ORACLE_TIMEOUT_MILLISECONDS 60000
#define ORACLE_TERMINATION_GRACE_MILLISECONDS 400
#define ORACLE_MAXIMUM_OUTPUT_BYTES (1024U * 1024U)

static gboolean valid_process_text(const char *text, size_t length) {
    return !memchr(text, '\0', length) && g_utf8_validate(text, (gssize)length, NULL);
}

static CodexBarSnapshot *fetch_oracle(const char *backend,
                                      const char *provider,
                                      const char *source,
                                      GError **error) {
    const char *argv[10] = {backend, "usage", "--format", "json", NULL};
    guint argument = 4;
    if (provider) {
        argv[argument++] = "--provider";
        argv[argument++] = provider;
    }
    if (source) {
        argv[argument++] = "--source";
        argv[argument++] = source;
    }
    argv[argument] = NULL;

    CodexBarProcessRequest request = {
        .arguments = argv,
        .timeout_milliseconds = ORACLE_TIMEOUT_MILLISECONDS,
        .termination_grace_milliseconds = ORACLE_TERMINATION_GRACE_MILLISECONDS,
        .maximum_output_bytes = ORACLE_MAXIMUM_OUTPUT_BYTES,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, NULL, error);
    if (!result) return NULL;
    if (!valid_process_text(result->standard_output, result->standard_output_length)) {
        codexbar_process_result_free(result);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Backend output is not valid UTF-8 text");
        return NULL;
    }
    if (!valid_process_text(result->standard_error, result->standard_error_length)) {
        codexbar_process_result_free(result);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Backend error output is not valid UTF-8 text");
        return NULL;
    }

    CodexBarSnapshot *snapshot = result->standard_output_length > 0
                                     ? codexbar_snapshot_parse(result->standard_output, NULL)
                                     : NULL;
    if (!snapshot && !codexbar_process_result_succeeded(result)) {
        const char *diagnostic = result->standard_error_length > 0 ? result->standard_error : "no diagnostic output";
        if (result->termination_signal != 0) {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "Backend terminated by signal %d: %s",
                        result->termination_signal,
                        diagnostic);
        } else {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "Backend exited with status %d: %s",
                        result->exit_status,
                        diagnostic);
        }
        codexbar_process_result_free(result);
        return NULL;
    }
    if (!snapshot) snapshot = codexbar_snapshot_parse(result->standard_output, error);
    codexbar_process_result_free(result);
    return snapshot;
}

static CodexBarProvider *provider_error(const CodexBarProviderConfig *config, const char *source, GError *error) {
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup(config->id);
    provider->source = g_strdup(source);
    provider->error = g_strdup(error ? error->message : "Provider fetch failed without a diagnostic");
    provider->error_code = 1;
    provider->error_kind = g_strdup("provider");
    if (error && error->domain == G_SPAWN_ERROR && error->code == G_SPAWN_ERROR_NOENT) {
        provider->error_code = 2;
        g_free(provider->error_kind);
        provider->error_kind = g_strdup("binaryNotFound");
    } else if (error && (strstr(error->message, "malformed") || strstr(error->message, "Invalid backend JSON"))) {
        provider->error_code = 3;
        g_free(provider->error_kind);
        provider->error_kind = g_strdup("parse");
    } else if (error && (strstr(error->message, "timed out") || strstr(error->message, "Timeout"))) {
        provider->error_code = 4;
        g_free(provider->error_kind);
        provider->error_kind = g_strdup("timeout");
    }
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
    const char *native_source = NULL;
    switch (descriptor->native_provider) {
    case CODEXBAR_NATIVE_CODEX:
    case CODEXBAR_NATIVE_JETBRAINS:
        native_source = "cli";
        break;
    case CODEXBAR_NATIVE_UNAVAILABLE:
        break;
    case CODEXBAR_NATIVE_CODEBUFF:
    case CODEXBAR_NATIVE_KIMI:
    case CODEXBAR_NATIVE_KIMI_K2:
    case CODEXBAR_NATIVE_OPENROUTER:
    case CODEXBAR_NATIVE_PROXY:
    case CODEXBAR_NATIVE_SIMPLE:
        native_source = "api";
        break;
    }
    if (!codexbar_provider_supports_source(descriptor, configured_source)) {
        GError *error = g_error_new(G_IO_ERROR,
                                    G_IO_ERROR_NOT_SUPPORTED,
                                    "Source '%s' is not supported for %s.",
                                    configured_source,
                                    descriptor->cli_name);
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
    case CODEXBAR_NATIVE_CODEBUFF:
        provider = codexbar_codebuff_fetch(config, &error);
        break;
    case CODEXBAR_NATIVE_JETBRAINS:
        provider = codexbar_jetbrains_fetch(&error);
        break;
    case CODEXBAR_NATIVE_KIMI:
        provider = codexbar_kimi_fetch(config, &error);
        break;
    case CODEXBAR_NATIVE_KIMI_K2:
        provider = codexbar_kimik2_fetch(config, &error);
        break;
    case CODEXBAR_NATIVE_OPENROUTER:
        provider = codexbar_openrouter_fetch(config, &error);
        break;
    case CODEXBAR_NATIVE_PROXY:
        provider = codexbar_proxy_provider_fetch(config, &error);
        break;
    case CODEXBAR_NATIVE_SIMPLE:
        provider = codexbar_simple_provider_fetch(config, &error);
        break;
    case CODEXBAR_NATIVE_UNAVAILABLE:
        error = g_error_new(
            G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "%s has no native Linux source yet", descriptor->display_name);
        break;
    }
    const char *error_source = descriptor->native_provider == CODEXBAR_NATIVE_JETBRAINS
                                   ? configured_source
                                   : native_source ? native_source : configured_source;
    return provider ? provider : provider_error(config, error_source, error);
}

CodexBarSnapshot *codexbar_backend_fetch(GError **error) {
    const char *backend = g_getenv("CODEXBAR_BACKEND");
    if (backend && backend[0] != '\0') {
        return fetch_oracle(backend, NULL, NULL, error);
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

CodexBarSnapshot *codexbar_backend_fetch_all(GError **error) {
    const char *backend = g_getenv("CODEXBAR_BACKEND");
    if (backend && backend[0] != '\0') return fetch_oracle(backend, "all", NULL, error);
    CodexBarConfig *config = codexbar_config_load(error);
    if (!config) return NULL;
    CodexBarSnapshot *snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    for (guint index = 0; index < codexbar_provider_registry_count(); index++) {
        const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_at(index);
        CodexBarProviderConfig *provider_config = codexbar_config_provider(config, descriptor->id);
        g_ptr_array_add(snapshot->providers, fetch_provider(provider_config));
    }
    codexbar_config_free(config);
    return snapshot;
}

CodexBarProvider *codexbar_backend_fetch_one(const char *provider_name, const char *source, GError **error) {
    const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(provider_name);
    if (!descriptor) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unknown provider: %s", provider_name);
        return NULL;
    }
    if (source && !codexbar_provider_supports_source(descriptor, source)) {
        CodexBarProviderConfig selected = {.id = (char *)descriptor->id, .source = (char *)source};
        GError *source_error = g_error_new(G_IO_ERROR,
                                           G_IO_ERROR_NOT_SUPPORTED,
                                           "Source '%s' is not supported for %s.",
                                           source,
                                           descriptor->cli_name);
        return provider_error(&selected, source, source_error);
    }
    const char *backend = g_getenv("CODEXBAR_BACKEND");
    if (backend && backend[0] != '\0') {
        CodexBarSnapshot *snapshot = fetch_oracle(backend, descriptor->cli_name, source, error);
        if (!snapshot) return NULL;
        CodexBarProvider *result = NULL;
        for (guint index = 0; index < snapshot->providers->len; index++) {
            CodexBarProvider *candidate = g_ptr_array_index(snapshot->providers, index);
            if (g_str_equal(candidate->provider, descriptor->id)) {
                result = g_ptr_array_steal_index(snapshot->providers, index);
                break;
            }
        }
        codexbar_snapshot_free(snapshot);
        if (!result) {
            g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Provider did not return data: %s", descriptor->id);
        }
        return result;
    }

    CodexBarConfig *config = codexbar_config_load(error);
    if (!config) return NULL;
    CodexBarProviderConfig *stored = codexbar_config_provider(config, descriptor->id);
    CodexBarProviderConfig selected = *stored;
    selected.source = (char *)(source ? source : stored->source);
    CodexBarProvider *result = fetch_provider(&selected);
    codexbar_config_free(config);
    return result;
}
