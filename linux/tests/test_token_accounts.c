#include "token_accounts.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

static CodexBarProviderConfig make_config(const char *provider) {
    CodexBarProviderConfig config = {0};
    config.id = g_strdup(provider);
    config.raw = json_object_new_object();
    return config;
}

static void clear_config(CodexBarProviderConfig *config) {
    g_free(config->id);
    g_free(config->api_key);
    json_object_put(config->raw);
    *config = (CodexBarProviderConfig){0};
}

static void test_catalog(void) {
    g_assert_true(codexbar_token_accounts_supported("openrouter"));
    g_assert_true(codexbar_token_accounts_supported("stepfun"));
    g_assert_false(codexbar_token_accounts_supported("codex"));
    g_assert_true(codexbar_token_accounts_use_cookie("claude"));
    g_assert_false(codexbar_token_accounts_use_cookie("openai"));
    CodexBarProviderConfig config = make_config("openrouter");
    g_assert_false(codexbar_token_account_is_selected(&config));
    json_object_object_add(config.raw, "tokenAccountSelected", json_object_new_boolean(TRUE));
    g_assert_true(codexbar_token_account_is_selected(&config));
    clear_config(&config);
}

static void test_crud_and_schema(void) {
    GError *error = NULL;
    CodexBarProviderConfig config = make_config("zai");
    char *first_id = NULL;
    g_assert_true(codexbar_token_accounts_add(&config, "Personal", "token-one", "external", "personal",
                                              NULL, NULL, &first_id, &error));
    g_assert_no_error(error);
    char *second_id = NULL;
    g_assert_true(codexbar_token_accounts_add(&config, "Team", "token-two", NULL, "team",
                                              "organization", "workspace", &second_id, &error));
    g_assert_no_error(error);
    int active = -1;
    json_object *accounts = codexbar_token_accounts_array(&config, &active);
    g_assert_cmpuint(json_object_array_length(accounts), ==, 2);
    g_assert_cmpint(active, ==, 1);
    json_object *team = json_object_array_get_idx(accounts, 1);
    char *organization = codexbar_token_account_string(team, "organizationId");
    g_assert_cmpstr(organization, ==, "organization");
    g_free(organization);
    json_object *legacy = NULL;
    g_assert_false(json_object_object_get_ex(team, "organizationID", &legacy));

    g_assert_true(codexbar_token_accounts_select(&config, first_id, &error));
    g_assert_no_error(error);
    codexbar_token_accounts_array(&config, &active);
    g_assert_cmpint(active, ==, 0);
    g_assert_true(codexbar_token_accounts_update(&config, "1", "Primary", "new-token", NULL, NULL, NULL, NULL,
                                                 &error));
    g_assert_no_error(error);
    char *label = codexbar_token_account_string(json_object_array_get_idx(accounts, 0), "label");
    g_assert_cmpstr(label, ==, "Primary");
    g_free(label);

    g_assert_true(codexbar_token_accounts_move(&config, first_id, 1, &error));
    g_assert_no_error(error);
    codexbar_token_accounts_array(&config, &active);
    g_assert_cmpint(active, ==, 1);
    g_assert_true(codexbar_token_accounts_remove(&config, second_id, &error));
    g_assert_no_error(error);
    accounts = codexbar_token_accounts_array(&config, &active);
    g_assert_cmpuint(json_object_array_length(accounts), ==, 1);
    g_assert_cmpint(active, ==, 0);
    g_assert_true(codexbar_token_accounts_remove(&config, first_id, &error));
    g_assert_no_error(error);
    g_assert_null(codexbar_token_accounts_array(&config, &active));
    g_free(first_id);
    g_free(second_id);
    clear_config(&config);
}

static void test_validation(void) {
    GError *error = NULL;
    CodexBarProviderConfig config = make_config("codex");
    g_assert_false(codexbar_token_accounts_add(&config, "one", "token", NULL, NULL, NULL, NULL, NULL, &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    clear_config(&config);

    config = make_config("openrouter");
    g_assert_false(codexbar_token_accounts_add(&config, "one", "line1\nline2", NULL, NULL, NULL, NULL, NULL,
                                               &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    g_assert_false(codexbar_token_accounts_select(&config, "missing", &error));
    g_assert_nonnull(error);
    g_clear_error(&error);
    clear_config(&config);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/token-accounts/catalog", test_catalog);
    g_test_add_func("/token-accounts/crud-schema", test_crud_and_schema);
    g_test_add_func("/token-accounts/validation", test_validation);
    return g_test_run();
}
