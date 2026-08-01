#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarGrokTransport)(const CodexBarHttpRequest *request,
                                                      GError **error);
typedef char *(*CodexBarGrokRunner)(const char *binary,
                                   const char *input,
                                   GCancellable *cancellable,
                                   GError **error);

CodexBarProvider *codexbar_grok_parse_billing(const char *json,
                                              size_t length,
                                              gint64 now_ms,
                                              GError **error);
CodexBarProvider *codexbar_grok_parse_web_billing(const guint8 *data,
                                                  size_t length,
                                                  gint64 now_ms,
                                                  GError **error);
CodexBarProvider *codexbar_grok_fetch_with_adapters(const CodexBarProviderConfig *config,
                                                    const char *source,
                                                    CodexBarGrokTransport transport,
                                                    CodexBarGrokRunner runner,
                                                    GCancellable *cancellable,
                                                    gint64 now_ms,
                                                    GError **error);
CodexBarProvider *codexbar_grok_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                       const char *source,
                                                       GCancellable *cancellable,
                                                       GError **error);
