#include "token_accounts.h"

#include <json-c/json.h>
#include <string.h>

#define TOKEN_LIMIT 65536U

static GQuark token_account_error_quark(void) {
    return g_quark_from_static_string("codexbar-token-account-error");
}

static gboolean id_in(const char *id, const char *const *ids, guint count) {
    for (guint index = 0; index < count; index++) {
        if (g_str_equal(id, ids[index])) return TRUE;
    }
    return FALSE;
}

gboolean codexbar_token_accounts_use_cookie(const char *provider) {
    static const char *const providers[] = {
        "claude", "cursor", "opencode", "opencodego", "factory", "minimax", "manus", "augment",
        "ollama", "abacus", "mistral", "qoder", "stepfun",
    };
    return provider && id_in(provider, providers, G_N_ELEMENTS(providers));
}

gboolean codexbar_token_accounts_supported(const char *provider) {
    static const char *const providers[] = {
        "openai",   "openrouter", "claude",     "deepseek", "deepinfra", "antigravity", "zai",
        "cursor",   "opencode",   "opencodego", "factory",  "minimax",   "manus",       "augment",
        "ollama",   "abacus",     "mistral",    "qoder",    "copilot",   "venice",      "elevenlabs",
        "neuralwatt", "groq",     "llmproxy",   "litellm",  "sub2api",   "stepfun",
    };
    return provider && id_in(provider, providers, G_N_ELEMENTS(providers));
}

char *codexbar_token_account_string(json_object *account, const char *key) {
    json_object *value = NULL;
    if (!account || !json_object_is_type(account, json_type_object) ||
        !json_object_object_get_ex(account, key, &value) || !json_object_is_type(value, json_type_string)) {
        return NULL;
    }
    const char *text = json_object_get_string(value);
    size_t length = (size_t)json_object_get_string_len(value);
    if (!length || length > TOKEN_LIMIT || memchr(text, '\0', length) || !g_utf8_validate(text, (gssize)length, NULL)) {
        return NULL;
    }
    char *clean = g_strstrip(g_strndup(text, length));
    if (clean[0]) return clean;
    g_free(clean);
    return NULL;
}

gboolean codexbar_token_account_token_is_safe(const char *token) {
    if (!token || !token[0] || strlen(token) > TOKEN_LIMIT || !g_utf8_validate(token, -1, NULL)) return FALSE;
    for (const unsigned char *cursor = (const unsigned char *)token; *cursor; cursor++) {
        if (g_ascii_iscntrl(*cursor)) return FALSE;
    }
    return TRUE;
}

gboolean codexbar_token_account_is_selected(const CodexBarProviderConfig *config) {
    json_object *selected = NULL;
    return config && config->raw && json_object_object_get_ex(config->raw, "tokenAccountSelected", &selected) &&
           json_object_is_type(selected, json_type_boolean) && json_object_get_boolean(selected);
}

json_object *codexbar_token_accounts_array(const CodexBarProviderConfig *config, int *active_index) {
    json_object *data = NULL;
    json_object *accounts = NULL;
    if (active_index) *active_index = 0;
    if (!config || !config->raw || !json_object_object_get_ex(config->raw, "tokenAccounts", &data) ||
        !json_object_is_type(data, json_type_object) ||
        !json_object_object_get_ex(data, "accounts", &accounts) ||
        !json_object_is_type(accounts, json_type_array)) return NULL;
    json_object *active = NULL;
    if (active_index && json_object_object_get_ex(data, "activeIndex", &active) &&
        json_object_is_type(active, json_type_int)) *active_index = json_object_get_int(active);
    return accounts;
}

static char *clean_field(const char *value) {
    if (!value) return NULL;
    size_t length = strlen(value);
    if (length > TOKEN_LIMIT || !g_utf8_validate(value, -1, NULL)) return NULL;
    char *copy = g_strstrip(g_strdup(value));
    for (const unsigned char *cursor = (const unsigned char *)copy; *cursor; cursor++) {
        if (g_ascii_iscntrl(*cursor)) {
            g_free(copy);
            return NULL;
        }
    }
    if (copy[0]) return copy;
    g_free(copy);
    return NULL;
}

static json_object *ensure_data(CodexBarProviderConfig *config) {
    if (!config->raw) config->raw = json_object_new_object();
    json_object *data = NULL;
    if (!json_object_object_get_ex(config->raw, "tokenAccounts", &data) ||
        !json_object_is_type(data, json_type_object)) {
        data = json_object_new_object();
        json_object_object_add(config->raw, "tokenAccounts", data);
    }
    json_object *accounts = NULL;
    if (!json_object_object_get_ex(data, "accounts", &accounts) ||
        !json_object_is_type(accounts, json_type_array)) {
        accounts = json_object_new_array();
        json_object_object_add(data, "accounts", accounts);
    }
    json_object_object_add(data, "version", json_object_new_int(1));
    return data;
}

