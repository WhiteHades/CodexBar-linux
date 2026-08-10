#include "notion.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <string.h>

typedef struct {
    const char *spaces_body;
    const char *status_body;
    long spaces_status;
    long status_status;
    const char *cookie;
    const char *space_id;
    gboolean captured_headers;
    gboolean cancel_after_spaces;
    GCancellable *cancellable;
    guint count;
} TransportFixture;

static TransportFixture fixture;
static char *spaces_fixture;
static char *status_fixture;

static const char *request_header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static CodexBarHttpResponse *make_response(long status, const char *body) {
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = status;
    response->body = g_strdup(body ? body : "{}");
    response->body_length = strlen(response->body);
    response->headers = g_ptr_array_new();
    return response;
}

static CodexBarHttpResponse *stub_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    guint index = fixture.count++;
    g_assert_cmpuint(index, <, 2);
    g_assert_cmpstr(request->method, ==, "POST");
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_DENY);
    g_assert_true(request->cancellable == fixture.cancellable);
    g_assert_cmpstr(request_header(request, "Content-Type"), ==, "application/json");
    g_assert_cmpstr(request_header(request, "Origin"), ==, "https://app.notion.com");
    g_assert_cmpstr(request_header(request, "Cookie"), ==, fixture.cookie);
    g_assert_null(request_header(request, "Authorization"));
    g_assert_null(request_header(request, "x-notion-space-id"));

    if (fixture.captured_headers) {
        g_assert_cmpstr(request_header(request, "Accept"), ==, "application/vnd.notion+json");
        g_assert_cmpstr(request_header(request, "Accept-Language"), ==, "fr-FR");
        g_assert_cmpstr(request_header(request, "notion-audit-log-platform"), ==, "web");
        g_assert_cmpstr(request_header(request, "notion-client-version"), ==, "23.13.0");
        g_assert_cmpstr(request_header(request, "Referer"), ==, "https://app.notion.com/settings");
        g_assert_cmpstr(request_header(request, "Sec-Fetch-Dest"), ==, "empty");
        g_assert_cmpstr(request_header(request, "Sec-Fetch-Mode"), ==, "cors");
        g_assert_cmpstr(request_header(request, "Sec-Fetch-Site"), ==, "same-origin");
        g_assert_cmpstr(request_header(request, "User-Agent"), ==, "Captured Browser");
        g_assert_cmpstr(request_header(request, "x-notion-active-user-header"), ==, "active-user");
    } else {
        g_assert_cmpstr(request_header(request, "Accept"), ==, "*/*");
        g_assert_cmpstr(request_header(request, "Accept-Language"), ==, "en-US,en;q=0.9");
        g_assert_cmpstr(request_header(request, "Referer"), ==, "https://app.notion.com/");
        g_assert_cmpstr(request_header(request, "Sec-Fetch-Dest"), ==, "empty");
        g_assert_cmpstr(request_header(request, "Sec-Fetch-Mode"), ==, "cors");
        g_assert_cmpstr(request_header(request, "Sec-Fetch-Site"), ==, "same-origin");
        g_assert_nonnull(request_header(request, "User-Agent"));
    }

    if (index == 0) {
        g_assert_cmpstr(request->url, ==, "https://app.notion.com/api/v3/getSpaces");
        g_assert_cmpuint(request->body_length, ==, 2);
        g_assert_cmpmem(request->body, request->body_length, "{}", 2);
        if (fixture.cancel_after_spaces && fixture.cancellable) g_cancellable_cancel(fixture.cancellable);
        return make_response(fixture.spaces_status, fixture.spaces_body);
    }
    g_assert_cmpstr(request->url, ==, "https://app.notion.com/api/v3/getCreditRateLimitStatus");
    char *expected = g_strdup_printf("{\"spaceId\":\"%s\"}", fixture.space_id);
    g_assert_cmpuint(request->body_length, ==, strlen(expected));
    g_assert_cmpmem(request->body, request->body_length, expected, strlen(expected));
    g_free(expected);
    return make_response(fixture.status_status, fixture.status_body);
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static CodexBarProviderConfig manual_config(const char *cookie, const char *workspace_id) {
    CodexBarProviderConfig config = {.id = "notion"};
    config.raw = json_object_new_object();
    json_object_object_add(config.raw, "cookieSource", json_object_new_string("manual"));
    if (cookie) json_object_object_add(config.raw, "cookieHeader", json_object_new_string(cookie));
    config.workspace_id = workspace_id ? g_strdup(workspace_id) : NULL;
    return config;
}

