#pragma once

#include <glib.h>

#define CODEXBAR_MANAGED_CODEX_UNREADABLE_HOME "/managed-store-unreadable"

typedef struct {
    char *id;
    char *email;
    char *provider_account_id;
    char *workspace_label;
    char *workspace_account_id;
    char *auth_fingerprint;
    char *managed_home_path;
    double created_at;
    double updated_at;
    double last_authenticated_at;
    gboolean has_last_authenticated_at;
} CodexBarManagedCodexAccount;

typedef struct CodexBarManagedCodexStore CodexBarManagedCodexStore;

char *codexbar_managed_codex_store_path(void);
char *codexbar_managed_codex_root_path(void);
CodexBarManagedCodexStore *codexbar_managed_codex_store_load(gboolean for_update, GError **error);
gboolean codexbar_managed_codex_store_save(CodexBarManagedCodexStore *store, GError **error);
GPtrArray *codexbar_managed_codex_accounts(const CodexBarManagedCodexStore *store);
const char *codexbar_managed_codex_store_file(const CodexBarManagedCodexStore *store);
const char *codexbar_managed_codex_root(const CodexBarManagedCodexStore *store);
char *codexbar_managed_codex_create_home(CodexBarManagedCodexStore *store, GError **error);
gboolean codexbar_managed_codex_discard_home(CodexBarManagedCodexStore *store,
                                             const char *path,
                                             GError **error);
CodexBarManagedCodexAccount *codexbar_managed_codex_find(CodexBarManagedCodexStore *store,
                                                         const char *selector);
CodexBarManagedCodexAccount *codexbar_managed_codex_import(CodexBarManagedCodexStore *store,
                                                           const char *auth_file,
                                                           GError **error);
CodexBarManagedCodexAccount *codexbar_managed_codex_reauthenticate(CodexBarManagedCodexStore *store,
                                                                   const char *selector,
                                                                   const char *auth_file,
                                                                   GError **error);
gboolean codexbar_managed_codex_remove(CodexBarManagedCodexStore *store,
                                       const char *selector,
                                       GError **error);
void codexbar_managed_codex_store_free(CodexBarManagedCodexStore *store);
