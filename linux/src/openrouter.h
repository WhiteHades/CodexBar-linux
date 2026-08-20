#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarOpenRouterTransport)(const CodexBarHttpRequest *request,
                                                            GError **error);

CodexBarProvider *codexbar_openrouter_fetch(const CodexBarProviderConfig *config, GError **error);
CodexBarProvider *codexbar_openrouter_fetch_with_transport(const CodexBarProviderConfig *config,
                                                           CodexBarOpenRouterTransport transport,
                                                           GError **error);
CodexBarProvider *codexbar_openrouter_parse_credits(const char *json, GError **error);
