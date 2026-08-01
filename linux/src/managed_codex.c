#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif

#include "managed_codex.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <glib/gstdio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

enum {
    MANAGED_CODEX_VERSION = 3,
    MAX_AUTH_BYTES = 1024 * 1024,
};

struct CodexBarManagedCodexStore {
    char *path;
    char *root;
    GPtrArray *accounts;
    int lock_fd;
};

static GQuark managed_error_quark(void) {
    return g_quark_from_static_string("codexbar-managed-codex-error");
}

static char *data_base(void) {
    const char *xdg = g_getenv("XDG_DATA_HOME");
    if (xdg && g_path_is_absolute(xdg)) return g_strdup(xdg);
    return g_build_filename(g_get_home_dir(), ".local", "share", NULL);
}

char *codexbar_managed_codex_store_path(void) {
    const char *override = g_getenv("CODEXBAR_MANAGED_CODEX_STORE");
    if (override && g_path_is_absolute(override)) return g_canonicalize_filename(override, NULL);
    char *base = data_base();
    char *path = g_build_filename(base, "codexbar", "managed-codex-accounts.json", NULL);
    g_free(base);
    return path;
}

char *codexbar_managed_codex_root_path(void) {
    const char *override = g_getenv("CODEXBAR_MANAGED_CODEX_ROOT");
    if (override && g_path_is_absolute(override)) return g_canonicalize_filename(override, NULL);
    char *base = data_base();
    char *path = g_build_filename(base, "codexbar", "managed-codex-homes", NULL);
    g_free(base);
    return path;
}

static char *normalized_string(json_object *object, const char *key, gboolean lowercase) {
    json_object *value = NULL;
    if (!json_object_object_get_ex(object, key, &value) || !json_object_is_type(value, json_type_string)) return NULL;
    char *result = g_strdup(json_object_get_string(value));
    g_strstrip(result);
    if (result[0] == '\0') {
        g_free(result);
        return NULL;
    }
    if (lowercase) {
        char *folded = g_utf8_strdown(result, -1);
        g_free(result);
        result = folded;
    }
    return result;
}

static void account_free(gpointer data) {
    CodexBarManagedCodexAccount *account = data;
    if (!account) return;
    g_free(account->id);
    g_free(account->email);
    g_free(account->provider_account_id);
    g_free(account->workspace_label);
    g_free(account->workspace_account_id);
    g_free(account->auth_fingerprint);
    g_free(account->managed_home_path);
    g_free(account);
}

static char *base64url_decode(const char *value) {
    char *encoded = g_strdup(value);
    for (char *cursor = encoded; *cursor; cursor++) {
        if (*cursor == '-') *cursor = '+';
        if (*cursor == '_') *cursor = '/';
    }
    size_t length = strlen(encoded);
    size_t padded_length = (length + 3) & ~(size_t)3;
    encoded = g_realloc(encoded, padded_length + 1);
    while (length < padded_length) encoded[length++] = '=';
    encoded[length] = '\0';
    gsize decoded_length = 0;
    guchar *decoded = g_base64_decode(encoded, &decoded_length);
    g_free(encoded);
    if (!decoded || memchr(decoded, '\0', decoded_length)) {
        g_free(decoded);
        return NULL;
    }
    char *result = g_strndup((const char *)decoded, decoded_length);
    g_free(decoded);
    return result;
}

static json_object *jwt_payload(const char *token) {
    if (!token) return NULL;
    char **parts = g_strsplit(token, ".", 3);
    char *decoded = parts[0] && parts[1] ? base64url_decode(parts[1]) : NULL;
    g_strfreev(parts);
    if (!decoded) return NULL;
    json_object *payload = json_tokener_parse(decoded);
    g_free(decoded);
    if (!payload || !json_object_is_type(payload, json_type_object)) {
        if (payload) json_object_put(payload);
        return NULL;
    }
    return payload;
}

static char *auth_string(json_object *root, json_object *tokens, const char *camel, const char *snake) {
    char *value = tokens ? normalized_string(tokens, camel, FALSE) : NULL;
    if (!value && tokens) value = normalized_string(tokens, snake, FALSE);
    if (!value) value = normalized_string(root, camel, FALSE);
    if (!value) value = normalized_string(root, snake, FALSE);
    return value;
}

