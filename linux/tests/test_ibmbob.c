#include "ibmbob.h"

#include <gio/gio.h>
#include <json-c/json.h>
#include <math.h>
#include <string.h>

typedef struct {
    const char *urls[4];
    const char *bodies[4];
    const char *instance_ids[4];
    const char *team_ids[4];
    long statuses[4];
    const char *authorization;
    GCancellable *cancellable;
    gboolean cancel_after_response;
    gboolean fail_request;
    guint calls;
    guint response_count;
} TransportFixture;

static TransportFixture fixture;

static CodexBarHttpResponse *make_response(long status, const char *body) {
    CodexBarHttpResponse *response = g_new0(CodexBarHttpResponse, 1);
    response->status = status;
    response->body = g_strdup(body ? body : "");
    response->body_length = strlen(response->body);
    response->headers = g_ptr_array_new();
    return response;
}

static const char *request_header(const CodexBarHttpRequest *request, const char *name) {
    for (size_t index = 0; index < request->header_count; index++) {
        if (g_ascii_strcasecmp(request->headers[index].name, name) == 0) return request->headers[index].value;
    }
    return NULL;
}

static CodexBarHttpResponse *stub_transport(const CodexBarHttpRequest *request, GError **error) {
    guint index = fixture.calls++;
    g_assert_cmpuint(index, <, fixture.response_count);
    g_assert_cmpstr(request->url, ==, fixture.urls[index]);
    g_assert_cmpstr(request->method, ==, "GET");
    g_assert_cmpstr(request_header(request, "Authorization"), ==, fixture.authorization);
    g_assert_cmpstr(request_header(request, "Accept"), ==, "application/json");
    g_assert_cmpstr(request_header(request, "Content-Type"), ==, "application/json");
    g_assert_cmpstr(request_header(request, "User-Agent"), ==, "CodexBar");
    g_assert_cmpint(request->timeout_seconds, ==, 20);
    g_assert_cmpuint(request->maximum_response_bytes, ==, 1024U * 1024U);
    g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_HTTPS_ONLY);
    g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_DENY);
    g_assert_true(request->cancellable == fixture.cancellable);
    if (index == 0) {
        g_assert_null(request_header(request, "x-instance-id"));
        g_assert_null(request_header(request, "x-team-id"));
    } else {
        g_assert_nonnull(request_header(request, "x-instance-id"));
        g_assert_nonnull(request_header(request, "x-team-id"));
        if (fixture.instance_ids[index]) {
            g_assert_cmpstr(request_header(request, "x-instance-id"), ==, fixture.instance_ids[index]);
        }
        if (fixture.team_ids[index]) {
            g_assert_cmpstr(request_header(request, "x-team-id"), ==, fixture.team_ids[index]);
        }
    }
    if (fixture.fail_request) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "fixture network failure");
        return NULL;
    }
    CodexBarHttpResponse *response = make_response(fixture.statuses[index], fixture.bodies[index]);
    if (fixture.cancel_after_response) g_cancellable_cancel(fixture.cancellable);
    return response;
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static void reset_fixture(const char *authorization) {
    fixture = (TransportFixture){
        .authorization = authorization,
    };
}

static void add_response(const char *url, long status, const char *body) {
    guint index = fixture.response_count++;
    g_assert_cmpuint(index, <, G_N_ELEMENTS(fixture.urls));
    fixture.urls[index] = url;
    fixture.statuses[index] = status;
    fixture.bodies[index] = body;
}

static json_object *object_member(json_object *object, const char *name) {
    json_object *value = NULL;
    g_assert_true(json_object_object_get_ex(object, name, &value));
    return value;
}

