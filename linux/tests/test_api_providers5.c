#include "api_providers5.h"

#include <json-c/json.h>
#include <string.h>

typedef enum {
    TRANSPORT_LITELLM,
    TRANSPORT_SUB2API,
    TRANSPORT_BEDROCK,
    TRANSPORT_UNEXPECTED,
} TransportMode;

typedef struct {
    TransportMode mode;
    const char *bodies[8];
    long statuses[8];
    guint response_count;
    guint count;
    GCancellable *cancellable;
    gboolean cancel_after_first;
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
    (void)error;
    guint index = fixture.count++;
    g_assert_cmpuint(index, <, fixture.response_count);
    g_assert_true(request->cancellable == fixture.cancellable);
    g_assert_cmpint(request->timeout_seconds, ==, 15);
    if (fixture.mode == TRANSPORT_LITELLM) {
        g_assert_cmpstr(request_header(request, "Authorization"), ==, "Bearer lite-key");
        g_assert_cmpstr(request_header(request, "Accept"), ==, "application/json");
        g_assert_cmpint(request->maximum_response_bytes, ==, 1024U * 1024U);
        g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_ALLOW_PRIVATE_HTTP);
        g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
        g_assert_cmpstr(request->method, ==, "GET");
        if (index == 0) {
            g_assert_cmpstr(request->url, ==, "https://lite.test/proxy/key/info");
        } else {
            g_assert_cmpstr(request->url, ==, "https://lite.test/proxy/user/info?user_id=user-123");
        }
    } else if (fixture.mode == TRANSPORT_SUB2API) {
        g_assert_cmpstr(request_header(request, "Authorization"), ==, "Bearer group-key");
        g_assert_cmpint(request->maximum_response_bytes, ==, 1024U * 1024U);
        g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP);
        g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_SAME_ORIGIN);
        g_assert_cmpstr(request->url, ==, "http://127.0.0.1:8080/root/v1/usage?days=30&timezone=UTC");
    } else if (fixture.mode == TRANSPORT_BEDROCK) {
        g_assert_cmpstr(request->method, ==, "POST");
        g_assert_cmpint(request->maximum_response_bytes, ==, 4U * 1024U * 1024U);
        g_assert_cmpint(request->protocol_policy, ==, CODEXBAR_HTTP_ALLOW_LOOPBACK_HTTP);
        g_assert_cmpint(request->redirect_policy, ==, CODEXBAR_HTTP_REDIRECT_DENY);
        g_assert_nonnull(request_header(request, "X-Amz-Date"));
        g_assert_nonnull(request_header(request, "X-Amz-Content-SHA256"));
        const char *authorization = request_header(request, "Authorization");
        g_assert_nonnull(authorization);
        json_object *body = json_tokener_parse(request->body);
        g_assert_nonnull(body);
        if (index < 2) {
            g_assert_cmpstr(request->url, ==, "https://ce.test");
            g_assert_cmpstr(request_header(request, "X-Amz-Target"),
                            ==,
                            "AWSInsightsIndexService.GetCostAndUsage");
            g_assert_nonnull(strstr(authorization, "/us-east-1/ce/aws4_request"));
            if (index == 0) {
                g_assert_false(json_object_object_get_ex(body, "NextPageToken", NULL));
            } else {
                g_assert_cmpstr(json_object_get_string(json_object_object_get(body, "NextPageToken")),
                                ==,
                                "cost-page-2");
            }
        } else {
            g_assert_cmpstr(request->url, ==, "https://cw.test");
            g_assert_cmpstr(request_header(request, "X-Amz-Target"),
                            ==,
                            "GraniteServiceVersion20100801.GetMetricData");
            g_assert_nonnull(strstr(authorization, "/us-west-2/monitoring/aws4_request"));
            json_object *queries = json_object_object_get(body, "MetricDataQueries");
            g_assert_cmpuint(json_object_array_length(queries), ==, 3);
            if (index == 3) {
                g_assert_cmpstr(json_object_get_string(json_object_object_get(body, "NextToken")),
                                ==,
                                "cw-page-2");
            }
        }
        json_object_put(body);
    } else {
        g_assert_not_reached();
    }
    CodexBarHttpResponse *response = make_response(fixture.statuses[index], fixture.bodies[index]);
    if (fixture.cancel_after_first && index == 0) g_cancellable_cancel(fixture.cancellable);
    return response;
}

