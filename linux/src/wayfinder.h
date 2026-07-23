#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarWayfinderTransport)(const CodexBarHttpRequest *request, GError **error);

char *codexbar_wayfinder_base_url_for_testing(const CodexBarProviderConfig *config, GError **error);
char *codexbar_wayfinder_dashboard_url_for_testing(const CodexBarProviderConfig *config, GError **error);
char *codexbar_wayfinder_endpoint_for_testing(const char *base_url, const char *path, const char *query, GError **error);
double codexbar_wayfinder_average_decision_ms_for_testing(const char *text, gboolean *present);
CodexBarProvider *codexbar_wayfinder_parse_usage(const char *health,
                                                  const char *models,
                                                  const char *savings,
                                                  const char *metrics,
                                                  gint64 now_ms,
                                                  GError **error);
CodexBarProvider *codexbar_wayfinder_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                           CodexBarWayfinderTransport transport,
                                                                           GCancellable *cancellable,
                                                                           gint64 now_ms,
                                                                           GError **error);
CodexBarProvider *codexbar_wayfinder_fetch_with_transport(const CodexBarProviderConfig *config,
                                                           CodexBarWayfinderTransport transport,
                                                           gint64 now_ms,
                                                           GError **error);
CodexBarProvider *codexbar_wayfinder_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                             GCancellable *cancellable,
                                                             GError **error);
CodexBarProvider *codexbar_wayfinder_fetch(const CodexBarProviderConfig *config, GError **error);