static void test_multi_team_aggregation(void) {
    const char *profile =
        "{\"instances\":["
        "{\"instance_id\":\"instance-one\",\"name\":\"Personal\",\"user_id\":\"user-one\","
        "\"plan_name\":\"Pro+\",\"refresh_at\":\"2024-03-01T00:00:00Z\","
        "\"region_domain\":\"us-east.bob.ibm.com\","
        "\"teams\":[{\"id\":\"team-one\",\"name\":\"Solo\",\"budget_limit\":50}]},"
        "{\"instance_id\":\"instance-two\",\"instance_name\":\"Work\",\"user_id\":\"user-two\","
        "\"plan_name\":\"Enterprise\",\"refresh_at\":\"2024-03-05T00:00:00.000Z\","
        "\"region_domain\":\"api.eu-de.bob.ibm.com\","
        "\"teams\":[{\"id\":\"team-two\",\"name\":\"Platform\",\"budget_limit\":160}]}]}";
    reset_fixture("Apikey config-key");
    add_response("https://api.us-east.bob.ibm.com/admin/v1/profile", 200, profile);
    add_response("https://api.us-east.bob.ibm.com/admin/v1/teams/team-one/users/user-one",
                 200,
                 "{\"usage\":10,\"budget_limit\":40}");
    add_response("https://api.eu-de.bob.ibm.com/admin/v1/teams/team-two/users/user-two",
                 200,
                 "{\"usage\":25}");
    fixture.instance_ids[1] = "instance-one";
    fixture.team_ids[1] = "team-one";
    fixture.instance_ids[2] = "instance-two";
    fixture.team_ids[2] = "team-two";
    CodexBarProviderConfig config = {.api_key = "  'config-key'  "};
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_ibmbob_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, G_GINT64_CONSTANT(1707955200000), &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.calls, ==, 3);
    g_assert_cmpstr(provider->provider, ==, "ibmbob");
    g_assert_cmpstr(provider->source, ==, "api");
    g_assert_true(provider->has_updated_at);
    g_assert_cmpint(provider->updated_at_ms, ==, G_GINT64_CONSTANT(1707955200000));
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, 0);
    g_assert_cmpstr(window->id, ==, "primary");
    g_assert_cmpstr(window->title, ==, "Monthly Bobcoins");
    g_assert_true(window->usage_known);
    g_assert_cmpfloat_with_epsilon(window->used_percent, 17.5, 0.000001);
    g_assert_true(window->has_window_minutes);
    g_assert_cmpint(window->window_minutes, ==, 29 * 24 * 60);
    g_assert_true(window->has_resets_at);
    g_assert_cmpint(window->resets_at_ms, ==, G_GINT64_CONSTANT(1709251200000));
    g_assert_nonnull(strstr(window->reset_description, "35 / 200 Bobcoins"));
    g_assert_nonnull(provider->identity);
    g_assert_cmpstr(provider->identity->organization, ==, "Enterprise, Pro+");
    g_assert_cmpstr(provider->identity->login_method, ==, "API key");

    json_object *details = object_member(provider->usage_extensions, "ibmBobUsage");
    g_assert_cmpfloat(json_object_get_double(object_member(details, "usedBobcoins")), ==, 35);
    g_assert_cmpfloat(json_object_get_double(object_member(details, "limitBobcoins")), ==, 200);
    json_object *teams = object_member(details, "teams");
    g_assert_cmpuint(json_object_array_length(teams), ==, 2);
    json_object *first = json_object_array_get_idx(teams, 0);
    g_assert_cmpstr(json_object_get_string(object_member(first, "instanceName")), ==, "Personal");
    g_assert_cmpstr(json_object_get_string(object_member(first, "teamName")), ==, "Solo");
    g_assert_cmpfloat(json_object_get_double(object_member(first, "usedBobcoins")), ==, 10);
    g_assert_cmpfloat(json_object_get_double(object_member(first, "limitBobcoins")), ==, 40);
    g_assert_cmpstr(json_object_get_string(object_member(first, "planName")), ==, "Pro+");
    g_assert_cmpint(json_object_get_int64(object_member(first, "resetsAt")),
                    ==,
                    G_GINT64_CONSTANT(1709251200000));
    g_assert_cmpstr(json_object_get_string(object_member(json_object_array_get_idx(teams, 1), "instanceName")),
                    ==,
                    "Work");
    codexbar_provider_free(provider);
}

