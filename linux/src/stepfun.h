#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarStepFunTransport)(const CodexBarHttpRequest *request,
                                                          GError **error);

CodexBarProvider *codexbar_stepfun_parse_usage(const char *json,
                                               size_t length,
                                               gint64 now_ms,
                                               GError **error);
gboolean codexbar_stepfun_apply_plan(CodexBarProvider *provider,
                                     const char *json,
                                     size_t length);
CodexBarProvider *codexbar_stepfun_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarStepFunTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_stepfun_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                          GCancellable *cancellable,
                                                          GError **error);
