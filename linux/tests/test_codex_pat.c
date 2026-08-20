#include "codex.h"
#include "http.h"
#include "model.h"

#include <glib.h>
#include <glib/gstdio.h>
#include <json-c/json.h>
#include <string.h>

#define PAT_WHOAMI_URL "https://auth.openai.com/api/accounts/v1/user-auth-credential/whoami"
#define USAGE_URL "https://chatgpt.com/backend-api/wham/usage"

typedef struct {
    const char *url;
    const char *authorization;
    const char *account_id;
    long status;
    const char *body;
    gboolean pat_headers;
} ExpectedRequest;

typedef struct {
    char *root;
    char *previous_home;
    char *previous_codex_home;
    char *previous_usage_url;
    char *previous_cli_version;
    CodexBarProviderConfig config;
} PatFixture;

static ExpectedRequest expected_requests[8];
static guint expected_request_count;
static guint expected_request_index;
static guint cli_calls;
static char expected_authorization[256];

static const char usage_json[] =
    "{\"plan_type\":\"plus\",\"rate_limit\":{\"primary_window\":{"
    "\"used_percent\":25,\"reset_at\":1776216359,\"limit_window_seconds\":18000}}}";

static const char *request_header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) {
            return request->headers[index].value;
        }
    }
    return NULL;
}

static void expect_requests(const ExpectedRequest *requests, guint count) {
    g_assert_cmpuint(count, <=, G_N_ELEMENTS(expected_requests));
    memset(expected_requests, 0, sizeof(expected_requests));
    if (count > 0) memcpy(expected_requests, requests, count * sizeof(*requests));
    expected_request_count = count;
    expected_request_index = 0;
}

static void assert_requests_consumed(void) {
    g_assert_cmpuint(expected_request_index, ==, expected_request_count);
}

static CodexBarHttpResponse *pat_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    g_assert_cmpuint(expected_request_index, <, expected_request_count);
    const ExpectedRequest *expected = &expected_requests[expected_request_index++];
    g_assert_cmpstr(request->url, ==, expected->url);
    g_assert_cmpstr(request->method, ==, "GET");
    g_assert_cmpint(request->timeout_seconds, ==, 30);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpstr(request_header(request, "Authorization"), ==, expected->authorization);
    if (expected->account_id) {
        g_assert_cmpstr(request_header(request, "ChatGPT-Account-Id"), ==, expected->account_id);
    } else {
        g_assert_null(request_header(request, "ChatGPT-Account-Id"));
    }
    g_assert_cmpstr(request_header(request, "Accept"), ==, "application/json");
    const char *user_agent = request_header(request, "User-Agent");
    g_assert_nonnull(user_agent);
    if (expected->pat_headers) {
        g_assert_cmpstr(request_header(request, "originator"), ==, "codex_cli_rs");
        g_assert_null(request_header(request, "OpenAI-Beta"));
        g_assert_true(g_str_has_prefix(user_agent, "codex_cli_rs/1.2.3 (Linux "));
        g_assert_nonnull(strstr(user_agent, "; "));
        g_assert_true(g_str_has_suffix(user_agent, ")"));
    } else {
        g_assert_null(request_header(request, "originator"));
        g_assert_cmpstr(request_header(request, "OpenAI-Beta"), ==, "codex-1");
        g_assert_cmpstr(user_agent, ==, "CodexBar");
    }

    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = expected->status;
    response->body = g_strdup(expected->body ? expected->body : "");
    response->body_length = strlen(response->body);
    return response;
}

static CodexBarProvider *fake_cli(GError **error) {
    (void)error;
    cli_calls++;
    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("codex");
    provider->source = g_strdup("cli");
    return provider;
}

static void restore_environment(const char *name, const char *value) {
    if (value) {
        g_setenv(name, value, TRUE);
    } else {
        g_unsetenv(name);
    }
}

static void remove_tree(const char *path) {
    if (!path || !g_file_test(path, G_FILE_TEST_EXISTS)) return;
    if (g_file_test(path, G_FILE_TEST_IS_DIR) && !g_file_test(path, G_FILE_TEST_IS_SYMLINK)) {
        GDir *directory = g_dir_open(path, 0, NULL);
        if (directory) {
            const char *name = NULL;
            while ((name = g_dir_read_name(directory))) {
                char *child = g_build_filename(path, name, NULL);
                remove_tree(child);
                g_free(child);
            }
            g_dir_close(directory);
        }
        g_rmdir(path);
        return;
    }
    g_remove(path);
}