static void test_unknown_budget_and_negative_usage(void) {
    const char *profile =
        "{\"instances\":[{\"instance_id\":\"instance\",\"user_id\":\"user\","
        "\"plan_name\":\"Pro\",\"refresh_at\":1709251200,"
        "\"teams\":[{\"id\":\"team\",\"budget_limit\":20}]}]}";
    reset_fixture("Apikey key");
    add_response("https://api.us-east.bob.ibm.com/admin/v1/profile", 200, profile);
    add_response("https://api.us-east.bob.ibm.com/admin/v1/teams/team/users/user",
                 200,
                 "{\"usage\":-5,\"budget_limit\":-1}");
    CodexBarProviderConfig config = {.api_key = "key"};
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_ibmbob_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, G_GINT64_CONSTANT(1707955200000), &error);
    g_assert_no_error(error);
    CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, 0);
    g_assert_false(window->usage_known);
    g_assert_cmpfloat(window->used_percent, ==, 0);
    json_object *details = object_member(provider->usage_extensions, "ibmBobUsage");
    json_object *team = json_object_array_get_idx(object_member(details, "teams"), 0);
    g_assert_cmpfloat(json_object_get_double(object_member(team, "usedBobcoins")), ==, 0);
    json_object *unused = NULL;
    g_assert_false(json_object_object_get_ex(team, "limitBobcoins", &unused));
    g_assert_false(json_object_object_get_ex(details, "limitBobcoins", &unused));
    codexbar_provider_free(provider);
}

static void run_authorization_case(const char *token, const char *authorization) {
    const char *profile =
        "{\"instances\":[{\"instance_id\":\"instance\",\"user_id\":\"user\","
        "\"teams\":[{\"id\":\"team\",\"budget_limit\":10}]}]}";
    reset_fixture(authorization);
    add_response("https://api.us-east.bob.ibm.com/admin/v1/profile", 200, profile);
    add_response("https://api.us-east.bob.ibm.com/admin/v1/teams/team/users/user", 200, "{\"usage\":1}");
    CodexBarProviderConfig config = {.api_key = (char *)token};
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_ibmbob_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    codexbar_provider_free(provider);
}

static void test_jwt_authorization(void) {
    run_authorization_case("header.eyJzdWIiOiJ1c2VyIn0.signature",
                           "Bearer header.eyJzdWIiOiJ1c2VyIn0.signature");
    run_authorization_case("header.W10.signature", "Apikey header.W10.signature");
    run_authorization_case("header.not-base64!.signature", "Apikey header.not-base64!.signature");
    run_authorization_case("header.eyJzdWIiOiJ1c2VyIn0.signature.extra",
                           "Apikey header.eyJzdWIiOiJ1c2VyIn0.signature.extra");
}

static void test_config_precedes_environment(void) {
    g_setenv("BOBSHELL_API_KEY", "environment-key", TRUE);
    reset_fixture("Apikey config-key");
    add_response("https://api.us-east.bob.ibm.com/admin/v1/profile", 200, "{\"instances\":[]}");
    CodexBarProviderConfig config = {.api_key = "config-key"};
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_ibmbob_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

    reset_fixture("Apikey environment-key");
    add_response("https://api.us-east.bob.ibm.com/admin/v1/profile", 200, "{\"instances\":[]}");
    config.api_key = NULL;
    provider = codexbar_ibmbob_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);
    g_unsetenv("BOBSHELL_API_KEY");
}

static void test_rejects_malicious_regions_before_team_request(void) {
    const char *invalid[] = {
        "evil.example",
        "bob.ibm.com.evil.example",
        "evil.example/x.bob.ibm.com",
        "x@evil.example",
        "evil.example/path/.bob.ibm.com",
        "evil.example?next=.bob.ibm.com",
        "evil.example#.bob.ibm.com",
        "evil.example@us-east.bob.ibm.com",
        "us-east.bob.ibm.com:443",
        "bob.ibm.com%2fevil.example",
    };
    CodexBarProviderConfig config = {.api_key = "credential-secret"};
    for (guint index = 0; index < G_N_ELEMENTS(invalid); index++) {
        char *profile = g_strdup_printf(
            "{\"instances\":[{\"instance_id\":\"instance\",\"user_id\":\"user\","
            "\"region_domain\":\"%s\",\"teams\":[{\"id\":\"team\"}]}]}",
            invalid[index]);
        reset_fixture("Apikey credential-secret");
        add_response("https://api.us-east.bob.ibm.com/admin/v1/profile", 200, profile);
        GError *error = NULL;
        CodexBarProvider *provider = codexbar_ibmbob_fetch_with_transport_and_cancellable(
            &config, stub_transport, NULL, 1, &error);
        g_assert_null(provider);
        g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT);
        g_assert_cmpuint(fixture.calls, ==, 1);
        g_assert_null(strstr(error->message, "credential-secret"));
        g_clear_error(&error);
        g_free(profile);
    }
}

