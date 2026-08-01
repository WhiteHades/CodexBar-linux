#pragma once

#include "process.h"

#include <glib.h>

typedef enum {
    CODEXBAR_SESSION_CODEX,
    CODEXBAR_SESSION_CLAUDE,
} CodexBarSessionProvider;

typedef enum {
    CODEXBAR_SESSION_CLI,
    CODEXBAR_SESSION_DESKTOP,
    CODEXBAR_SESSION_IDE,
    CODEXBAR_SESSION_UNKNOWN,
} CodexBarSessionSource;

typedef struct {
    char *id;
    CodexBarSessionProvider provider;
    CodexBarSessionSource source;
    gboolean active;
    gboolean has_pid;
    gint64 pid;
    char *cwd;
    char *project_name;
    char *session_name;
    gboolean has_started_at;
    gint64 started_at;
    gboolean has_last_activity_at;
    gint64 last_activity_at;
    char *transcript_path;
    char *host;
} CodexBarAgentSession;

typedef struct {
    char *host;
    GPtrArray *sessions;
    char *error;
} CodexBarRemoteSessionHostResult;

typedef CodexBarProcessResult *(*CodexBarSessionProcessRunner)(const CodexBarProcessRequest *request,
                                                              GCancellable *cancellable,
                                                              GError **error);

GPtrArray *codexbar_sessions_scan(GError **error);
GPtrArray *codexbar_tailscale_status_parse_hosts(const char *json,
                                                 size_t length,
                                                 const char *local_host);
GPtrArray *codexbar_session_hosts_sanitize(const char *const *hosts, size_t count);
GPtrArray *codexbar_session_hosts_from_csv(const char *csv);
GPtrArray *codexbar_remote_sessions_discover(CodexBarSessionProcessRunner runner,
                                             GCancellable *cancellable);
GPtrArray *codexbar_remote_sessions_fetch(const char *const *hosts,
                                          size_t count,
                                          CodexBarSessionProcessRunner runner,
                                          GCancellable *cancellable);
GPtrArray *codexbar_remote_sessions_parse(const char *json,
                                          size_t length,
                                          const char *host,
                                          GError **error);
gboolean codexbar_remote_session_focus(const char *session_id,
                                       const char *host,
                                       CodexBarSessionProcessRunner runner,
                                       GCancellable *cancellable,
                                       GError **error);
void codexbar_agent_session_free(CodexBarAgentSession *session);
void codexbar_remote_session_host_result_free(CodexBarRemoteSessionHostResult *result);
const char *codexbar_session_provider_name(CodexBarSessionProvider provider);
const char *codexbar_session_source_name(CodexBarSessionSource source);