static gboolean auth_identity(const char *contents, char **email, char **account_id, GError **error) {
    *email = NULL;
    *account_id = NULL;
    json_object *root = json_tokener_parse(contents);
    if (!root || !json_object_is_type(root, json_type_object)) {
        if (root) json_object_put(root);
        g_set_error_literal(error, managed_error_quark(), 5, "Codex auth file is not valid JSON");
        return FALSE;
    }
    json_object *tokens = NULL;
    if (!json_object_object_get_ex(root, "tokens", &tokens) || !json_object_is_type(tokens, json_type_object)) {
        tokens = root;
    }
    char *id_token = auth_string(root, tokens, "idToken", "id_token");
    *email = auth_string(root, tokens, "email", "email");
    *account_id = auth_string(root, tokens, "accountId", "account_id");
    json_object *payload = jwt_payload(id_token);
    if (payload) {
        if (!*email) *email = normalized_string(payload, "email", TRUE);
        json_object *auth = NULL;
        if (json_object_object_get_ex(payload, "https://api.openai.com/auth", &auth) &&
            json_object_is_type(auth, json_type_object)) {
            if (!*account_id) *account_id = normalized_string(auth, "chatgpt_account_id", TRUE);
        }
        if (!*account_id) *account_id = normalized_string(payload, "chatgpt_account_id", TRUE);
        json_object_put(payload);
    }
    g_free(id_token);
    json_object_put(root);
    if (*email) {
        char *folded = g_utf8_strdown(*email, -1);
        g_free(*email);
        *email = folded;
    }
    if (*account_id) {
        char *folded = g_utf8_strdown(*account_id, -1);
        g_free(*account_id);
        *account_id = folded;
    }
    if (!*email) {
        g_free(*account_id);
        *account_id = NULL;
        g_set_error_literal(error, managed_error_quark(), 6, "Codex auth file contains no account email");
        return FALSE;
    }
    return TRUE;
}

static char *fingerprint(const char *contents, gsize length) {
    return g_compute_checksum_for_data(G_CHECKSUM_SHA256, (const guchar *)contents, length);
}

static gboolean read_auth_file(const char *path, char **contents, gsize *length, GError **error) {
    GStatBuf status;
    if (g_lstat(path, &status) != 0 || !S_ISREG(status.st_mode) || status.st_size < 0 ||
        status.st_size > MAX_AUTH_BYTES) {
        g_set_error_literal(
            error, managed_error_quark(), 4, "Codex auth file must be a regular file no larger than 1 MiB");
        return FALSE;
    }
    return g_file_get_contents(path, contents, length, error);
}

static CodexBarManagedCodexAccount *parse_account(json_object *object) {
    CodexBarManagedCodexAccount *account = g_new0(CodexBarManagedCodexAccount, 1);
    account->id = normalized_string(object, "id", TRUE);
    account->email = normalized_string(object, "email", TRUE);
    account->provider_account_id = normalized_string(object, "providerAccountID", TRUE);
    account->workspace_label = normalized_string(object, "workspaceLabel", FALSE);
    account->workspace_account_id = normalized_string(object, "workspaceAccountID", TRUE);
    account->auth_fingerprint = normalized_string(object, "authFingerprint", TRUE);
    account->managed_home_path = normalized_string(object, "managedHomePath", FALSE);
    json_object *created = NULL;
    json_object *updated = NULL;
    json_object *authenticated = NULL;
    gboolean valid = account->id && g_uuid_string_is_valid(account->id) && account->email &&
                     account->managed_home_path && g_path_is_absolute(account->managed_home_path) &&
                     json_object_object_get_ex(object, "createdAt", &created) &&
                     json_object_object_get_ex(object, "updatedAt", &updated);
    account->created_at = valid ? json_object_get_double(created) : 0;
    account->updated_at = valid ? json_object_get_double(updated) : 0;
    valid = valid && isfinite(account->created_at) && isfinite(account->updated_at);
    if (valid && json_object_object_get_ex(object, "lastAuthenticatedAt", &authenticated) &&
        !json_object_is_type(authenticated, json_type_null)) {
        account->last_authenticated_at = json_object_get_double(authenticated);
        account->has_last_authenticated_at = isfinite(account->last_authenticated_at);
        valid = account->has_last_authenticated_at;
    }
    if (valid) return account;
    account_free(account);
    return NULL;
}

