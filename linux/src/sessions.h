#pragma once

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
    gboolean has_started_at;
    gint64 started_at;
    gboolean has_last_activity_at;
    gint64 last_activity_at;
    char *transcript_path;
    char *host;
} CodexBarAgentSession;

GPtrArray *codexbar_sessions_scan(GError **error);
void codexbar_agent_session_free(CodexBarAgentSession *session);
const char *codexbar_session_provider_name(CodexBarSessionProvider provider);
const char *codexbar_session_source_name(CodexBarSessionSource source);