static void fixture_setup(PatFixture *fixture, gconstpointer user_data) {
    (void)user_data;
    memset(&fixture->config, 0, sizeof(fixture->config));
    fixture->previous_home = g_strdup(g_getenv("HOME"));
    fixture->previous_codex_home = g_strdup(g_getenv("CODEX_HOME"));
    fixture->previous_usage_url = g_strdup(g_getenv("CODEXBAR_CODEX_USAGE_URL"));
    fixture->previous_cli_version = g_strdup(g_getenv("CODEXBAR_CODEX_CLI_VERSION"));
    GError *error = NULL;
    fixture->root = g_dir_make_tmp("codexbar-pat-XXXXXX", &error);
    g_assert_no_error(error);
    g_assert_nonnull(fixture->root);
    g_setenv("HOME", fixture->root, TRUE);
    g_unsetenv("CODEX_HOME");
    g_unsetenv("CODEXBAR_CODEX_USAGE_URL");
    g_setenv("CODEXBAR_CODEX_CLI_VERSION", "codex-cli 1.2.3", TRUE);
    fixture->config.id = "codex";
    fixture->config.raw = json_object_new_object();
    expected_request_count = 0;
    expected_request_index = 0;
    cli_calls = 0;
}

static void fixture_teardown(PatFixture *fixture, gconstpointer user_data) {
    (void)user_data;
    if (fixture->config.raw) json_object_put(fixture->config.raw);
    g_free(fixture->config.codex_active_account_id);
    g_free(fixture->config.codex_active_home_path);
    g_clear_pointer(&fixture->config.codex_profile_home_paths, g_ptr_array_unref);
    remove_tree(fixture->root);
    restore_environment("HOME", fixture->previous_home);
    restore_environment("CODEX_HOME", fixture->previous_codex_home);
    restore_environment("CODEXBAR_CODEX_USAGE_URL", fixture->previous_usage_url);
    restore_environment("CODEXBAR_CODEX_CLI_VERSION", fixture->previous_cli_version);
    g_free(fixture->previous_cli_version);
    g_free(fixture->previous_usage_url);
    g_free(fixture->previous_codex_home);
    g_free(fixture->previous_home);
    g_free(fixture->root);
}

static char *ambient_home(const PatFixture *fixture) {
    return g_build_filename(fixture->root, ".codex", NULL);
}

static void write_auth(const char *home, const char *json) {
    g_assert_cmpint(g_mkdir_with_parents(home, 0700), ==, 0);
    char *path = g_build_filename(home, "auth.json", NULL);
    GError *error = NULL;
    g_assert_true(g_file_set_contents(path, json, -1, &error));
    g_assert_no_error(error);
    g_free(path);
}

static void assert_pat_extensions(const CodexBarProvider *provider) {
    g_assert_nonnull(provider->usage_extensions);
    json_object *value = NULL;
    g_assert_true(json_object_object_get_ex(provider->usage_extensions, "dataConfidence", &value));
    g_assert_cmpstr(json_object_get_string(value), ==, "exact");
    g_assert_true(json_object_object_get_ex(provider->usage_extensions, "codexCredentialKind", &value));
    g_assert_cmpstr(json_object_get_string(value), ==, "pat");
}

static void test_explicit_pat(PatFixture *fixture, gconstpointer user_data) {
    (void)user_data;
    char *home = ambient_home(fixture);
    write_auth(home, "{\"personal_access_token\":\"  pat-token  \"}");
    g_free(home);
    json_object_object_add(fixture->config.raw, "accountID", json_object_new_string("stale-account"));
    json_object_object_add(fixture->config.raw, "cliVersion", json_object_new_string("codex-cli 1.2.3"));

    const ExpectedRequest requests[] = {
        {PAT_WHOAMI_URL,
         "Bearer pat-token",
         NULL,
         200,
         "{\"chatgpt_account_id\":\"acct-pat\",\"chatgpt_plan_type\":\"team\","
         "\"email\":\"pat@example.com\"}",
         TRUE},
        {USAGE_URL, "Bearer pat-token", "acct-pat", 200, usage_json, TRUE},
    };
    expect_requests(requests, G_N_ELEMENTS(requests));
    g_assert_true(codexbar_codex_pat_is_available(&fixture->config));

    GError *error = NULL;
    CodexBarProvider *provider = codexbar_codex_fetch_with_adapters(
        &fixture->config, "api", pat_transport, fake_cli, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->source, ==, "api");
    g_assert_cmpstr(provider->account, ==, "pat@example.com");
    g_assert_cmpstr(provider->plan, ==, "Plus");
    g_assert_nonnull(provider->identity);
    g_assert_cmpstr(provider->identity->account_id, ==, "acct-pat");
    g_assert_cmpstr(provider->identity->login_method, ==, "Plus");
    assert_pat_extensions(provider);
    g_assert_cmpuint(cli_calls, ==, 0);
    assert_requests_consumed();
    codexbar_provider_free(provider);
}

