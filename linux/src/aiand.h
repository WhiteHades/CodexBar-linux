#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarAiAndTransport)(const CodexBarHttpRequest *request, GError **error);

gboolean codexbar_aiand_has_api_key(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_aiand_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                      CodexBarAiAndTransport transport,
                                                                      GCancellable *cancellable,
                                                                      gint64 now_ms,
                                                                      GError **error);
CodexBarProvider *codexbar_aiand_fetch_with_transport(const CodexBarProviderConfig *config,
                                                      CodexBarAiAndTransport transport,
                                                      gint64 now_ms,
                                                      GError **error);
CodexBarProvider *codexbar_aiand_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                        GCancellable *cancellable,
                                                        GError **error);
CodexBarProvider *codexbar_aiand_fetch(const CodexBarProviderConfig *config, GError **error);