static gboolean duplicate_account(GPtrArray *accounts, CodexBarManagedCodexAccount *candidate) {
    for (guint index = 0; index < accounts->len; index++) {
        CodexBarManagedCodexAccount *account = g_ptr_array_index(accounts, index);
        if (g_str_equal(account->id, candidate->id)) return TRUE;
        if (candidate->provider_account_id && account->provider_account_id &&
            g_str_equal(account->email, candidate->email) &&
            g_str_equal(account->provider_account_id, candidate->provider_account_id)) {
            return TRUE;
        }
        if (!candidate->provider_account_id && !account->provider_account_id &&
            g_str_equal(account->email, candidate->email)) {
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean acquire_lock(CodexBarManagedCodexStore *store, GError **error) {
    char *lock_path = g_strconcat(store->path, ".lock", NULL);
    char *directory = g_path_get_dirname(lock_path);
    if (g_mkdir_with_parents(directory, 0700) != 0 || g_chmod(directory, 0700) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not secure %s", directory);
        g_free(directory);
        g_free(lock_path);
        return FALSE;
    }
    g_free(directory);
    store->lock_fd = g_open(lock_path, O_RDWR | O_CREAT, 0600);
    g_free(lock_path);
    if (store->lock_fd >= 0 && flock(store->lock_fd, LOCK_EX) == 0) return TRUE;
    g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not lock managed Codex accounts");
    return FALSE;
}

CodexBarManagedCodexStore *codexbar_managed_codex_store_load(gboolean for_update, GError **error) {
    CodexBarManagedCodexStore *store = g_new0(CodexBarManagedCodexStore, 1);
    store->path = codexbar_managed_codex_store_path();
    store->root = codexbar_managed_codex_root_path();
    store->accounts = g_ptr_array_new_with_free_func(account_free);
    store->lock_fd = -1;
    if (for_update && !acquire_lock(store, error)) {
        codexbar_managed_codex_store_free(store);
        return NULL;
    }
    if (!g_file_test(store->path, G_FILE_TEST_EXISTS)) return store;
    char *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(store->path, &contents, &length, error)) {
        codexbar_managed_codex_store_free(store);
        return NULL;
    }
    json_object *root = json_tokener_parse(contents);
    g_free(contents);
    json_object *version = NULL;
    json_object *accounts = NULL;
    gboolean valid = root && json_object_is_type(root, json_type_object) &&
                     json_object_object_get_ex(root, "version", &version) &&
                     json_object_is_type(version, json_type_int) &&
                     json_object_object_get_ex(root, "accounts", &accounts) &&
                     json_object_is_type(accounts, json_type_array);
    int stored_version = valid ? json_object_get_int(version) : 0;
    if (!valid || stored_version < 1 || stored_version > MANAGED_CODEX_VERSION) {
        if (valid) {
            g_set_error(error,
                        managed_error_quark(),
                        1,
                        "Unsupported managed Codex account version: %d",
                        stored_version);
        } else {
            g_set_error_literal(error, managed_error_quark(), 1, "Managed Codex account store is malformed");
        }
        if (root) json_object_put(root);
        codexbar_managed_codex_store_free(store);
        return NULL;
    }
    for (size_t index = 0; index < json_object_array_length(accounts); index++) {
        json_object *entry = json_object_array_get_idx(accounts, index);
        CodexBarManagedCodexAccount *account =
            json_object_is_type(entry, json_type_object) ? parse_account(entry) : NULL;
        if (!account) {
            g_set_error(error, managed_error_quark(), 3, "Managed Codex account %zu is malformed", index);
            json_object_put(root);
            codexbar_managed_codex_store_free(store);
            return NULL;
        }
        if (stored_version < MANAGED_CODEX_VERSION) {
            char *auth_path = g_build_filename(account->managed_home_path, "auth.json", NULL);
            char *auth = NULL;
            gsize auth_length = 0;
            if (read_auth_file(auth_path, &auth, &auth_length, NULL)) {
                if (!account->auth_fingerprint) account->auth_fingerprint = fingerprint(auth, auth_length);
                if (!account->provider_account_id) {
                    char *email = NULL;
                    char *provider_id = NULL;
                    if (auth_identity(auth, &email, &provider_id, NULL)) account->provider_account_id = provider_id;
                    else g_free(provider_id);
                    g_free(email);
                }
                g_free(auth);
            }
            g_free(auth_path);
        }
        if (duplicate_account(store->accounts, account)) {
            account_free(account);
            continue;
        }
        g_ptr_array_add(store->accounts, account);
    }
    json_object_put(root);
    return store;
}

static json_object *account_json(const CodexBarManagedCodexAccount *account) {
    json_object *object = json_object_new_object();
    if (account->auth_fingerprint) {
        json_object_object_add(object, "authFingerprint", json_object_new_string(account->auth_fingerprint));
    }
    json_object_object_add(object, "createdAt", json_object_new_double(account->created_at));
    json_object_object_add(object, "email", json_object_new_string(account->email));
    json_object_object_add(object, "id", json_object_new_string(account->id));
    json_object_object_add(object,
                           "lastAuthenticatedAt",
                           account->has_last_authenticated_at
                               ? json_object_new_double(account->last_authenticated_at)
                               : NULL);
    json_object_object_add(object, "managedHomePath", json_object_new_string(account->managed_home_path));
    if (account->provider_account_id) {
        json_object_object_add(object, "providerAccountID", json_object_new_string(account->provider_account_id));
    }
    json_object_object_add(object, "updatedAt", json_object_new_double(account->updated_at));
    if (account->workspace_account_id) {
        json_object_object_add(object, "workspaceAccountID", json_object_new_string(account->workspace_account_id));
    }
    if (account->workspace_label) {
        json_object_object_add(object, "workspaceLabel", json_object_new_string(account->workspace_label));
    }
    return object;
}

gboolean codexbar_managed_codex_store_save(CodexBarManagedCodexStore *store, GError **error) {
    g_return_val_if_fail(store != NULL, FALSE);
    if (store->lock_fd < 0) {
        g_set_error_literal(error, managed_error_quark(), 2, "Managed Codex store must be opened for update");
        return FALSE;
    }
    json_object *root = json_object_new_object();
    json_object *accounts = json_object_new_array_ext((int)store->accounts->len);
    for (guint index = 0; index < store->accounts->len; index++) {
        json_object_array_add(accounts, account_json(g_ptr_array_index(store->accounts, index)));
    }
    json_object_object_add(root, "accounts", accounts);
    json_object_object_add(root, "version", json_object_new_int(MANAGED_CODEX_VERSION));
    const char *json = json_object_to_json_string_ext(root, JSON_C_TO_STRING_PRETTY);
    char *contents = g_strconcat(json, "\n", NULL);
    char *directory = g_path_get_dirname(store->path);
    gboolean saved = g_mkdir_with_parents(directory, 0700) == 0 && g_chmod(directory, 0700) == 0;
    if (!saved) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not secure %s", directory);
    } else {
        saved = g_file_set_contents_full(store->path,
                                         contents,
                                         -1,
                                         G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
                                         0600,
                                         error);
        if (saved && g_chmod(store->path, 0600) != 0) {
            g_set_error(error,
                        G_FILE_ERROR,
                        g_file_error_from_errno(errno),
                        "Could not secure managed Codex account store");
            saved = FALSE;
        }
    }
    g_free(directory);
    g_free(contents);
    json_object_put(root);
    return saved;
}

GPtrArray *codexbar_managed_codex_accounts(const CodexBarManagedCodexStore *store) {
    return store ? store->accounts : NULL;
}

const char *codexbar_managed_codex_store_file(const CodexBarManagedCodexStore *store) {
    return store ? store->path : NULL;
}

const char *codexbar_managed_codex_root(const CodexBarManagedCodexStore *store) {
    return store ? store->root : NULL;
}

CodexBarManagedCodexAccount *codexbar_managed_codex_find(CodexBarManagedCodexStore *store,
                                                         const char *selector) {
    if (!store || !selector) return NULL;
    char *normalized = g_utf8_strdown(selector, -1);
    CodexBarManagedCodexAccount *match = NULL;
    for (guint index = 0; index < store->accounts->len; index++) {
        CodexBarManagedCodexAccount *account = g_ptr_array_index(store->accounts, index);
        if (g_str_equal(account->id, normalized) || g_str_equal(account->email, normalized)) {
            if (match) {
                match = NULL;
                break;
            }
            match = account;
        }
    }
    g_free(normalized);
    return match;
}

static gboolean safe_home(const char *root, const char *path) {
    char *canonical_root = g_canonicalize_filename(root, NULL);
    char *canonical_path = g_canonicalize_filename(path, NULL);
    char *prefix = g_strconcat(canonical_root, G_DIR_SEPARATOR_S, NULL);
    gboolean safe = g_str_has_prefix(canonical_path, prefix) && !g_str_equal(canonical_path, canonical_root);
    g_free(prefix);
    g_free(canonical_path);
    g_free(canonical_root);
    return safe;
}

static gboolean remove_tree(const char *path, GError **error) {
    GStatBuf status;
    if (g_lstat(path, &status) != 0) return errno == ENOENT;
    if (!S_ISDIR(status.st_mode) || S_ISLNK(status.st_mode)) {
        if (g_unlink(path) == 0) return TRUE;
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not remove %s", path);
        return FALSE;
    }
    DIR *directory = opendir(path);
    if (!directory) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not open %s", path);
        return FALSE;
    }
    gboolean removed = TRUE;
    struct dirent *entry;
    while (removed && (entry = readdir(directory))) {
        if (g_str_equal(entry->d_name, ".") || g_str_equal(entry->d_name, "..")) continue;
        char *child = g_build_filename(path, entry->d_name, NULL);
        removed = remove_tree(child, error);
        g_free(child);
    }
    closedir(directory);
    if (removed && g_rmdir(path) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not remove %s", path);
        removed = FALSE;
    }
    return removed;
}

