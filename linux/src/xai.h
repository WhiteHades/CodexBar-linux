#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarXAITransport)(const CodexBarHttpRequest *request, GError **error);

gboolean codexbar_xai_has_api_key(const CodexBarProviderConfig *config);
gboolean codexbar_xai_has_credentials(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_xai_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                    CodexBarXAITransport transport,
                                                                    GCancellable *cancellable,
                                                                    gint64 now_ms,
                                                                    GError **error);
CodexBarProvider *codexbar_xai_fetch_with_transport(const CodexBarProviderConfig *config,
                                                    CodexBarXAITransport transport,
                                                    gint64 now_ms,
                                                    GError **error);
CodexBarProvider *codexbar_xai_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                      GCancellable *cancellable,
                                                      GError **error);
CodexBarProvider *codexbar_xai_fetch(const CodexBarProviderConfig *config, GError **error);
