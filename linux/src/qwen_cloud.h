#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarQwenCloudTransport)(const CodexBarHttpRequest *request, GError **error);

gboolean codexbar_qwen_cloud_has_cookie(const CodexBarProviderConfig *config);
CodexBarProvider *codexbar_qwen_cloud_parse_usage(const char *usage_json,
                                                  const char *subscription_json,
                                                  const char *quota_config_json,
                                                  gint64 now_ms,
                                                  GError **error);
CodexBarProvider *codexbar_qwen_cloud_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarQwenCloudTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_qwen_cloud_fetch_with_transport(const CodexBarProviderConfig *config,
                                                           CodexBarQwenCloudTransport transport,
                                                           gint64 now_ms,
                                                           GError **error);
CodexBarProvider *codexbar_qwen_cloud_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                             GCancellable *cancellable,
                                                             GError **error);
CodexBarProvider *codexbar_qwen_cloud_fetch(const CodexBarProviderConfig *config, GError **error);