static void configure_profile(PatFixture *fixture, const char *profile_home) {
    fixture->config.has_codex_active_source = TRUE;
    fixture->config.codex_active_source = CODEXBAR_CODEX_SOURCE_PROFILE_HOME;
    fixture->config.codex_active_home_path = g_strdup(profile_home);
    fixture->config.has_codex_profile_home_paths = TRUE;
    fixture->config.codex_profile_home_paths = g_ptr_array_new_with_free_func(g_free);
    g_ptr_array_add(fixture->config.codex_profile_home_paths, g_strdup(profile_home));
}

static void expect_successful_pat(const char *token) {
    g_snprintf(expected_authorization, sizeof(expected_authorization), "Bearer %s", token);
    ExpectedRequest requests[] = {
        {PAT_WHOAMI_URL,
         expected_authorization,
         NULL,
         200,
         "{\"chatgpt_account_id\":\"acct-scope\",\"email\":\"scope@example.com\"}",
         TRUE},
        {USAGE_URL, expected_authorization, "acct-scope", 200, usage_json, TRUE},
    };
    expect_requests(requests, G_N_ELEMENTS(requests));
}

static void fetch_and_assert_api(PatFixture *fixture) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_codex_fetch_with_adapters(
        &fixture->config, "api", pat_transport, fake_cli, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->source, ==, "api");
    codexbar_provider_free(provider);
    assert_requests_consumed();
}

static void test_scoped_home_resolution(PatFixture *fixture, gconstpointer user_data) {
    (void)user_data;
    char *ambient = ambient_home(fixture);
    char *profile = g_build_filename(fixture->root, "profile", NULL);
    write_auth(ambient, "{\"personal_access_token\":\"ambient-token\"}");
    write_auth(profile, "{\"personalAccessToken\":\"profile-token\"}");
    configure_profile(fixture, profile);

    expect_successful_pat("profile-token");
    g_assert_true(codexbar_codex_pat_is_available(&fixture->config));
    fetch_and_assert_api(fixture);

    write_auth(profile, "{not-json");
    expect_successful_pat("ambient-token");
    g_assert_true(codexbar_codex_pat_is_available(&fixture->config));
    fetch_and_assert_api(fixture);

    fixture->config.codex_active_source = CODEXBAR_CODEX_SOURCE_MANAGED_ACCOUNT;
    expect_successful_pat("ambient-token");
    g_assert_true(codexbar_codex_pat_is_available(&fixture->config));
    fetch_and_assert_api(fixture);

    g_free(profile);
    g_free(ambient);
}

static void test_auto_fallbacks(PatFixture *fixture, gconstpointer user_data) {
    (void)user_data;
    char *home = ambient_home(fixture);
    write_auth(home, "{\"personal_access_token\":\"pat-token\"}");
    g_free(home);
    json_object_object_add(fixture->config.raw, "oauthToken", json_object_new_string("oauth-token"));

    const ExpectedRequest oauth_requests[] = {
        {PAT_WHOAMI_URL, "Bearer pat-token", NULL, 401, "{}", TRUE},
        {USAGE_URL, "Bearer oauth-token", NULL, 200, usage_json, FALSE},
    };
    expect_requests(oauth_requests, G_N_ELEMENTS(oauth_requests));
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_codex_fetch_with_adapters(
        &fixture->config, "auto", pat_transport, fake_cli, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->source, ==, "oauth");
    g_assert_cmpuint(cli_calls, ==, 0);
    codexbar_provider_free(provider);
    assert_requests_consumed();

    const ExpectedRequest cli_requests[] = {
        {PAT_WHOAMI_URL, "Bearer pat-token", NULL, 403, "{}", TRUE},
        {USAGE_URL, "Bearer oauth-token", NULL, 401, "{}", FALSE},
    };
    expect_requests(cli_requests, G_N_ELEMENTS(cli_requests));
    provider = codexbar_codex_fetch_with_adapters(
        &fixture->config, "auto", pat_transport, fake_cli, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->source, ==, "cli");
    g_assert_cmpuint(cli_calls, ==, 1);
    codexbar_provider_free(provider);
    assert_requests_consumed();
}

