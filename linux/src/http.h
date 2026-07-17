#pragma once

#include <glib.h>

typedef struct {
    long status;
    char *body;
} CodexBarHttpResponse;

CodexBarHttpResponse *codexbar_http_get(const char *url, const char *bearer_token, GError **error);
void codexbar_http_response_free(CodexBarHttpResponse *response);
