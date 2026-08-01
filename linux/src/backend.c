#include "backend.h"

#include "aiand.h"
#include "api_providers.h"
#include "api_providers2.h"
#include "api_providers3.h"
#include "api_providers4.h"
#include "api_providers5.h"
#include "azure_openai.h"
#include "config.h"
#include "codebuff.h"
#include "claude.h"
#include "clinepass.h"
#include "copilot.h"
#include "codex.h"
#include "deepinfra.h"
#include "grok.h"
#include "jetbrains.h"
#include "kilo.h"
#include "kimi.h"
#include "local_providers.h"
#include "managed_codex.h"
#include "neuralwatt.h"
#include "openai_api.h"
#include "opencode_go.h"
#include "openrouter.h"
#include "process.h"
#include "provider_registry.h"
#include "proxy_providers.h"
#include "qwen_cloud.h"
#include "simple_providers.h"
#include "stepfun.h"
#include "token_accounts.h"
#include "vertex.h"
#include "wayfinder.h"
#include "web_providers.h"
#include "web_providers2.h"
#include "web_providers3.h"
#include "web_providers4.h"
#include "web_providers5.h"
#include "windsurf.h"
#include "xai.h"
#include "zai.h"
#include "zed.h"
#include "zoommate.h"

#include <gio/gio.h>
#include <json-c/json.h>
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
                                       const char *account_label,
                                       int account_index,
                                       gboolean all_accounts,
                                       GCancellable *cancellable,
                                       GError **error) {
    const char *argv[16] = {backend, "usage", "--format", "json", NULL};
    char account_index_text[32] = {0};
    guint argument = 4;
    if (provider) {
        argv[argument++] = "--provider";
        argv[argument++] = provider;
    }
    if (source) {
        argv[argument++] = "--source";
        argv[argument++] = source;
    }
    if (account_label) {
        argv[argument++] = "--account";
        argv[argument++] = account_label;
    } else if (account_index >= 0) {
        g_snprintf(account_index_text, sizeof(account_index_text), "%d", account_index + 1);
        argv[argument++] = "--account-index";
        argv[argument++] = account_index_text;
    } else if (all_accounts) {
        argv[argument++] = "--all-accounts";
    }
    argv[argument] = NULL;

    CodexBarProcessRequest request = {
        .arguments = argv,
        .timeout_milliseconds = ORACLE_TIMEOUT_MILLISECONDS,
        .termination_grace_milliseconds = ORACLE_TERMINATION_GRACE_MILLISECONDS,
        .maximum_output_bytes = ORACLE_MAXIMUM_OUTPUT_BYTES,
        .new_session = TRUE,
    };
    CodexBarProcessResult *result = codexbar_process_run(&request, cancellable, error);
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
    } else if (error && (strstr(error->message, "malformed") || strstr(error->message, "Failed to parse") ||
                         strstr(error->message, "Could not parse Wayfinder gateway response") ||
                         strstr(error->message, "Invalid backend JSON") ||
                         strstr(error->message, "response parse error"))) {
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

static CodexBarProvider *fetch_provider(const CodexBarProviderConfig *config, GCancellable *cancellable) {
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
    case CODEXBAR_NATIVE_KIRO:
    case CODEXBAR_NATIVE_AUGMENT:
    case CODEXBAR_NATIVE_ANTIGRAVITY:
        native_source = "cli";
        break;
    case CODEXBAR_NATIVE_CLAUDE:
    case CODEXBAR_NATIVE_VERTEX:
        native_source = "oauth";
        break;
    case CODEXBAR_NATIVE_OPENCODE_GO:
        native_source = "auto";
        break;
    case CODEXBAR_NATIVE_QWEN_CLOUD:
    case CODEXBAR_NATIVE_ZOOMMATE:
    case CODEXBAR_NATIVE_CURSOR:
    case CODEXBAR_NATIVE_OPENCODE:
    case CODEXBAR_NATIVE_DEVIN:
    case CODEXBAR_NATIVE_MANUS:
    case CODEXBAR_NATIVE_T3CHAT:
    case CODEXBAR_NATIVE_SAKANA:
    case CODEXBAR_NATIVE_ABACUS:
    case CODEXBAR_NATIVE_MISTRAL:
    case CODEXBAR_NATIVE_COMMANDCODE:
    case CODEXBAR_NATIVE_QODER:
    case CODEXBAR_NATIVE_PERPLEXITY:
    case CODEXBAR_NATIVE_LONGCAT:
    case CODEXBAR_NATIVE_ALIBABA_TOKEN_PLAN:
    case CODEXBAR_NATIVE_MIMO:
    case CODEXBAR_NATIVE_WINDSURF:
    case CODEXBAR_NATIVE_STEPFUN:
        native_source = "web";
        break;
    case CODEXBAR_NATIVE_COPILOT:
    case CODEXBAR_NATIVE_AZURE_OPENAI:
    case CODEXBAR_NATIVE_CLINEPASS:
    case CODEXBAR_NATIVE_DEEPINFRA:
    case CODEXBAR_NATIVE_AIAND:
    case CODEXBAR_NATIVE_NEURALWATT:
    case CODEXBAR_NATIVE_WAYFINDER:
    case CODEXBAR_NATIVE_ZAI:
    case CODEXBAR_NATIVE_OPENAI:
    case CODEXBAR_NATIVE_CODEBUFF:
    case CODEXBAR_NATIVE_KIMI:
    case CODEXBAR_NATIVE_OPENROUTER:
    case CODEXBAR_NATIVE_PROXY:
    case CODEXBAR_NATIVE_SIMPLE:
    case CODEXBAR_NATIVE_XAI:
    case CODEXBAR_NATIVE_DEEPGRAM:
    case CODEXBAR_NATIVE_POE:
    case CODEXBAR_NATIVE_CHUTES:
    case CODEXBAR_NATIVE_SYNTHETIC:
    case CODEXBAR_NATIVE_WARP:
    case CODEXBAR_NATIVE_GROQ:
    case CODEXBAR_NATIVE_MINIMAX:
    case CODEXBAR_NATIVE_ALIBABA:
    case CODEXBAR_NATIVE_DOUBAO:
    case CODEXBAR_NATIVE_FACTORY:
    case CODEXBAR_NATIVE_GEMINI:
    case CODEXBAR_NATIVE_OLLAMA:
    case CODEXBAR_NATIVE_LITELLM:
    case CODEXBAR_NATIVE_SUB2API:
    case CODEXBAR_NATIVE_BEDROCK:
    case CODEXBAR_NATIVE_ZED:
        native_source = "api";
        break;
    case CODEXBAR_NATIVE_KILO:
    case CODEXBAR_NATIVE_AMP:
    case CODEXBAR_NATIVE_GROK:
        native_source = configured_source;
        break;
    case CODEXBAR_NATIVE_UNAVAILABLE:
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
    case CODEXBAR_NATIVE_AZURE_OPENAI:
        provider = codexbar_azure_openai_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_CODEX:
        provider = codexbar_codex_fetch(&error);
        break;
    case CODEXBAR_NATIVE_CLAUDE:
        provider = codexbar_claude_fetch(config, &error);
        break;
    case CODEXBAR_NATIVE_CLINEPASS:
        provider = codexbar_clinepass_fetch(config, &error);
        break;
    case CODEXBAR_NATIVE_COPILOT:
        provider = codexbar_copilot_fetch(config, &error);
        break;
    case CODEXBAR_NATIVE_DEEPINFRA:
        provider = codexbar_deepinfra_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_AIAND:
        provider = codexbar_aiand_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_NEURALWATT:
        provider = codexbar_neuralwatt_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_WAYFINDER:
        provider = codexbar_wayfinder_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_ZAI:
        provider = codexbar_zai_fetch(config, &error);
        break;
    case CODEXBAR_NATIVE_OPENAI:
        provider = codexbar_openai_api_fetch(config, &error);
        break;
    case CODEXBAR_NATIVE_KILO:
        provider = codexbar_kilo_fetch(config, configured_source, &error);
        break;
    case CODEXBAR_NATIVE_CODEBUFF:
        provider = codexbar_codebuff_fetch(config, &error);
        break;
    case CODEXBAR_NATIVE_JETBRAINS:
        provider = codexbar_jetbrains_fetch(&error);
        break;
    case CODEXBAR_NATIVE_OPENCODE_GO:
        provider = codexbar_opencode_go_fetch(&error);
        break;
    case CODEXBAR_NATIVE_KIMI:
        provider = codexbar_kimi_fetch(config, &error);
        break;
    case CODEXBAR_NATIVE_QWEN_CLOUD:
        provider = codexbar_qwen_cloud_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_ZOOMMATE:
        provider = codexbar_zoommate_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_XAI:
        provider = codexbar_xai_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_DEEPGRAM:
        provider = codexbar_deepgram_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_POE:
        provider = codexbar_poe_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_CHUTES:
        provider = codexbar_chutes_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_SYNTHETIC:
        provider = codexbar_synthetic_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_WARP:
        provider = codexbar_warp_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_GROQ:
        provider = codexbar_groq_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_MINIMAX:
        provider = codexbar_minimax_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_ALIBABA:
        provider = codexbar_alibaba_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_DOUBAO:
        provider = codexbar_doubao_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_FACTORY:
        provider = codexbar_factory_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_GEMINI:
        provider = codexbar_gemini_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_OLLAMA:
        provider = codexbar_ollama_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_KIRO:
        provider = codexbar_kiro_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_AUGMENT:
        provider = codexbar_augment_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_ANTIGRAVITY:
        provider = codexbar_antigravity_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_CURSOR:
        provider = codexbar_cursor_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_OPENCODE:
        provider = codexbar_opencode_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_DEVIN:
        provider = codexbar_devin_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_MANUS:
        provider = codexbar_manus_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_AMP:
        provider = codexbar_amp_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_T3CHAT:
        provider = codexbar_t3chat_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_SAKANA:
        provider = codexbar_sakana_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_ABACUS:
        provider = codexbar_abacus_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_MISTRAL:
        provider = codexbar_mistral_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_COMMANDCODE:
        provider = codexbar_commandcode_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_QODER:
        provider = codexbar_qoder_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_PERPLEXITY:
        provider = codexbar_perplexity_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_LONGCAT:
        provider = codexbar_longcat_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_ALIBABA_TOKEN_PLAN:
        provider = codexbar_alibaba_token_plan_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_MIMO:
        provider = codexbar_mimo_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_ZED:
        provider = codexbar_zed_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_VERTEX:
        provider = codexbar_vertex_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_WINDSURF:
        provider = codexbar_windsurf_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_GROK:
        provider = codexbar_grok_fetch_with_cancellable(config, configured_source, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_STEPFUN:
        provider = codexbar_stepfun_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_LITELLM:
        provider = codexbar_litellm_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_SUB2API:
        provider = codexbar_sub2api_fetch_with_cancellable(config, cancellable, &error);
        break;
    case CODEXBAR_NATIVE_BEDROCK:
        provider = codexbar_bedrock_fetch_with_cancellable(config, cancellable, &error);
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
    const char *error_source = descriptor->native_provider == CODEXBAR_NATIVE_JETBRAINS ||
                                        descriptor->native_provider == CODEXBAR_NATIVE_KILO ||
                                        descriptor->native_provider == CODEXBAR_NATIVE_AZURE_OPENAI
                                   ? configured_source
                                   : native_source ? native_source : configured_source;
    return provider ? provider : provider_error(config, error_source, error);
}

static gboolean append_managed_codex_accounts(CodexBarSnapshot *snapshot,
                                              const CodexBarProviderConfig *config,
                                              GCancellable *cancellable,
                                              GError **fatal_error) {
    GError *error = NULL;
    CodexBarManagedCodexStore *store = codexbar_managed_codex_store_load(FALSE, &error);
    if (!store) {
        g_ptr_array_add(snapshot->providers, provider_error(config, "cli", error));
        return TRUE;
    }
    GPtrArray *accounts = codexbar_managed_codex_accounts(store);
    for (guint index = 0; index < accounts->len; index++) {
        if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, fatal_error)) {
            codexbar_managed_codex_store_free(store);
            return FALSE;
        }
        CodexBarManagedCodexAccount *account = g_ptr_array_index(accounts, index);
        error = NULL;
        CodexBarProvider *provider = codexbar_codex_fetch_with_home(account->managed_home_path, &error);
        if (!provider) provider = provider_error(config, "cli", error);
        g_free(provider->account);
        provider->account = g_strdup(account->email);
        if (!provider->identity) provider->identity = g_new0(CodexBarProviderIdentity, 1);
        g_free(provider->identity->account_id);
        g_free(provider->identity->organization);
        g_free(provider->identity->login_method);
        provider->identity->account_id = g_strdup(account->provider_account_id);
        provider->identity->organization = g_strdup(account->workspace_label);
        provider->identity->login_method = g_strdup("Managed Codex account");
        g_ptr_array_add(snapshot->providers, provider);
    }
    codexbar_managed_codex_store_free(store);
    return TRUE;
}