static void test_missing_pat_falls_back(PatFixture *fixture, gconstpointer user_data) {
    (void)user_data;
    json_object_object_add(fixture->config.raw, "oauthToken", json_object_new_string("oauth-token"));
    const ExpectedRequest requests[] = {
        {USAGE_URL, "Bearer oauth-token", NULL, 200, usage_json, FALSE},
    };
    expect_requests(requests, G_N_ELEMENTS(requests));
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_codex_fetch_with_adapters(
        &fixture->config, "auto", pat_transport, fake_cli, NULL, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->source, ==, "oauth");
    codexbar_provider_free(provider);
    assert_requests_consumed();

    expect_requests(NULL, 0);
    provider = codexbar_codex_fetch_with_adapters(
        &fixture->config, "api", pat_transport, fake_cli, NULL, &error);
    g_assert_null(provider);
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "not found"));
    g_clear_error(&error);
    g_assert_cmpuint(cli_calls, ==, 0);
    assert_requests_consumed();
}

static void test_malformed_auth_is_terminal(PatFixture *fixture, gconstpointer user_data) {
    (void)user_data;
    char *home = ambient_home(fixture);
    write_auth(home, "{not-json");
    g_free(home);
    json_object_object_add(fixture->config.raw, "oauthToken", json_object_new_string("oauth-token"));
    expect_requests(NULL, 0);

    GError *error = NULL;
    CodexBarProvider *provider = codexbar_codex_fetch_with_adapters(
        &fixture->config, "auto", pat_transport, fake_cli, NULL, &error);
    g_assert_null(provider);
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "invalid JSON"));
    g_clear_error(&error);
    g_assert_false(codexbar_codex_pat_is_available(&fixture->config));
    g_assert_cmpuint(cli_calls, ==, 0);
    assert_requests_consumed();
}

static void test_pat_endpoint_errors_are_terminal(PatFixture *fixture, gconstpointer user_data) {
    (void)user_data;
    char *home = ambient_home(fixture);
    write_auth(home, "{\"personal_access_token\":\"pat-token\"}");
    g_free(home);
    json_object_object_add(fixture->config.raw, "oauthToken", json_object_new_string("oauth-token"));

    const ExpectedRequest server_requests[] = {
        {PAT_WHOAMI_URL, "Bearer pat-token", NULL, 500, "server failure", TRUE},
    };
    expect_requests(server_requests, G_N_ELEMENTS(server_requests));
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_codex_fetch_with_adapters(
        &fixture->config, "auto", pat_transport, fake_cli, NULL, &error);
    g_assert_null(provider);
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "HTTP 500"));
    g_clear_error(&error);
    g_assert_cmpuint(cli_calls, ==, 0);
    assert_requests_consumed();

    const ExpectedRequest malformed_requests[] = {
        {PAT_WHOAMI_URL, "Bearer pat-token", NULL, 200, "{\"chatgpt_account_id\":17}", TRUE},
    };
    expect_requests(malformed_requests, G_N_ELEMENTS(malformed_requests));
    provider = codexbar_codex_fetch_with_adapters(
        &fixture->config, "auto", pat_transport, fake_cli, NULL, &error);
    g_assert_null(provider);
    g_assert_nonnull(error);
    g_assert_nonnull(strstr(error->message, "whoami response is malformed"));
    g_clear_error(&error);
    g_assert_cmpuint(cli_calls, ==, 0);
    assert_requests_consumed();
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add("/codex/pat/explicit", PatFixture, NULL, fixture_setup, test_explicit_pat, fixture_teardown);
    g_test_add("/codex/pat/scoped-home", PatFixture, NULL, fixture_setup, test_scoped_home_resolution, fixture_teardown);
    g_test_add("/codex/pat/auto-fallbacks", PatFixture, NULL, fixture_setup, test_auto_fallbacks, fixture_teardown);
    g_test_add("/codex/pat/missing-fallback", PatFixture, NULL, fixture_setup, test_missing_pat_falls_back, fixture_teardown);
    g_test_add("/codex/pat/malformed-auth-terminal", PatFixture, NULL, fixture_setup, test_malformed_auth_is_terminal, fixture_teardown);
    g_test_add("/codex/pat/endpoint-errors-terminal", PatFixture, NULL, fixture_setup, test_pat_endpoint_errors_are_terminal, fixture_teardown);
    return g_test_run();
}