static CodexBarHttpResponse *unexpected_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)request;
    (void)error;
    g_assert_not_reached();
}

static CodexBarHttpResponse *oversized_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    CodexBarHttpResponse *response = make_response(200, "");
    g_free(response->body);
    response->body_length = request->maximum_response_bytes + 1;
    response->body = g_malloc0(response->body_length + 1);
    return response;
}

static CodexBarHttpResponse *cost_only_transport(const CodexBarHttpRequest *request, GError **error) {
    (void)error;
    g_assert_cmpstr(request->url, ==, "https://ce.test");
    return make_response(
        200,
        "{\"ResultsByTime\":[{\"Groups\":[{\"Keys\":[\"Amazon Bedrock\"],"
        "\"Metrics\":{\"UnblendedCost\":{\"Amount\":\"7.5\"}}}]}]}");
}

static CodexBarHttpResponse *profile_cost_transport(const CodexBarHttpRequest *request, GError **error) {
    g_assert_nonnull(strstr(request_header(request, "Authorization"), "Credential=AKIAPROFILE/"));
    g_assert_cmpstr(request_header(request, "X-Amz-Security-Token"), ==, "profile-token");
    return cost_only_transport(request, error);
}

static void reset_fixture(TransportMode mode, guint response_count) {
    memset(&fixture, 0, sizeof(fixture));
    fixture.mode = mode;
    fixture.response_count = response_count;
}

static void clear_provider_environment(void) {
    const char *keys[] = {
        "LITELLM_API_KEY",
        "LITELLM_BASE_URL",
        "SUB2API_API_KEY",
        "SUB2API_BASE_URL",
        "AWS_ACCESS_KEY_ID",
        "AWS_SECRET_ACCESS_KEY",
        "AWS_SESSION_TOKEN",
        "AWS_REGION",
        "AWS_DEFAULT_REGION",
        "AWS_PROFILE",
        "AWS_CLI_PATH",
        "CODEXBAR_BEDROCK_AUTH_MODE",
        "CODEXBAR_BEDROCK_BUDGET",
        "CODEXBAR_BEDROCK_API_URL",
        "CODEXBAR_BEDROCK_CLOUDWATCH_API_URL",
        "TZ",
        NULL,
    };
    for (size_t index = 0; keys[index]; index++) g_unsetenv(keys[index]);
}

