#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarAzureOpenAITransport)(const CodexBarHttpRequest *request,
                                                              GError **error);

gboolean codexbar_azure_openai_has_api_key(const CodexBarProviderConfig *config);
char *codexbar_azure_openai_endpoint_for_testing(const char *raw, GError **error);
char *codexbar_azure_openai_chat_url_for_testing(const char *endpoint,
                                                  const char *deployment,
                                                  const char *api_version,
                                                  GError **error);
CodexBarProvider *codexbar_azure_openai_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarAzureOpenAITransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_azure_openai_fetch_with_transport(const CodexBarProviderConfig *config,
                                                              CodexBarAzureOpenAITransport transport,
                                                              gint64 now_ms,
                                                              GError **error);
CodexBarProvider *codexbar_azure_openai_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                                GCancellable *cancellable,
                                                                GError **error);
CodexBarProvider *codexbar_azure_openai_fetch(const CodexBarProviderConfig *config, GError **error);