static void clear_config(CodexBarProviderConfig *config) {
    if (config->raw) json_object_put(config->raw);
    g_free(config->workspace_id);
    *config = (CodexBarProviderConfig){0};
}

static void reset_fixture(const char *cookie, const char *space_id) {
    fixture = (TransportFixture){
        .spaces_body = spaces_fixture,
        .status_body = status_fixture,
        .spaces_status = 200,
        .status_status = 200,
        .cookie = cookie,
        .space_id = space_id,
    };
}

static CodexBarProvider *fetch(CodexBarProviderConfig *config, gint64 now_ms, GError **error) {
    return codexbar_notion_fetch_with_transport_and_cancellable(
        config, stub_transport, fixture.cancellable, now_ms, error);
}

static void test_mapping_and_exact_requests(void) {
    const gint64 now_ms = 1785600000000LL;
    CodexBarProviderConfig config = manual_config("bare-token", NULL);
    reset_fixture("token_v2=bare-token", "11111111-2222-3333-4444-555555555555");
    GError *error = NULL;
    CodexBarProvider *provider = fetch(&config, now_ms, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 2);
    g_assert_cmpstr(provider->provider, ==, "notion");
    g_assert_cmpstr(provider->source, ==, "web");
    g_assert_cmpstr(provider->account, ==, "person@example.com");
    g_assert_cmpstr(provider->plan, ==, "Business");
    g_assert_true(provider->explicit_quota_slots);
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    const CodexBarQuotaWindow *rolling = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpstr(rolling->id, ==, "primary");
    g_assert_cmpstr(rolling->title, ==, "Rolling");
    g_assert_cmpfloat(rolling->used_percent, ==, 42.5);
    g_assert_cmpint(rolling->window_minutes, ==, 360);
    g_assert_cmpint(rolling->resets_at_ms, ==, now_ms + 12600 * 1000LL);
    const CodexBarQuotaWindow *monthly = codexbar_provider_quota_window(provider, 1);
    g_assert_cmpstr(monthly->id, ==, "secondary");
    g_assert_cmpstr(monthly->title, ==, "Monthly");
    g_assert_cmpfloat(monthly->used_percent, ==, 18.0);
    g_assert_cmpint(monthly->resets_at_ms, ==, 1788000000000LL);
    g_assert_nonnull(provider->identity);
    g_assert_cmpstr(provider->identity->organization, ==, "Acme");
    g_assert_cmpstr(provider->identity->account_id, ==, "aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee");
    g_assert_cmpstr(provider->identity->login_method, ==, "Business");
    codexbar_provider_free(provider);
    clear_config(&config);
}

static void test_workspace_selection(void) {
    const char *configured[] = {
        "66666666-7777-8888-9999-aaaaaaaaaaaa",
        "66666666777788889999aaaaaaaaaaaa",
        "00000000-0000-0000-0000-000000000000",
    };
    const char *expected[] = {
        "66666666-7777-8888-9999-aaaaaaaaaaaa",
        "66666666-7777-8888-9999-aaaaaaaaaaaa",
        "11111111-2222-3333-4444-555555555555",
    };
    for (guint index = 0; index < G_N_ELEMENTS(configured); index++) {
        CodexBarProviderConfig config = manual_config("token_v2=abc", configured[index]);
        reset_fixture("token_v2=abc", expected[index]);
        GError *error = NULL;
        CodexBarProvider *provider = fetch(&config, 1, &error);
        g_assert_no_error(error);
        g_assert_nonnull(provider);
        codexbar_provider_free(provider);
        clear_config(&config);
    }
}

