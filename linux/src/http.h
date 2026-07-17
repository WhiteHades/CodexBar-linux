#pragma once

#include <glib.h>

typedef struct {
    const char *name;
    const char *value;
} CodexBarHttpRequestHeader;

typedef struct {
    char *name;
    char *value;
} CodexBarHttpResponseHeader;

typedef enum {
    CODEXBAR_HTTP_HTTPS_ONLY,
    CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP,
} CodexBarHttpProtocolPolicy;

typedef enum {
    CODEXBAR_HTTP_REDIRECT_DENY,
    CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN,
} CodexBarHttpRedirectPolicy;

typedef struct {
    const char *url;
    const char *method;
    const CodexBarHttpRequestHeader *headers;
    size_t header_count;
    const void *body;
    size_t body_length;
    long timeout_seconds;
    size_t maximum_response_bytes;
    CodexBarHttpProtocolPolicy protocol_policy;
    CodexBarHttpRedirectPolicy redirect_policy;
} CodexBarHttpRequest;

typedef struct {
    long status;
    char *body;
    size_t body_length;
    GPtrArray *headers;
    char *effective_url;
} CodexBarHttpResponse;

CodexBarHttpResponse *codexbar_http_send(const CodexBarHttpRequest *request, GError **error);
CodexBarHttpResponse *codexbar_http_get(
    const char *url, const char *auth_header, const char *auth_value, GError **error);
const char *codexbar_http_response_header_first(const CodexBarHttpResponse *response, const char *name);
char *codexbar_http_normalize_endpoint(
    const char *raw, CodexBarHttpProtocolPolicy protocol_policy, GError **error);
void codexbar_http_response_free(CodexBarHttpResponse *response);
