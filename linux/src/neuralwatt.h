#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarNeuralWattTransport)(const CodexBarHttpRequest *request, GError **error);

gboolean codexbar_neuralwatt_has_api_key(const CodexBarProviderConfig *config);
char *codexbar_neuralwatt_quota_url_for_testing(const char *override, GError **error);
double codexbar_neuralwatt_retry_delay_for_testing(const CodexBarHttpResponse *response);
CodexBarProvider *codexbar_neuralwatt_parse_usage_bytes(
    const char *json, size_t length, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_neuralwatt_parse_usage(const char *json, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_neuralwatt_fetch_with_transport_and_cancellable(const CodexBarProviderConfig *config,
                                                                           CodexBarNeuralWattTransport transport,
                                                                           GCancellable *cancellable,
                                                                           gint64 now_ms,
                                                                           GError **error);
CodexBarProvider *codexbar_neuralwatt_fetch_with_transport(const CodexBarProviderConfig *config,
                                                            CodexBarNeuralWattTransport transport,
                                                            gint64 now_ms,
                                                            GError **error);
CodexBarProvider *codexbar_neuralwatt_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                              GCancellable *cancellable,
                                                              GError **error);
CodexBarProvider *codexbar_neuralwatt_fetch(const CodexBarProviderConfig *config, GError **error);