static void test_curl_capture_allowlist(void) {
    const char *capture =
        "curl $'https://app.notion.com/api/v3/getCreditRateLimitStatus' \\\n"
        "  -H $'Accept: application/vnd.notion+json' -H $'Accept-Language: fr-FR' \\\n"
        "  -H $'notion-audit-log-platform: web' -H $'notion-client-version: 23.13.0' \\\n"
        "  -H $'Referer: https://app.notion.com/settings' -H $'Sec-Fetch-Dest: empty' \\\n"
        "  -H $'Sec-Fetch-Mode: cors' -H $'Sec-Fetch-Site: same-origin' \\\n"
        "  -H $'User-Agent: Captured Browser' -H $'x-notion-active-user-header: active-user' \\\n"
        "  -H $'x-notion-space-id: attacker-space' -H $'Authorization: secret' \\\n"
        "  -H $'Origin: https://attacker.example' -H $'Content-Type: text/plain' \\\n"
        "-H $'Cookie: token_v2=abc; notion_user_id=def' --data-raw $'{\"spaceId\":\"old\"}'";
    CodexBarProviderConfig config = manual_config(capture, NULL);
    reset_fixture("token_v2=abc; notion_user_id=def", "11111111-2222-3333-4444-555555555555");
    fixture.captured_headers = TRUE;
    GError *error = NULL;
    CodexBarProvider *provider = fetch(&config, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    codexbar_provider_free(provider);
    clear_config(&config);
}

static gint64 utc_ms(gint year, gint month, gint day) {
    GDateTime *date = g_date_time_new_utc(year, month, day, 0, 0, 0);
    g_assert_nonnull(date);
    gint64 milliseconds = g_date_time_to_unix(date) * 1000;
    g_date_time_unref(date);
    return milliseconds;
}

static void test_window_boundaries_and_month_lengths(void) {
    CodexBarProviderConfig config = manual_config("Cookie: token_v2=abc", NULL);
    reset_fixture("token_v2=abc", "11111111-2222-3333-4444-555555555555");
    fixture.status_body =
        "{\"status\":\"within_limit\",\"window\":{\"window\":\"6h\",\"used\":120,\"limit\":100},"
        "\"resetsInSeconds\":0,\"billingPeriodWindow\":{\"used\":42,\"limit\":0}}";
    const gint64 now_ms = utc_ms(2026, 2, 20);
    GError *error = NULL;
    CodexBarProvider *provider = fetch(&config, now_ms, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    const CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpfloat(window->used_percent, ==, 120.0);
    g_assert_true(window->has_resets_at);
    g_assert_cmpint(window->resets_at_ms, ==, now_ms);
    codexbar_provider_free(provider);

    const gint64 period_ends[] = {utc_ms(2026, 3, 1), utc_ms(2026, 6, 1)};
    const gint64 expected_minutes[] = {28 * 24 * 60, 31 * 24 * 60};
    for (guint index = 0; index < G_N_ELEMENTS(period_ends); index++) {
        char *body = g_strdup_printf(
            "{\"status\":\"within_limit\",\"window\":{\"used\":1},"
            "\"resetsInSeconds\":-1,\"billingPeriodWindow\":{\"used\":25,\"limit\":50,"
            "\"periodEndMs\":%" G_GINT64_FORMAT "}}",
            period_ends[index]);
        reset_fixture("token_v2=abc", "11111111-2222-3333-4444-555555555555");
        fixture.status_body = body;
        provider = fetch(&config, now_ms, &error);
        g_assert_no_error(error);
        g_assert_cmpuint(provider->quota_windows->len, ==, 1);
        window = codexbar_provider_quota_window(provider, 0);
        g_assert_cmpstr(window->id, ==, "secondary");
        g_assert_cmpfloat(window->used_percent, ==, 50.0);
        g_assert_true(window->has_window_minutes);
        g_assert_cmpint(window->window_minutes, ==, expected_minutes[index]);
        codexbar_provider_free(provider);
        g_free(body);
    }
    clear_config(&config);
}

static void test_credential_and_api_errors(void) {
    g_setenv("NOTION_TOKEN", "must-not-be-read", TRUE);
    CodexBarProviderConfig config = manual_config(NULL, NULL);
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_notion_fetch_with_transport_and_cancellable(
        &config, unexpected_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    clear_config(&config);
    g_unsetenv("NOTION_TOKEN");

    config = manual_config("Cookie: other=value", NULL);
    provider = codexbar_notion_fetch_with_transport_and_cancellable(
        &config, unexpected_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    clear_config(&config);

    config = manual_config("abc", NULL);
    json_object_object_add(config.raw, "cookieSource", json_object_new_string("auto"));
    provider = codexbar_notion_fetch_with_transport_and_cancellable(
        &config, unexpected_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
    g_clear_error(&error);
    clear_config(&config);

    config = manual_config("abc", NULL);
    reset_fixture("token_v2=abc", "11111111-2222-3333-4444-555555555555");
    fixture.spaces_status = 401;
    provider = fetch(&config, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);

    reset_fixture("token_v2=abc", "11111111-2222-3333-4444-555555555555");
    fixture.spaces_status = 403;
    provider = fetch(&config, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr(error->message, "HTTP 403"));
    g_clear_error(&error);

    reset_fixture("token_v2=abc", "11111111-2222-3333-4444-555555555555");
    fixture.status_status = 500;
    provider = fetch(&config, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_assert_nonnull(strstr(error->message, "getCreditRateLimitStatus"));
    g_clear_error(&error);

    reset_fixture("token_v2=abc", "66666666-7777-8888-9999-aaaaaaaaaaaa");
    fixture.status_body = "{\"status\":\"not_applicable\"}";
    g_free(config.workspace_id);
    config.workspace_id = g_strdup("66666666-7777-8888-9999-aaaaaaaaaaaa");
    provider = fetch(&config, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED);
    g_assert_nonnull(strstr(error->message, "Personal"));
    g_assert_nonnull(strstr(error->message, "Business and Enterprise"));
    g_clear_error(&error);
    clear_config(&config);
}

static void test_parse_errors(void) {
    const char *second = "bbbbbbbb-cccc-dddd-eeee-ffffffffffff";
    char *ambiguous = g_strdup_printf(
        "{\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\":{\"notion_user\":{"
        "\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\":{\"value\":{\"id\":"
        "\"aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee\"}}}},\"%s\":{\"notion_user\":{\"%s\":{"
        "\"value\":{\"id\":\"%s\"}}}}}",
        second,
        second,
        second);
    CodexBarProviderConfig config = manual_config("abc", NULL);
    reset_fixture("token_v2=abc", "unused");
    fixture.spaces_body = ambiguous;
    GError *error = NULL;
    CodexBarProvider *provider = fetch(&config, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_assert_cmpuint(fixture.count, ==, 1);
    g_clear_error(&error);
    g_free(ambiguous);

    reset_fixture("token_v2=abc", "unused");
    fixture.spaces_body = "{\"user\":{\"notion_user\":{},\"space\":{}}}";
    provider = fetch(&config, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

    reset_fixture("token_v2=abc", "11111111-2222-3333-4444-555555555555");
    fixture.status_body = "{\"errorId\":\"abc\",\"name\":\"UnauthorizedError\"}";
    provider = fetch(&config, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
    clear_config(&config);
}

static void test_cancellation(void) {
    CodexBarProviderConfig config = manual_config("abc", NULL);
    GCancellable *cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);
    fixture = (TransportFixture){.cancellable = cancellable};
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_notion_fetch_with_transport_and_cancellable(
        &config, unexpected_transport, cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(cancellable);

    cancellable = g_cancellable_new();
    reset_fixture("token_v2=abc", "11111111-2222-3333-4444-555555555555");
    fixture.cancellable = cancellable;
    fixture.cancel_after_spaces = TRUE;
    provider = fetch(&config, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_assert_cmpuint(fixture.count, ==, 1);
    g_clear_error(&error);
    g_object_unref(cancellable);
    clear_config(&config);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_assert_cmpint(argc, ==, 2);
    char *spaces_path = g_build_filename(argv[1], "get-spaces.json", NULL);
    char *status_path = g_build_filename(argv[1], "get-credit-rate-limit-status.json", NULL);
    g_assert_true(g_file_get_contents(spaces_path, &spaces_fixture, NULL, NULL));
    g_assert_true(g_file_get_contents(status_path, &status_fixture, NULL, NULL));
    g_free(spaces_path);
    g_free(status_path);

    g_test_add_func("/notion/mapping-exact-requests", test_mapping_and_exact_requests);
    g_test_add_func("/notion/workspace-selection", test_workspace_selection);
    g_test_add_func("/notion/curl-allowlist", test_curl_capture_allowlist);
    g_test_add_func("/notion/window-boundaries-months", test_window_boundaries_and_month_lengths);
    g_test_add_func("/notion/credential-api-errors", test_credential_and_api_errors);
    g_test_add_func("/notion/parse-errors", test_parse_errors);
    g_test_add_func("/notion/cancellation", test_cancellation);
    int result = g_test_run();
    g_free(spaces_fixture);
    g_free(status_fixture);
    return result;
}
