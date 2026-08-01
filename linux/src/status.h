#pragma once

#include "http.h"
#include "model.h"

typedef struct CodexBarStatusComponent {
    char *id;
    char *name;
    CodexBarServiceStatusIndicator indicator;
    char *status;
    GPtrArray *children;
} CodexBarStatusComponent;

typedef struct {
    CodexBarServiceStatus *status;
    GPtrArray *components;
} CodexBarStatusSummary;

typedef CodexBarHttpResponse *(*CodexBarStatusTransport)(const CodexBarHttpRequest *request,
                                                        GError **error);

CodexBarServiceStatus *codexbar_statuspage_parse_status(const char *json,
                                                        size_t length,
                                                        GError **error);
GPtrArray *codexbar_statuspage_parse_components(const char *json, size_t length, GError **error);
CodexBarStatusSummary *codexbar_incident_io_parse_summary(const char *json,
                                                          size_t length,
                                                          GError **error);
CodexBarServiceStatus *codexbar_workspace_parse_status(const char *json,
                                                       size_t length,
                                                       const char *product_id,
                                                       GError **error);

CodexBarStatusSummary *codexbar_statuspage_fetch_summary(const char *base_url,
                                                         CodexBarStatusTransport transport,
                                                         GCancellable *cancellable,
                                                         GError **error);
CodexBarServiceStatus *codexbar_workspace_fetch_status(const char *product_id,
                                                       CodexBarStatusTransport transport,
                                                       GCancellable *cancellable,
                                                       GError **error);

void codexbar_status_component_free(CodexBarStatusComponent *component);
void codexbar_status_summary_free(CodexBarStatusSummary *summary);