static void test_errors_and_cancellation(void) {
    g_unsetenv("BOBSHELL_API_KEY");
    CodexBarProviderConfig config = {0};
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_ibmbob_fetch_with_transport_and_cancellable(
        &config, unexpected_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

    config.api_key = "key";
    GCancellable *cancellable = g_cancellable_new();
    g_cancellable_cancel(cancellable);
    provider = codexbar_ibmbob_fetch_with_transport_and_cancellable(
        &config, unexpected_transport, cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);

    g_cancellable_reset(cancellable);
    reset_fixture("Apikey key");
    fixture.cancellable = cancellable;
    fixture.cancel_after_response = TRUE;
    add_response("https://api.us-east.bob.ibm.com/admin/v1/profile", 200, "{\"instances\":[]}");
    provider = codexbar_ibmbob_fetch_with_transport_and_cancellable(
        &config, stub_transport, cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(cancellable);

    reset_fixture("Apikey key");
    add_response("https://api.us-east.bob.ibm.com/admin/v1/profile", 401, "{\"secret\":\"body-secret\"}");
    provider = codexbar_ibmbob_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_assert_null(strstr(error->message, "body-secret"));
    g_clear_error(&error);

    reset_fixture("Apikey key");
    fixture.fail_request = TRUE;
    add_response("https://api.us-east.bob.ibm.com/admin/v1/profile", 0, NULL);
    provider = codexbar_ibmbob_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_FAILED);
    g_clear_error(&error);
}

static void test_malformed_and_no_subscription(void) {
    CodexBarProviderConfig config = {.api_key = "key"};
    const char *profiles[] = {"{}", "{\"instances\":{}}", "{\"instances\":[]}"};
    const int codes[] = {G_IO_ERROR_INVALID_DATA, G_IO_ERROR_INVALID_DATA, G_IO_ERROR_NOT_FOUND};
    for (guint index = 0; index < G_N_ELEMENTS(profiles); index++) {
        reset_fixture("Apikey key");
        add_response("https://api.us-east.bob.ibm.com/admin/v1/profile", 200, profiles[index]);
        GError *error = NULL;
        CodexBarProvider *provider = codexbar_ibmbob_fetch_with_transport_and_cancellable(
            &config, stub_transport, NULL, 1, &error);
        g_assert_null(provider);
        g_assert_error(error, G_IO_ERROR, codes[index]);
        g_clear_error(&error);
    }

    const char *profile =
        "{\"instances\":[{\"instance_id\":\"instance\",\"user_id\":\"user\","
        "\"teams\":[{\"id\":\"team\",\"budget_limit\":10}]}]}";
    reset_fixture("Apikey key");
    add_response("https://api.us-east.bob.ibm.com/admin/v1/profile", 200, profile);
    add_response("https://api.us-east.bob.ibm.com/admin/v1/teams/team/users/user", 200, "{\"budget_limit\":10}");
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_ibmbob_fetch_with_transport_and_cancellable(
        &config, stub_transport, NULL, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/ibmbob/multi-team-aggregation", test_multi_team_aggregation);
    g_test_add_func("/ibmbob/unknown-budget-negative-usage", test_unknown_budget_and_negative_usage);
    g_test_add_func("/ibmbob/jwt-authorization", test_jwt_authorization);
    g_test_add_func("/ibmbob/config-environment-precedence", test_config_precedes_environment);
    g_test_add_func("/ibmbob/malicious-regions", test_rejects_malicious_regions_before_team_request);
    g_test_add_func("/ibmbob/errors-cancellation", test_errors_and_cancellation);
    g_test_add_func("/ibmbob/malformed-no-subscription", test_malformed_and_no_subscription);
    return g_test_run();
}
