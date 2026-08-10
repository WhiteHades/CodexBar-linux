#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarNotionTransport)(const CodexBarHttpRequest *request, GError **error);

CodexBarProvider *codexbar_notion_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                        CodexBarNotionTransport transport,
                                                                        GCancellable *cancellable,
                                                                        gint64 now_ms,
                                                                        GError **error);
CodexBarProvider *codexbar_notion_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error);
CodexBarProvider *codexbar_notion_fetch(const CodexBarProviderConfig *config, GError **error);