char *codexbar_managed_codex_create_home(CodexBarManagedCodexStore *store, GError **error) {
    g_return_val_if_fail(store != NULL, NULL);
    char *identifier = g_uuid_string_random();
    char *home = g_build_filename(store->root, identifier, NULL);
    g_free(identifier);
    if (!safe_home(store->root, home) || g_mkdir_with_parents(store->root, 0700) != 0 ||
        g_chmod(store->root, 0700) != 0 || g_mkdir_with_parents(home, 0700) != 0 || g_chmod(home, 0700) != 0) {
        g_set_error_literal(error, managed_error_quark(), 7, "Could not create a secure managed Codex home");
        g_free(home);
        return NULL;
    }
    return home;
}

gboolean codexbar_managed_codex_discard_home(CodexBarManagedCodexStore *store,
                                             const char *path,
                                             GError **error) {
    g_return_val_if_fail(store != NULL && path != NULL, FALSE);
    if (!safe_home(store->root, path)) {
        g_set_error(error, managed_error_quark(), 9, "Refusing to delete unmanaged Codex home: %s", path);
        return FALSE;
    }
    return remove_tree(path, error);
}

CodexBarManagedCodexAccount *codexbar_managed_codex_import(CodexBarManagedCodexStore *store,
                                                           const char *auth_file,
                                                           GError **error) {
    g_return_val_if_fail(store != NULL && auth_file != NULL, NULL);
    char *contents = NULL;
    gsize length = 0;
    if (!read_auth_file(auth_file, &contents, &length, error)) return NULL;
    char *email = NULL;
    char *provider_id = NULL;
    if (!auth_identity(contents, &email, &provider_id, error)) {
        g_free(contents);
        return NULL;
    }
    CodexBarManagedCodexAccount *existing = NULL;
    for (guint index = 0; index < store->accounts->len; index++) {
        CodexBarManagedCodexAccount *candidate = g_ptr_array_index(store->accounts, index);
        gboolean match = provider_id && candidate->provider_account_id
                             ? g_str_equal(candidate->email, email) &&
                                   g_str_equal(candidate->provider_account_id, provider_id)
                             : g_str_equal(candidate->email, email) && !candidate->provider_account_id;
        if (match) {
            existing = candidate;
            break;
        }
    }
    char *id = existing ? g_strdup(existing->id) : g_uuid_string_random();
    char *home = codexbar_managed_codex_create_home(store, error);
    if (!home) {
        g_free(id);
        g_free(provider_id);
        g_free(email);
        g_free(contents);
        return NULL;
    }
    char *destination = g_build_filename(home, "auth.json", NULL);
    gboolean copied = g_file_set_contents_full(destination,
                                                contents,
                                                (gssize)length,
                                                G_FILE_SET_CONTENTS_CONSISTENT | G_FILE_SET_CONTENTS_DURABLE,
                                                0600,
                                                error);
    if (copied && g_chmod(destination, 0600) != 0) {
        g_set_error(error, G_FILE_ERROR, g_file_error_from_errno(errno), "Could not secure managed Codex auth file");
        copied = FALSE;
    }
    g_free(destination);
    if (!copied) {
        remove_tree(home, NULL);
        g_free(home);
        g_free(id);
        g_free(provider_id);
        g_free(email);
        g_free(contents);
        return NULL;
    }
    double now = (double)g_get_real_time() / G_USEC_PER_SEC;
    char *old_home = existing ? g_strdup(existing->managed_home_path) : NULL;
    CodexBarManagedCodexAccount *account = existing ? existing : g_new0(CodexBarManagedCodexAccount, 1);
    if (!existing) {
        account->id = id;
        account->created_at = now;
        g_ptr_array_add(store->accounts, account);
    } else {
        g_free(id);
    }
    g_free(account->email);
    g_free(account->provider_account_id);
    g_free(account->auth_fingerprint);
    g_free(account->managed_home_path);
    account->email = email;
    account->provider_account_id = provider_id;
    account->auth_fingerprint = fingerprint(contents, length);
    account->managed_home_path = home;
    account->updated_at = now;
    account->last_authenticated_at = now;
    account->has_last_authenticated_at = TRUE;
    g_free(contents);
    if (!codexbar_managed_codex_store_save(store, error)) {
        remove_tree(home, NULL);
        g_free(old_home);
        return NULL;
    }
    if (old_home && !g_str_equal(old_home, home) && safe_home(store->root, old_home)) remove_tree(old_home, NULL);
    g_free(old_home);
    return account;
}

gboolean codexbar_managed_codex_remove(CodexBarManagedCodexStore *store,
                                       const char *selector,
                                       GError **error) {
    CodexBarManagedCodexAccount *account = codexbar_managed_codex_find(store, selector);
    if (!account) {
        g_set_error_literal(error, managed_error_quark(), 8, "Managed Codex account was not found or is ambiguous");
        return FALSE;
    }
    gboolean can_delete_home = safe_home(store->root, account->managed_home_path);
    guint index = 0;
    while (g_ptr_array_index(store->accounts, index) != account) index++;
    char *home = g_strdup(account->managed_home_path);
    g_ptr_array_remove_index(store->accounts, index);
    if (!codexbar_managed_codex_store_save(store, error)) {
        g_free(home);
        return FALSE;
    }
    gboolean removed = !can_delete_home || remove_tree(home, error);
    g_free(home);
    return removed;
}

void codexbar_managed_codex_store_free(CodexBarManagedCodexStore *store) {
    if (!store) return;
    g_free(store->path);
    g_free(store->root);
    g_ptr_array_unref(store->accounts);
    if (store->lock_fd >= 0) {
        flock(store->lock_fd, LOCK_UN);
        close(store->lock_fd);
    }
    g_free(store);
}
