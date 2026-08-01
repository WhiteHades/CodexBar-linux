#pragma once

#include "config.h"

gboolean codexbar_token_accounts_supported(const char *provider);
gboolean codexbar_token_accounts_use_cookie(const char *provider);
json_object *codexbar_token_accounts_array(const CodexBarProviderConfig *config, int *active_index);
char *codexbar_token_account_string(json_object *account, const char *key);
gboolean codexbar_token_account_token_is_safe(const char *token);
gboolean codexbar_token_account_is_selected(const CodexBarProviderConfig *config);

gboolean codexbar_token_accounts_add(CodexBarProviderConfig *config,
                                     const char *label,
                                     const char *token,
                                     const char *external_identifier,
                                     const char *usage_scope,
                                     const char *organization_id,
                                     const char *workspace_id,
                                     char **created_id,
                                     GError **error);
gboolean codexbar_token_accounts_update(CodexBarProviderConfig *config,
                                        const char *selector,
                                        const char *label,
                                        const char *token,
                                        const char *external_identifier,
                                        const char *usage_scope,
                                        const char *organization_id,
                                        const char *workspace_id,
                                        GError **error);
gboolean codexbar_token_accounts_remove(CodexBarProviderConfig *config,
                                        const char *selector,
                                        GError **error);
gboolean codexbar_token_accounts_select(CodexBarProviderConfig *config,
                                        const char *selector,
                                        GError **error);
gboolean codexbar_token_accounts_move(CodexBarProviderConfig *config,
                                      const char *selector,
                                      guint destination,
                                      GError **error);
