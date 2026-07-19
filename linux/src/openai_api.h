#pragma once

#include "config.h"
#include "model.h"

char *codexbar_openai_api_url(const char *endpoint,
                              gint64 start_time,
                              gint64 end_time,
                              const char *group_by,
                              const char *project_id,
                              const char *page);
CodexBarProvider *codexbar_openai_api_parse_usage(const char *costs_json,
                                                  const char *completions_json,
                                                  gint64 now_seconds,
                                                  const char *project_id,
                                                  GError **error);
CodexBarProvider *codexbar_openai_api_fetch(const CodexBarProviderConfig *config, GError **error);