static json_object *data_accounts(json_object *data) {
    json_object *accounts = NULL;
    json_object_object_get_ex(data, "accounts", &accounts);
    return accounts;
}

static gboolean selector_index(json_object *accounts, const char *selector, guint *result) {
    if (!selector || !selector[0]) return FALSE;
    char *end = NULL;
    guint64 one_based = g_ascii_strtoull(selector, &end, 10);
    if (end && *end == '\0' && one_based > 0 && one_based <= json_object_array_length(accounts)) {
        *result = (guint)(one_based - 1);
        return TRUE;
    }
    for (guint index = 0; index < json_object_array_length(accounts); index++) {
        json_object *account = json_object_array_get_idx(accounts, index);
        char *id = codexbar_token_account_string(account, "id");
        char *label = codexbar_token_account_string(account, "label");
        gboolean match = (id && g_ascii_strcasecmp(id, selector) == 0) ||
                         (label && g_ascii_strcasecmp(label, selector) == 0);
        g_free(id);
        g_free(label);
        if (match) {
            *result = index;
            return TRUE;
        }
    }
    return FALSE;
}

static void set_optional(json_object *account, const char *key, const char *value) {
    char *clean = clean_field(value);
    if (clean) json_object_object_add(account, key, json_object_new_string(clean));
    g_free(clean);
}

gboolean codexbar_token_accounts_add(CodexBarProviderConfig *config,
                                     const char *label,
                                     const char *token,
                                     const char *external_identifier,
                                     const char *usage_scope,
                                     const char *organization_id,
                                     const char *workspace_id,
                                     char **created_id,
                                     GError **error) {
    if (!config || !codexbar_token_accounts_supported(config->id)) {
        g_set_error(error, token_account_error_quark(), 1, "Token accounts are not supported for %s",
                    config ? config->id : "<missing>");
        return FALSE;
    }
    char *clean_token = clean_field(token);
    if (!codexbar_token_account_token_is_safe(clean_token)) {
        g_free(clean_token);
        g_set_error_literal(error, token_account_error_quark(), 2,
                            "Token must be non-empty and contain no control characters");
        return FALSE;
    }
    json_object *data = ensure_data(config);
    json_object *accounts = data_accounts(data);
    char *clean_label = clean_field(label);
    if (!clean_label) clean_label = g_strdup_printf("Account %zu", json_object_array_length(accounts) + 1);
    char *id = g_uuid_string_random();
    json_object *account = json_object_new_object();
    json_object_object_add(account, "id", json_object_new_string(id));
    json_object_object_add(account, "label", json_object_new_string(clean_label));
    json_object_object_add(account, "token", json_object_new_string(clean_token));
    json_object_object_add(account, "addedAt", json_object_new_double((double)g_get_real_time() / G_USEC_PER_SEC));
    set_optional(account, "externalIdentifier", external_identifier);
    set_optional(account, "usageScope", usage_scope);
    set_optional(account, "organizationId", organization_id);
    set_optional(account, "workspaceID", workspace_id);
    json_object_array_add(accounts, account);
    json_object_object_add(data, "activeIndex", json_object_new_int((int)json_object_array_length(accounts) - 1));
    if (codexbar_token_accounts_use_cookie(config->id)) {
        json_object_object_add(config->raw, "cookieSource", json_object_new_string("manual"));
    }
    if (g_str_equal(config->id, "copilot")) g_clear_pointer(&config->api_key, g_free);
    if (created_id) *created_id = g_strdup(id);
    g_free(id);
    g_free(clean_label);
    g_free(clean_token);
    return TRUE;
}

static void replace_optional(json_object *account, const char *key, const char *value) {
    if (!value) return;
    char *clean = clean_field(value);
    if (clean) json_object_object_add(account, key, json_object_new_string(clean));
    else json_object_object_del(account, key);
    g_free(clean);
}

