#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarZoomMateTransport)(const CodexBarHttpRequest *request, GError **error);

CodexBarProvider *codexbar_zoommate_parse_usage(const char *json, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_zoommate_parse_usage_bytes(
    const char *json, size_t length, gint64 now_ms, GError **error);
gboolean codexbar_zoommate_capture_is_valid_for_testing(const char *capture);
CodexBarProvider *codexbar_zoommate_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                         CodexBarZoomMateTransport transport,
                                                                         GCancellable *cancellable,
                                                                         gint64 now_ms,
                                                                         GError **error);
CodexBarProvider *codexbar_zoommate_fetch_with_transport(const CodexBarProviderConfig *config,
                                                          CodexBarZoomMateTransport transport,
                                                          gint64 now_ms,
                                                          GError **error);
CodexBarProvider *codexbar_zoommate_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                            GCancellable *cancellable,
                                                            GError **error);
CodexBarProvider *codexbar_zoommate_fetch(const CodexBarProviderConfig *config, GError **error);
