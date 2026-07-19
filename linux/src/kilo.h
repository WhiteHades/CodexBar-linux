#pragma once

#include "config.h"
#include "model.h"

char *codexbar_kilo_usage_url(void);
char *codexbar_kilo_parse_auth_token(const char *json, GError **error);
char *codexbar_kilo_load_cli_token(const char *home_directory, GError **error);
CodexBarProvider *codexbar_kilo_parse_usage(
    const char *json, const char *source, const char *organization_id, GError **error);
CodexBarProvider *codexbar_kilo_fetch(
    const CodexBarProviderConfig *config, const char *source, GError **error);