static void test_litellm_user_parser(void) {
    const char *key = "{\"info\":{\"key_name\":\"virtual\",\"spend\":212.35,"
                      "\"expires\":\"2026-09-11T00:12:55.950000+00:00\","
                      "\"user_id\":\"user-123\",\"team_id\":\"team-456\"}}";
    const char *user = "{\"user_id\":\"user-123\",\"user_info\":{\"user_id\":\"user-123\","
                       "\"user_email\":\"user@example.com\",\"max_budget\":300,\"spend\":212.35},"
                       "\"teams\":[{\"team_id\":\"other\",\"max_budget\":5,\"spend\":4},"
                       "{\"team_id\":\"team-456\",\"team_alias\":\"ai\",\"max_budget\":1000,"
                       "\"spend\":215.3,\"budget_reset_at\":\"2026-06-15T00:00:00Z\"}]}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_litellm_parse_usage(key, user, 1000, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->provider, ==, "litellm");
    g_assert_cmpstr(provider->account, ==, "user@example.com");
    g_assert_cmpstr(provider->identity->organization, ==, "ai");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    g_assert_cmpfloat_with_epsilon(codexbar_provider_quota_window(provider, 0)->used_percent,
                                  212.35 / 300 * 100,
                                  0.0001);
    g_assert_cmpfloat_with_epsilon(codexbar_provider_quota_window(provider, 1)->used_percent,
                                  215.3 / 1000 * 100,
                                  0.0001);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 212.35);
    g_assert_cmpfloat(provider->provider_cost->limit, ==, 300);
    g_assert_true(provider->has_subscription_expires_at);
    codexbar_provider_free(provider);

    provider = codexbar_litellm_parse_usage(
        key,
        "{\"user_id\":\"other\",\"user_info\":{\"spend\":1}}",
        1,
        &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_litellm_team_and_fetch(void) {
    const char *key = "{\"info\":{\"key_name\":\"team-key\",\"team_id\":\"team-456\"}}";
    const char *team = "{\"team_id\":\"team-456\",\"team_info\":{\"team_id\":\"team-456\","
                       "\"team_alias\":\"platform\",\"max_budget\":100,\"spend\":25}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_litellm_parse_usage(key, team, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    g_assert_cmpstr(codexbar_provider_quota_window(provider, 0)->id, ==, "secondary");
    g_assert_cmpfloat(provider->provider_cost->used, ==, 25);
    g_assert_cmpstr(provider->provider_cost->period, ==, "Team budget");
    codexbar_provider_free(provider);

    clear_provider_environment();
    CodexBarProviderConfig config = {.api_key = " 'lite-key' ", .enterprise_host = "https://lite.test/proxy/v1/"};
    reset_fixture(TRANSPORT_LITELLM, 2);
    fixture.statuses[0] = fixture.statuses[1] = 200;
    fixture.bodies[0] = "{\"info\":{\"user_id\":\"user-123\"}}";
    fixture.bodies[1] =
        "{\"user_info\":{\"user_id\":\"user-123\",\"max_budget\":10,\"spend\":2}}";
    provider = codexbar_litellm_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 2);
    codexbar_provider_free(provider);

    config.enterprise_host = "http://public.example.com";
    provider = codexbar_litellm_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_nonnull(error);
    g_clear_error(&error);
}

static void test_sub2api_parser(void) {
    const char *json =
        "{\"mode\":\"quota_limited\",\"isValid\":true,\"planName\":\"Enterprise\","
        "\"balance\":42.5,\"unit\":\"USD\",\"quota\":{\"limit\":100,\"used\":25,"
        "\"remaining\":75},\"rate_limits\":[{\"window\":\"5h\",\"limit\":20,\"used\":5,"
        "\"remaining\":15,\"reset_at\":\"2026-07-11T12:30:00Z\"}],"
        "\"usage\":{\"today\":{\"requests\":4,\"total_tokens\":1200,\"actual_cost\":1.25}}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_sub2api_parse_usage(json, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpstr(provider->plan, ==, "Enterprise");
    g_assert_cmpuint(provider->quota_windows->len, ==, 2);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 25);
    g_assert_cmpstr(codexbar_provider_quota_window(provider, 1)->id, ==, "5h");
    g_assert_cmpint(codexbar_provider_quota_window(provider, 1)->window_minutes, ==, 300);
    g_assert_cmpuint(provider->balances->len, ==, 2);
    json_object *details = json_object_object_get(provider->usage_extensions, "sub2APIUsage");
    g_assert_cmpstr(json_object_get_string(json_object_object_get(details, "kind")), ==, "keyQuota");
    g_assert_cmpint(json_object_get_int(json_object_object_get(json_object_object_get(details, "today"), "requests")),
                    ==,
                    4);
    codexbar_provider_free(provider);

    provider = codexbar_sub2api_parse_usage("{\"isValid\":false}", 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED);
    g_clear_error(&error);
    static const char embedded_nul[] = "{\"isValid\":true}\0junk";
    provider = codexbar_sub2api_parse_usage_bytes(embedded_nul, sizeof(embedded_nul) - 1, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_sub2api_subscription_and_fetch(void) {
    const char *json =
        "{\"subscription\":{\"daily_usage_usd\":2,\"weekly_usage_usd\":10,"
        "\"monthly_usage_usd\":30,\"daily_limit_usd\":10,\"weekly_limit_usd\":40,"
        "\"monthly_limit_usd\":100,\"expires_at\":\"2026-08-15T00:00:00.123Z\"}}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_sub2api_parse_usage(json, 1, &error);
    g_assert_no_error(error);
    g_assert_cmpuint(provider->quota_windows->len, ==, 3);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 20);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 1)->used_percent, ==, 25);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 2)->used_percent, ==, 30);
    g_assert_true(provider->has_subscription_expires_at);
    codexbar_provider_free(provider);

    clear_provider_environment();
    g_setenv("TZ", "UTC", TRUE);
    CodexBarProviderConfig config = {
        .api_key = "group-key",
        .enterprise_host = "http://127.0.0.1:8080/root",
    };
    reset_fixture(TRANSPORT_SUB2API, 1);
    fixture.statuses[0] = 200;
    fixture.bodies[0] = "{\"mode\":\"unrestricted\",\"balance\":5}";
    provider = codexbar_sub2api_fetch_with_transport(&config, stub_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    codexbar_provider_free(provider);

    config.enterprise_host = "https://user:pass@example.com";
    provider = codexbar_sub2api_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_nonnull(error);
    g_clear_error(&error);
}

