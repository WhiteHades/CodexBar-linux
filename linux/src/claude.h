#pragma once

#include "config.h"
#include "model.h"

CodexBarProvider *codexbar_claude_fetch(const CodexBarProviderConfig *config, GError **error);
CodexBarProvider *codexbar_claude_parse_oauth_usage(const char *json,
                                                    const char *rate_limit_tier,
                                                    const char *subscription_type,
                                                    gint64 updated_at_ms,
                                                    GError **error);