CodexBarSnapshot *codexbar_backend_fetch(GError **error) {
    return codexbar_backend_fetch_with_cancellable(NULL, error);
}

CodexBarSnapshot *codexbar_backend_fetch_with_cancellable(GCancellable *cancellable, GError **error) {
    const char *backend = g_getenv("CODEXBAR_BACKEND");
    if (backend && backend[0] != '\0') {
        return fetch_oracle(backend, NULL, NULL, NULL, -1, FALSE, cancellable, error);
    }

    CodexBarConfig *config = codexbar_config_load(error);
    if (!config) {
        return NULL;
    }
    CodexBarSnapshot *snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    for (guint index = 0; index < config->providers->len; index++) {
        if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) {
            codexbar_snapshot_free(snapshot);
            codexbar_config_free(config);
            return NULL;
        }
        CodexBarProviderConfig *provider_config = g_ptr_array_index(config->providers, index);
        if (!provider_config->enabled) {
            continue;
        }
        CodexBarProvider *provider = fetch_provider(provider_config, cancellable);
        if (cancellable && g_cancellable_set_error_if_cancelled(cancellable, error)) {
            codexbar_provider_free(provider);
            codexbar_snapshot_free(snapshot);
            codexbar_config_free(config);
            return NULL;
        }
        g_ptr_array_add(snapshot->providers, provider);
        if (g_str_equal(provider_config->id, "codex") &&
            !append_managed_codex_accounts(snapshot, provider_config, cancellable, error)) {
            codexbar_snapshot_free(snapshot);
            codexbar_config_free(config);
            return NULL;
        }
    }
    codexbar_config_free(config);
    return snapshot;
}