static const char *bedrock_cost_page_one(void) {
    return "{\"NextPageToken\":\"cost-page-2\",\"ResultsByTime\":[{\"Groups\":["
           "{\"Keys\":[\"Amazon EC2\"],\"Metrics\":{\"UnblendedCost\":{\"Amount\":\"5\"}}}]}]}";
}

static const char *bedrock_cost_page_two(void) {
    return "{\"ResultsByTime\":[{\"Groups\":["
           "{\"Keys\":[\"Amazon Bedrock\"],\"Metrics\":{\"UnblendedCost\":{\"Amount\":\"12\"}}},"
           "{\"Keys\":[\"Claude Sonnet (Bedrock Edition)\"],"
           "\"Metrics\":{\"UnblendedCost\":{\"Amount\":\"8.5\"}}}]}]}";
}

static CodexBarProviderConfig bedrock_config(void) {
    return (CodexBarProviderConfig){
        .api_key = "AKIATEST",
        .secret_key = "testSecret",
        .region = "us-west-2",
    };
}

static void test_bedrock_parser(void) {
    clear_provider_environment();
    g_setenv("CODEXBAR_BEDROCK_BUDGET", "100", TRUE);
    CodexBarProviderConfig config = bedrock_config();
    const char *cost =
        "{\"ResultsByTime\":[{\"Groups\":[{\"Keys\":[\"Amazon Bedrock\"],"
        "\"Metrics\":{\"UnblendedCost\":{\"Amount\":\"42.5\"}}},"
        "{\"Keys\":[\"Amazon EC2\"],\"Metrics\":{\"UnblendedCost\":{\"Amount\":\"100\"}}}]}]}";
    const char *cloudwatch =
        "{\"MetricDataResults\":[{\"Id\":\"inputTokens\",\"StatusCode\":\"Complete\","
        "\"Values\":[1000,2500]},{\"Id\":\"outputTokens\",\"StatusCode\":\"Complete\","
        "\"Values\":[400,600]},{\"Id\":\"requests\",\"StatusCode\":\"Complete\","
        "\"Values\":[7,8]}]}";
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_bedrock_parse_usage(cost, cloudwatch, &config, 1750334400000, &error);
    g_assert_no_error(error);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 42.5);
    g_assert_cmpfloat(provider->provider_cost->limit, ==, 100);
    g_assert_cmpuint(provider->quota_windows->len, ==, 1);
    g_assert_cmpfloat(codexbar_provider_quota_window(provider, 0)->used_percent, ==, 42.5);
    g_assert_nonnull(strstr(provider->identity->login_method, "4500 tokens"));
    g_assert_nonnull(strstr(provider->identity->login_method, "Requests: 15"));
    codexbar_provider_free(provider);

    provider = codexbar_bedrock_parse_usage("{}", NULL, &config, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

static void test_bedrock_fetch_and_signing(void) {
    clear_provider_environment();
    g_setenv("CODEXBAR_BEDROCK_BUDGET", "100", TRUE);
    g_setenv("CODEXBAR_BEDROCK_API_URL", "https://ce.test", TRUE);
    g_setenv("CODEXBAR_BEDROCK_CLOUDWATCH_API_URL", "https://cw.test", TRUE);
    CodexBarProviderConfig config = bedrock_config();
    reset_fixture(TRANSPORT_BEDROCK, 4);
    fixture.statuses[0] = fixture.statuses[1] = fixture.statuses[2] = fixture.statuses[3] = 200;
    fixture.bodies[0] = bedrock_cost_page_one();
    fixture.bodies[1] = bedrock_cost_page_two();
    fixture.bodies[2] = "{\"NextToken\":\"cw-page-2\",\"MetricDataResults\":["
                        "{\"Id\":\"inputTokens\",\"StatusCode\":\"Complete\",\"Values\":[2]}]}";
    fixture.bodies[3] = "{\"MetricDataResults\":["
                        "{\"Id\":\"inputTokens\",\"StatusCode\":\"Complete\",\"Values\":[3]},"
                        "{\"Id\":\"requests\",\"StatusCode\":\"Complete\",\"Values\":[1]}]}";
    GError *error = NULL;
    CodexBarProvider *provider =
        codexbar_bedrock_fetch_with_transport(&config, stub_transport, 1750334400000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpuint(fixture.count, ==, 4);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 20.5);
    g_assert_nonnull(strstr(provider->identity->login_method, "5 tokens"));
    codexbar_provider_free(provider);

    char *authorization = codexbar_bedrock_sign_for_testing("POST",
                                                            "https://ce.us-east-1.amazonaws.com",
                                                            "{}",
                                                            "AKIDEXAMPLE",
                                                            "secret",
                                                            NULL,
                                                            "us-east-1",
                                                            "ce",
                                                            "20260102T030405Z",
                                                            &error);
    g_assert_no_error(error);
    g_assert_cmpstr(
        authorization,
        ==,
        "AWS4-HMAC-SHA256 Credential=AKIDEXAMPLE/20260102/us-east-1/ce/aws4_request, "
        "SignedHeaders=content-type;host;x-amz-content-sha256;x-amz-date;x-amz-target, "
        "Signature=7000f96a888f85ead3393daff0acbf6cd826130605eb40f86fc83a92b016960e");
    g_free(authorization);
}

static void test_bedrock_profile_fetch(void) {
    clear_provider_environment();
    GError *error = NULL;
    char *executable = g_file_read_link("/proc/self/exe", &error);
    g_assert_no_error(error);
    g_assert_nonnull(executable);
    g_assert_true(g_setenv("AWS_CLI_PATH", executable, TRUE));
    g_assert_true(g_setenv("CODEXBAR_BEDROCK_API_URL", "https://ce.test", TRUE));
    CodexBarProviderConfig config = {
        .aws_auth_mode = "profile",
        .aws_profile = "work",
    };
    g_assert_true(codexbar_bedrock_has_credentials(&config));
    g_assert_false(codexbar_bedrock_has_static_credentials(&config));
    CodexBarProvider *provider =
        codexbar_bedrock_fetch_with_transport(&config, profile_cost_transport, 1750334400000, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpstr(provider->identity->organization, ==, "ap-southeast-2");
    g_assert_cmpfloat(provider->provider_cost->used, ==, 7.5);
    codexbar_provider_free(provider);
    g_free(executable);
}

static void test_security_cancellation_and_limits(void) {
    clear_provider_environment();
    CodexBarProviderConfig config = {.api_key = "bad key", .enterprise_host = "https://lite.test"};
    GError *error = NULL;
    g_assert_false(codexbar_litellm_has_credentials(&config));
    CodexBarProvider *provider =
        codexbar_litellm_fetch_with_transport(&config, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND);
    g_clear_error(&error);

    config = (CodexBarProviderConfig){.api_key = "lite-key", .enterprise_host = "https://lite.test/proxy/v1"};
    reset_fixture(TRANSPORT_LITELLM, 1);
    fixture.statuses[0] = 200;
    fixture.bodies[0] = "{\"info\":{\"user_id\":\"user-123\"}}";
    fixture.cancellable = g_cancellable_new();
    fixture.cancel_after_first = TRUE;
    provider = codexbar_litellm_fetch_with_transport_and_cancellable(
        &config, stub_transport, fixture.cancellable, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_CANCELLED);
    g_clear_error(&error);
    g_object_unref(fixture.cancellable);

    provider = codexbar_litellm_fetch_with_transport(&config, oversized_transport, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_NO_SPACE);
    g_clear_error(&error);

    clear_provider_environment();
    CodexBarProviderConfig aws = bedrock_config();
    g_assert_true(g_setenv("CODEXBAR_BEDROCK_API_URL", "http://aws.example.com", TRUE));
    g_assert_cmpstr(g_getenv("CODEXBAR_BEDROCK_API_URL"), ==, "http://aws.example.com");
    provider = codexbar_bedrock_fetch_with_transport(&aws, unexpected_transport, 1, &error);
    g_assert_null(provider);
    g_assert_nonnull(error);
    g_clear_error(&error);

    g_assert_true(g_setenv("CODEXBAR_BEDROCK_API_URL", "https://ce.test", TRUE));
    g_assert_true(g_setenv("CODEXBAR_BEDROCK_CLOUDWATCH_API_URL", "http://cloudwatch.example.com", TRUE));
    g_assert_cmpstr(g_getenv("CODEXBAR_BEDROCK_API_URL"), ==, "https://ce.test");
    provider = codexbar_bedrock_fetch_with_transport(&aws, cost_only_transport, 1, &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpfloat(provider->provider_cost->used, ==, 7.5);
    codexbar_provider_free(provider);

    static const char embedded_nul[] = "{\"ResultsByTime\":[]}\0junk";
    provider = codexbar_bedrock_parse_usage_bytes(
        embedded_nul, sizeof(embedded_nul) - 1, NULL, 0, &aws, 1, &error);
    g_assert_null(provider);
    g_assert_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA);
    g_clear_error(&error);
}

int main(int argc, char **argv) {
    if (argc > 2 && g_str_equal(argv[1], "configure") && g_str_equal(argv[2], "export-credentials")) {
        g_print("{\"Version\":1,\"AccessKeyId\":\"AKIAPROFILE\","
                "\"SecretAccessKey\":\"profile-secret\",\"SessionToken\":\"profile-token\"}");
        return 0;
    }
    if (argc > 3 && g_str_equal(argv[1], "configure") && g_str_equal(argv[2], "get") &&
        g_str_equal(argv[3], "region")) {
        g_print("ap-southeast-2\n");
        return 0;
    }
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/api5/litellm/user-parser", test_litellm_user_parser);
    g_test_add_func("/api5/litellm/team-fetch", test_litellm_team_and_fetch);
    g_test_add_func("/api5/sub2api/parser", test_sub2api_parser);
    g_test_add_func("/api5/sub2api/subscription-fetch", test_sub2api_subscription_and_fetch);
    g_test_add_func("/api5/bedrock/parser", test_bedrock_parser);
    g_test_add_func("/api5/bedrock/fetch-signing", test_bedrock_fetch_and_signing);
    g_test_add_func("/api5/bedrock/profile-fetch", test_bedrock_profile_fetch);
    g_test_add_func("/api5/security", test_security_cancellation_and_limits);
    int result = g_test_run();
    clear_provider_environment();
    return result;
}
