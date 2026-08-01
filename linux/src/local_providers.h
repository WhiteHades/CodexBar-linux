#pragma once

#include "config.h"
#include "http.h"
#include "model.h"

typedef CodexBarHttpResponse *(*CodexBarLocalProviderTransport)(const CodexBarHttpRequest *request,
                                                                GError **error);

CodexBarProvider *codexbar_kiro_parse_usage(const char *usage_output,
                                            const char *account_output,
                                            const char *context_output,
                                            gint64 now_ms,
                                            GError **error);
CodexBarProvider *codexbar_kiro_fetch_with_binary_and_cancellable(const char *binary,
                                                                  GCancellable *cancellable,
                                                                  GError **error);
CodexBarProvider *codexbar_kiro_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                        GCancellable *cancellable,
                                                        GError **error);
CodexBarProvider *codexbar_kiro_fetch(const CodexBarProviderConfig *config, GError **error);

CodexBarProvider *codexbar_augment_parse_cli_usage(const char *output, gint64 now_ms, GError **error);
CodexBarProvider *codexbar_augment_fetch_with_binary_and_cancellable(const char *binary,
                                                                     GCancellable *cancellable,
                                                                     GError **error);
CodexBarProvider *codexbar_augment_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                           GCancellable *cancellable,
                                                           GError **error);
CodexBarProvider *codexbar_augment_fetch(const CodexBarProviderConfig *config, GError **error);

CodexBarProvider *codexbar_antigravity_parse_usage(const char *quota_summary_json,
                                                    const char *user_status_json,
                                                    const char *command_model_json,
                                                    gint64 now_ms,
                                                    GError **error);
GArray *codexbar_antigravity_parse_listening_ports(const char *proc_net_tcp,
                                                    const char *const *socket_inodes,
                                                    size_t socket_inode_count);
CodexBarProvider *codexbar_antigravity_fetch_ports_with_transport_and_cancellable(
    const guint16 *ports,
    size_t port_count,
    const char *csrf_token,
    CodexBarLocalProviderTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_antigravity_fetch_with_transport_and_cancellable(
    const CodexBarProviderConfig *config,
    CodexBarLocalProviderTransport transport,
    GCancellable *cancellable,
    gint64 now_ms,
    GError **error);
CodexBarProvider *codexbar_antigravity_fetch_with_cancellable(const CodexBarProviderConfig *config,
                                                               GCancellable *cancellable,
                                                               GError **error);
CodexBarProvider *codexbar_antigravity_fetch(const CodexBarProviderConfig *config, GError **error);