CodexBarSnapshot *codexbar_backend_fetch_all(GError **error) {
    const char *backend = g_getenv("CODEXBAR_BACKEND");
    if (backend && backend[0] != '\0') return fetch_oracle(backend, "all", NULL, NULL, -1, FALSE, NULL, error);
    CodexBarConfig *config = codexbar_config_load(error);
    if (!config) return NULL;
    CodexBarSnapshot *snapshot = g_new0(CodexBarSnapshot, 1);
    snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
    for (guint index = 0; index < codexbar_provider_registry_count(); index++) {
        const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_at(index);
        CodexBarProviderConfig *provider_config = codexbar_config_provider(config, descriptor->id);
        g_ptr_array_add(snapshot->providers, fetch_provider(provider_config, NULL));
        if (g_str_equal(provider_config->id, "codex")) {
            append_managed_codex_accounts(snapshot, provider_config, NULL, NULL);
        }
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
        CodexBarProviderConfig selected = {.id = g_strdup(descriptor->id)};
        GError *source_error = g_error_new(G_IO_ERROR,
                                           G_IO_ERROR_NOT_SUPPORTED,
                                           "Source '%s' is not supported for %s.",
                                           source,
                                           descriptor->cli_name);
        CodexBarProvider *result = provider_error(&selected, source, source_error);
        g_free(selected.id);
        return result;
    }
    const char *backend = g_getenv("CODEXBAR_BACKEND");
    if (backend && backend[0] != '\0') {
        CodexBarSnapshot *snapshot = fetch_oracle(backend, descriptor->cli_name, source, NULL, -1, FALSE, NULL, error);
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
    selected.source = g_strdup(source ? source : stored->source);
    CodexBarProvider *result = fetch_provider(&selected, NULL);
    g_free(selected.source);
    codexbar_config_free(config);
    return result;
}

static CodexBarProvider *fetch_token_account(const CodexBarProviderConfig *stored,
                                             json_object *account,
                                             const char *source,
                                             GError **error) {
    char *label = codexbar_token_account_string(account, "label");
    char *token = codexbar_token_account_string(account, "token");
    if (!label || !token || !codexbar_token_account_token_is_safe(token)) {
        g_free(label);
        g_free(token);
        g_set_error_literal(error,
                            G_IO_ERROR,
                            G_IO_ERROR_INVALID_DATA,
                            "Token account requires non-empty label and token.");
        return NULL;
    }

    CodexBarProviderConfig selected = *stored;
    selected.source = g_strdup(source ? source : stored->source);
    selected.api_key = stored->api_key;
    selected.raw = stored->raw ? json_tokener_parse(json_object_to_json_string_ext(stored->raw, JSON_C_TO_STRING_PLAIN))
                               : json_object_new_object();
    if (!selected.raw) selected.raw = json_object_new_object();
    json_object_object_add(selected.raw, "tokenAccountSelected", json_object_new_boolean(TRUE));

    char *account_api_key = NULL;
    char *usage_scope = codexbar_token_account_string(account, "usageScope");
    char *organization_id = codexbar_token_account_string(account, "organizationId");
    char *workspace_id = codexbar_token_account_string(account, "workspaceID");
    if (usage_scope) json_object_object_add(selected.raw, "usageScope", json_object_new_string(usage_scope));
    if (organization_id) {
        json_object_object_add(selected.raw, "organizationId", json_object_new_string(organization_id));
    }
    if (workspace_id) json_object_object_add(selected.raw, "workspaceID", json_object_new_string(workspace_id));

    if (codexbar_token_accounts_use_cookie(stored->id)) {
        char *cookie = NULL;
        if (g_str_equal(stored->id, "manus") && !strchr(token, '=') && !g_str_has_prefix(token, "Cookie:")) {
            cookie = g_strdup_printf("session_id=%s", token);
        } else if (g_str_equal(stored->id, "claude") && !strchr(token, '=') &&
                   !g_str_has_prefix(token, "Cookie:") && !g_str_has_prefix(token, "sk-ant-oat")) {
            cookie = g_strdup_printf("sessionKey=%s", token);
        } else {
            cookie = g_strdup(token);
        }
        json_object_object_add(selected.raw, "cookieSource", json_object_new_string("manual"));
        json_object_object_add(selected.raw, "cookieHeader", json_object_new_string(cookie));
        if (g_str_equal(stored->id, "claude") && g_str_has_prefix(token, "sk-ant-oat")) {
            json_object_object_add(selected.raw, "oauthToken", json_object_new_string(token));
            g_free(selected.source);
            selected.source = g_strdup("oauth");
        }
        g_free(cookie);
    } else {
        account_api_key = g_strdup(token);
        selected.api_key = account_api_key;
    }

    CodexBarProvider *provider = fetch_provider(&selected, NULL);
    if (provider) {
        g_free(provider->account);
        provider->account = g_strdup(label);
    }
    json_object_put(selected.raw);
    g_free(account_api_key);
    g_free(selected.source);
    g_free(usage_scope);
    g_free(organization_id);
    g_free(workspace_id);
    g_free(token);
    g_free(label);
    return provider;
}

static void mark_token_account_used(const char *provider_id, json_object *selected_account) {
    char *account_id = codexbar_token_account_string(selected_account, "id");
    if (!account_id) return;
    GError *error = NULL;
    CodexBarConfig *config = codexbar_config_load_for_update(&error);
    if (!config) {
        g_clear_error(&error);
        g_free(account_id);
        return;
    }
    CodexBarProviderConfig *provider = codexbar_config_provider(config, provider_id);
    int unused = 0;
    json_object *accounts = codexbar_token_accounts_array(provider, &unused);
    for (guint index = 0; accounts && index < json_object_array_length(accounts); index++) {
        json_object *account = json_object_array_get_idx(accounts, index);
        char *id = codexbar_token_account_string(account, "id");
        gboolean match = id && g_str_equal(id, account_id);
        g_free(id);
        if (!match) continue;
        json_object_object_add(account, "lastUsed", json_object_new_double((double)g_get_real_time() / G_USEC_PER_SEC));
        codexbar_config_save(config, &error);
        break;
    }
    codexbar_config_free(config);
    g_clear_error(&error);
    g_free(account_id);
}

CodexBarSnapshot *codexbar_backend_fetch_selected_accounts(const char *provider_name,
                                                            const char *source,
                                                            const char *account_label,
                                                            int account_index,
                                                            gboolean all_accounts,
                                                            GError **error) {
    const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(provider_name);
    if (!descriptor) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unknown provider: %s", provider_name);
        return NULL;
    }
    const char *backend = g_getenv("CODEXBAR_BACKEND");
    if (backend && backend[0] != '\0') {
        return fetch_oracle(backend,
                            descriptor->cli_name,
                            source,
                            account_label,
                            account_index,
                            all_accounts,
                            NULL,
                            error);
    }
    if (!codexbar_token_accounts_supported(descriptor->id)) {
        g_set_error(error,
                    G_IO_ERROR,
                    G_IO_ERROR_NOT_SUPPORTED,
                    "Token accounts are not supported for %s.",
                    descriptor->display_name);
        return NULL;
    }

    CodexBarConfig *config = codexbar_config_load(error);
    if (!config) return NULL;
    CodexBarProviderConfig *stored = codexbar_config_provider(config, descriptor->id);
    int active_index = 0;
    json_object *accounts = codexbar_token_accounts_array(stored, &active_index);
    size_t count = accounts ? json_object_array_length(accounts) : 0;
    if (count == 0) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "No token accounts configured for %s.", descriptor->id);
        codexbar_config_free(config);
        return NULL;
    }

    GArray *indices = g_array_new(FALSE, FALSE, sizeof(size_t));
    if (all_accounts) {
        for (size_t index = 0; index < count; index++) g_array_append_val(indices, index);
    } else if (account_label) {
        for (size_t index = 0; index < count; index++) {
            char *label = codexbar_token_account_string(json_object_array_get_idx(accounts, index), "label");
            gboolean matches = label && g_ascii_strcasecmp(label, account_label) == 0;
            g_free(label);
            if (matches) {
                g_array_append_val(indices, index);
                break;
            }
        }
        if (indices->len == 0) {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_NOT_FOUND,
                        "No token account labeled '%s' for %s.",
                        account_label,
                        descriptor->id);
        }
    } else {
        int selected = account_index >= 0 ? account_index : CLAMP(active_index, 0, (int)count - 1);
        if (selected < 0 || (size_t)selected >= count) {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_INVALID_ARGUMENT,
                        "Token account index %d out of range for %s (1-%zu).",
                        selected + 1,
                        descriptor->id,
                        count);
        } else {
            size_t index = (size_t)selected;
            g_array_append_val(indices, index);
        }
    }

    CodexBarSnapshot *snapshot = NULL;
    if (indices->len > 0) {
        snapshot = g_new0(CodexBarSnapshot, 1);
        snapshot->providers = g_ptr_array_new_with_free_func((GDestroyNotify)codexbar_provider_free);
        for (guint position = 0; position < indices->len; position++) {
            size_t index = g_array_index(indices, size_t, position);
            CodexBarProvider *result = fetch_token_account(stored,
                                                          json_object_array_get_idx(accounts, index),
                                                          source,
                                                          error);
            if (!result) {
                codexbar_snapshot_free(snapshot);
                snapshot = NULL;
                break;
            }
            g_ptr_array_add(snapshot->providers, result);
            mark_token_account_used(descriptor->id, json_object_array_get_idx(accounts, index));
            if (all_accounts && g_str_equal(descriptor->id, "neuralwatt") && position + 1 < indices->len) {
                g_usleep(G_USEC_PER_SEC);
            }
        }
    }
    g_array_unref(indices);
    codexbar_config_free(config);
    return snapshot;
}
