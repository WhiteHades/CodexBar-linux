#pragma once

#include <glib.h>

typedef enum {
    CODEXBAR_SOURCE_AUTO = 1U << 0,
    CODEXBAR_SOURCE_WEB = 1U << 1,
    CODEXBAR_SOURCE_CLI = 1U << 2,
    CODEXBAR_SOURCE_OAUTH = 1U << 3,
    CODEXBAR_SOURCE_API = 1U << 4,
} CodexBarProviderSource;

typedef enum {
    CODEXBAR_NATIVE_UNAVAILABLE,
    CODEXBAR_NATIVE_CODEX,
    CODEXBAR_NATIVE_CLAUDE,
    CODEXBAR_NATIVE_COPILOT,
    CODEXBAR_NATIVE_ZAI,
    CODEXBAR_NATIVE_CODEBUFF,
    CODEXBAR_NATIVE_JETBRAINS,
    CODEXBAR_NATIVE_KIMI,
    CODEXBAR_NATIVE_KIMI_K2,
    CODEXBAR_NATIVE_OPENROUTER,
    CODEXBAR_NATIVE_PROXY,
    CODEXBAR_NATIVE_SIMPLE,
} CodexBarNativeProvider;

typedef struct {
    const char *id;
    const char *display_name;
    const char *cli_name;
    const char *aliases;
    guint source_modes;
    gboolean default_enabled;
    const char *dashboard_url;
    const char *status_url;
    CodexBarNativeProvider native_provider;
} CodexBarProviderDescriptor;

guint codexbar_provider_registry_count(void);
const CodexBarProviderDescriptor *codexbar_provider_registry_at(guint index);
const CodexBarProviderDescriptor *codexbar_provider_registry_find(const char *name);
gboolean codexbar_provider_supports_source(const CodexBarProviderDescriptor *provider, const char *source);
gboolean codexbar_provider_status_is_pollable(const CodexBarProviderDescriptor *provider);
gboolean codexbar_provider_supports_config_api_key(const CodexBarProviderDescriptor *provider);