gboolean codexbar_token_accounts_update(CodexBarProviderConfig *config,
                                        const char *selector,
                                        const char *label,
                                        const char *token,
                                        const char *external_identifier,
                                        const char *usage_scope,
                                        const char *organization_id,
                                        const char *workspace_id,
                                        GError **error) {
    int active = 0;
    json_object *accounts = codexbar_token_accounts_array(config, &active);
    guint index = 0;
    if (!accounts || !selector_index(accounts, selector, &index)) {
        g_set_error(error, token_account_error_quark(), 3, "Token account '%s' was not found", selector);
        return FALSE;
    }
    json_object *account = json_object_array_get_idx(accounts, index);
    if (token) {
        char *clean = clean_field(token);
        gboolean safe = codexbar_token_account_token_is_safe(clean);
        if (!safe) {
            g_free(clean);
            g_set_error_literal(error, token_account_error_quark(), 2,
                                "Token must be non-empty and contain no control characters");
            return FALSE;
        }
        json_object_object_add(account, "token", json_object_new_string(clean));
        g_free(clean);
    }
    if (label) {
        char *clean = clean_field(label);
        if (!clean) {
            g_set_error_literal(error, token_account_error_quark(), 2, "Account label must not be empty");
            return FALSE;
        }
        json_object_object_add(account, "label", json_object_new_string(clean));
        g_free(clean);
    }
    replace_optional(account, "externalIdentifier", external_identifier);
    replace_optional(account, "usageScope", usage_scope);
    replace_optional(account, "organizationId", organization_id);
    replace_optional(account, "workspaceID", workspace_id);
    if (g_str_equal(config->id, "copilot")) g_clear_pointer(&config->api_key, g_free);
    return TRUE;
}

gboolean codexbar_token_accounts_remove(CodexBarProviderConfig *config,
                                        const char *selector,
                                        GError **error) {
    int active = 0;
    json_object *accounts = codexbar_token_accounts_array(config, &active);
    guint index = 0;
    if (!accounts || !selector_index(accounts, selector, &index)) {
        g_set_error(error, token_account_error_quark(), 3, "Token account '%s' was not found", selector);
        return FALSE;
    }
    json_object_array_del_idx(accounts, index, 1);
    size_t count = json_object_array_length(accounts);
    if (!count) {
        json_object_object_del(config->raw, "tokenAccounts");
    } else {
        json_object *data = NULL;
        json_object_object_get_ex(config->raw, "tokenAccounts", &data);
        int next = active == (int)index ? MIN((int)index, (int)count - 1)
                                       : active > (int)index ? active - 1 : active;
        json_object_object_add(data, "activeIndex", json_object_new_int(CLAMP(next, 0, (int)count - 1)));
    }
    if (g_str_equal(config->id, "copilot")) g_clear_pointer(&config->api_key, g_free);
    return TRUE;
}

gboolean codexbar_token_accounts_select(CodexBarProviderConfig *config,
                                        const char *selector,
                                        GError **error) {
    int unused = 0;
    json_object *accounts = codexbar_token_accounts_array(config, &unused);
    guint index = 0;
    if (!accounts || !selector_index(accounts, selector, &index)) {
        g_set_error(error, token_account_error_quark(), 3, "Token account '%s' was not found", selector);
        return FALSE;
    }
    json_object *data = NULL;
    json_object_object_get_ex(config->raw, "tokenAccounts", &data);
    json_object_object_add(data, "activeIndex", json_object_new_int((int)index));
    if (codexbar_token_accounts_use_cookie(config->id)) {
        json_object_object_add(config->raw, "cookieSource", json_object_new_string("manual"));
    }
    return TRUE;
}

gboolean codexbar_token_accounts_move(CodexBarProviderConfig *config,
                                      const char *selector,
                                      guint destination,
                                      GError **error) {
    int active = 0;
    json_object *accounts = codexbar_token_accounts_array(config, &active);
    guint source = 0;
    size_t count = accounts ? json_object_array_length(accounts) : 0;
    if (!accounts || !selector_index(accounts, selector, &source) || destination >= count) {
        g_set_error(error, token_account_error_quark(), 3,
                    "Token account or destination index is out of range");
        return FALSE;
    }
    if (source == destination) return TRUE;
    json_object *moved = json_object_get(json_object_array_get_idx(accounts, source));
    json_object_array_del_idx(accounts, source, 1);
    json_object_array_insert_idx(accounts, destination, moved);
    int next_active = active;
    if (active == (int)source) next_active = (int)destination;
    else if (source < destination && active > (int)source && active <= (int)destination) next_active--;
    else if (destination < source && active >= (int)destination && active < (int)source) next_active++;
    json_object *data = NULL;
    json_object_object_get_ex(config->raw, "tokenAccounts", &data);
    json_object_object_add(data, "activeIndex", json_object_new_int(next_active));
    return TRUE;
}
