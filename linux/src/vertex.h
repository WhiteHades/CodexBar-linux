#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarVertexTransport)(const CodexBarHttpRequest *request,
                                                        GError **error);
typedef char *(*CodexBarVertexTokenRunner)(GCancellable *cancellable, GError **error);

CodexBarProvider *codexbar_vertex_fetch_with_adapters(const CodexBarProviderConfig *config,
                                                      CodexBarVertexTransport transport,
                                                      CodexBarVertexTokenRunner token_runner,
                                                      GCancellable *cancellable,
                                                      gint64 now_ms,
                                                      GError **error);
CodexBarProvider *codexbar_vertex_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                         GCancellable *cancellable,
                                                         GError **error);
