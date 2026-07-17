#include "codex.h"
#include "model.h"
#include "openrouter.h"
#include "simple_providers.h"
#include "render.h"

#include <glib.h>
#include <json-c/json.h>

static const char *fixture =
    "[{"
    "\"provider\":\"codex\","
    "\"account\":\"dev@example.com\","
    "\"plan\":\"Pro\","
    "\"source\":\"oauth\","
    "\"note\":\"plan expires Jul 31, 2026\","
    "\"usage\":{"
    "\"primary\":{\"label\":\"session\",\"usedPercent\":28,"
    "\"resetDescription\":\"28 / 100 requests\","
    "\"resetsAt\":\"resets Thu, Jul 23 at 10:16\"},"
    "\"secondary\":{\"usedPercent\":71.4,\"resetDescription\":\"Resets Friday\"},"
    "\"tertiary\":null},"
    "\"credits\":{\"label\":\"balance\",\"remaining\":12.5}"
    "},{"
    "\"provider\":\"claude\","
    "\"source\":\"cli\","
    "\"usage\":{\"primary\":{\"usedPercent\":91}},"
    "\"error\":null"
    "}]";

static void test_parse_snapshot(void) {
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(fixture, &error);
    g_assert_no_error(error);
    g_assert_nonnull(snapshot);
    g_assert_cmpuint(snapshot->providers->len, ==, 2);

    CodexBarProvider *codex = g_ptr_array_index(snapshot->providers, 0);
    g_assert_cmpstr(codex->provider, ==, "codex");
    g_assert_cmpstr(codex->account, ==, "dev@example.com");
    g_assert_cmpstr(codex->plan, ==, "Pro");
    g_assert_cmpstr(codex->note, ==, "plan expires Jul 31, 2026");
    g_assert_true(codex->primary.available);
    g_assert_cmpstr(codex->primary.label, ==, "session");
    g_assert_cmpfloat(codex->primary.used_percent, ==, 28.0);
    g_assert_true(codex->has_credits);
    g_assert_cmpstr(codex->credits_label, ==, "balance");
    g_assert_cmpfloat(codex->credits_remaining, ==, 12.5);
    g_assert_cmpfloat(codexbar_snapshot_highest_used(snapshot), ==, 91.0);
    codexbar_snapshot_free(snapshot);
}

static void test_rejects_non_array(void) {
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse("{}", &error);
    g_assert_null(snapshot);
    g_assert_error(error, g_quark_from_static_string("codexbar-model-error"), 2);
    g_clear_error(&error);
}

static void test_waybar_rendering(void) {
    GError *error = NULL;
    CodexBarSnapshot *snapshot = codexbar_snapshot_parse(fixture, &error);
    g_assert_no_error(error);

    char *rendered = codexbar_render_waybar(snapshot);
    json_object *object = json_tokener_parse(rendered);
    g_assert_nonnull(object);

    json_object *class_name = NULL;
    json_object *percentage = NULL;
    json_object *tooltip = NULL;
    g_assert_true(json_object_object_get_ex(object, "class", &class_name));
    g_assert_true(json_object_object_get_ex(object, "percentage", &percentage));
    g_assert_true(json_object_object_get_ex(object, "tooltip", &tooltip));
    g_assert_cmpstr(json_object_get_string(class_name), ==, "critical");
    g_assert_cmpint(json_object_get_int(percentage), ==, 91);
    const char *tooltip_text = json_object_get_string(tooltip);
    g_assert_true(g_str_has_prefix(tooltip_text, "codex · dev@example.com"));
    g_assert_null(strstr(tooltip_text, "CODEXBAR // USAGE"));
    g_assert_nonnull(strstr(tooltip_text, "Pro · oauth"));
    g_assert_nonnull(strstr(tooltip_text, "plan expires Jul 31, 2026"));
    g_assert_nonnull(strstr(tooltip_text, "28% used · 72% left"));
    g_assert_nonnull(strstr(tooltip_text, "███░░░░░░░"));
    g_assert_nonnull(strstr(tooltip_text, "28 / 100 requests"));
    g_assert_nonnull(strstr(tooltip_text, "resets Thu, Jul 23 at 10:16"));
    g_assert_nonnull(strstr(json_object_get_string(tooltip), "claude"));

    json_object_put(object);
    g_free(rendered);
    codexbar_snapshot_free(snapshot);
}

static void test_openrouter_credits(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_openrouter_parse_credits(
        "{\"data\":{\"total_credits\":100,\"total_usage\":27.5}}", &error);
    g_assert_no_error(error);
    g_assert_nonnull(provider);
    g_assert_cmpfloat_with_epsilon(provider->credits_remaining, 72.5, 0.0001);
    g_assert_cmpfloat_with_epsilon(provider->primary.used_percent, 27.5, 0.0001);
    codexbar_provider_free(provider);
}

static void test_simple_provider_parsers(void) {
    GError *error = NULL;
    CodexBarProvider *deepseek = codexbar_deepseek_parse(
        "{\"is_available\":true,\"balance_infos\":[{\"total_balance\":\"8.25\"}]}", &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(deepseek->credits_remaining, 8.25, 0.0001);
    codexbar_provider_free(deepseek);

    CodexBarProvider *moonshot = codexbar_moonshot_parse(
        "{\"code\":0,\"status\":true,\"data\":{\"available_balance\":12.5}}", &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(moonshot->credits_remaining, 12.5, 0.0001);
    codexbar_provider_free(moonshot);

    CodexBarProvider *elevenlabs = codexbar_elevenlabs_parse(
        "{\"character_count\":250,\"character_limit\":1000}", &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(elevenlabs->primary.used_percent, 25.0, 0.0001);
    codexbar_provider_free(elevenlabs);

    CodexBarProvider *crof = codexbar_crof_parse(
        "{\"credits\":9.9999,\"requests_plan\":1000,\"usable_requests\":998}", &error);
    g_assert_no_error(error);
    g_assert_cmpstr(crof->primary.label, ==, "requests");
    g_assert_cmpfloat_with_epsilon(crof->primary.used_percent, 1.0, 0.0001);
    g_assert_cmpstr(crof->primary.reset_description, ==, "998 requests left");
    g_assert_nonnull(strstr(crof->primary.resets_at, "resets "));
    g_assert_cmpstr(crof->secondary.label, ==, "balance");
    g_assert_cmpstr(crof->secondary.reset_description, ==, "$9.99");
    codexbar_provider_free(crof);

    crof = codexbar_crof_parse("{\"credits\":0,\"requests_plan\":0,\"usable_requests\":0}", &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(crof->primary.used_percent, 100.0, 0.0001);
    codexbar_provider_free(crof);

    crof = codexbar_crof_parse("{\"credits\":0,\"requests_plan\":1000,\"usable_requests\":1200}", &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(crof->primary.used_percent, 0.0, 0.0001);
    g_assert_cmpstr(crof->primary.reset_description, ==, "1200 requests left");
    codexbar_provider_free(crof);

    CodexBarProvider *venice = codexbar_venice_parse(
        "{\"canConsume\":true,\"consumptionCurrency\":\"BUNDLED_CREDITS\","
        "\"balances\":{\"diem\":\"50.0\",\"usd\":10.0},\"diemEpochAllocation\":\"100.0\"}",
        &error);
    g_assert_no_error(error);
    g_assert_cmpstr(venice->primary.label, ==, "balance");
    g_assert_cmpfloat_with_epsilon(venice->primary.used_percent, 50.0, 0.0001);
    g_assert_cmpstr(venice->primary.reset_description, ==, "DIEM 50.00 / 100.00 epoch allocation");
    codexbar_provider_free(venice);

    venice = codexbar_venice_parse(
        "{\"canConsume\":true,\"balances\":{\"diem\":\"not-a-number\",\"usd\":null}}", &error);
    g_assert_null(venice);
    g_assert_error(error, g_quark_from_static_string("codexbar-simple-provider-error"), 8);
    g_clear_error(&error);

    venice = codexbar_venice_parse(
        "{\"canConsume\":true,\"consumptionCurrency\":null,"
        "\"balances\":{\"diem\":\"   \",\"usd\":null},\"diemEpochAllocation\":null}",
        &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(venice->primary.used_percent, 100.0, 0.0001);
    codexbar_provider_free(venice);

    CodexBarProvider *zenmux = codexbar_zenmux_parse_subscription(
        "{\"success\":true,\"data\":{\"plan\":{\"tier\":\"ultra\","
        "\"expires_at\":\"2026-04-12T08:26:56.000Z\"},\"account_status\":\"healthy\","
        "\"quota_5_hour\":{\"usage_percentage\":0.0715,\"resets_at\":\"2026-03-24T08:35:09.000Z\","
        "\"max_flows\":800,\"used_flows\":57.2,\"remaining_flows\":742.8},"
        "\"quota_7_day\":{\"usage_percentage\":0.0673,\"resets_at\":\"2026-03-26T02:15:05.000Z\","
        "\"max_flows\":6182,\"used_flows\":416.11,\"remaining_flows\":5765.89}}}",
        &error);
    g_assert_no_error(error);
    g_assert_cmpstr(zenmux->plan, ==, "Ultra plan");
    g_assert_nonnull(strstr(zenmux->note, "plan expires "));
    g_assert_nonnull(strstr(zenmux->note, "2026"));
    g_assert_cmpstr(zenmux->primary.label, ==, "5-hour");
    g_assert_cmpfloat_with_epsilon(zenmux->primary.used_percent, 7.15, 0.0001);
    g_assert_cmpstr(zenmux->primary.reset_description, ==, "57.20 / 800 flows");
    g_assert_nonnull(strstr(zenmux->primary.resets_at, "Mar"));
    g_assert_cmpstr(zenmux->secondary.label, ==, "weekly");
    g_assert_true(codexbar_zenmux_apply_payg(
        zenmux, "{\"success\":true,\"data\":{\"currency\":\"usd\",\"total_credits\":482.74}}", &error));
    g_assert_no_error(error);
    g_assert_cmpstr(zenmux->credits_label, ==, "pay as you go");
    g_assert_cmpfloat_with_epsilon(zenmux->credits_remaining, 482.74, 0.0001);
    g_assert_true(codexbar_zenmux_apply_payg(
        zenmux, "{\"success\":true,\"data\":{\"currency\":\"usd\",\"total_credits\":10}}", &error));
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(zenmux->credits_remaining, 10.0, 0.0001);
    g_assert_false(codexbar_zenmux_apply_payg(
        zenmux, "{\"success\":true,\"data\":{\"currency\":\"eur\",\"total_credits\":10}}", &error));
    g_assert_error(error, g_quark_from_static_string("codexbar-simple-provider-error"), 10);
    g_clear_error(&error);
    codexbar_provider_free(zenmux);

    zenmux = codexbar_zenmux_parse_subscription(
        "{\"success\":true,\"data\":{\"plan\":{},\"quota_5_hour\":{},\"quota_7_day\":{}}}", &error);
    g_assert_null(zenmux);
    g_assert_error(error, g_quark_from_static_string("codexbar-simple-provider-error"), 9);
    g_clear_error(&error);
}

static void test_codex_rate_limits(void) {
    GError *error = NULL;
    CodexBarProvider *provider = codexbar_codex_parse_rate_limits(
        "{\"id\":2,\"result\":{\"rateLimits\":{\"primary\":{\"usedPercent\":28,\"windowDurationMins\":300,\"resetsAt\":1776216359},\"secondary\":{\"usedPercent\":71,\"windowDurationMins\":10080,\"resetsAt\":1776395384},\"credits\":{\"hasCredits\":true,\"balance\":\"12.5\"}}}}",
        &error);
    g_assert_no_error(error);
    g_assert_cmpfloat_with_epsilon(provider->primary.used_percent, 28.0, 0.0001);
    g_assert_cmpfloat_with_epsilon(provider->secondary.used_percent, 71.0, 0.0001);
    g_assert_cmpfloat_with_epsilon(provider->credits_remaining, 12.5, 0.0001);
    g_assert_nonnull(strstr(provider->primary.reset_description, "2026"));

    g_assert_true(codexbar_codex_apply_account(
        provider,
        "{\"id\":3,\"result\":{\"account\":{\"type\":\"chatgpt\",\"email\":\"dev@example.com\",\"planType\":\"pro\"}}}",
        &error));
    g_assert_no_error(error);
    g_assert_cmpstr(provider->account, ==, "dev@example.com");
    g_assert_cmpstr(provider->plan, ==, "Pro");
    codexbar_provider_free(provider);
}

int main(int argc, char **argv) {
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/model/parse-snapshot", test_parse_snapshot);
    g_test_add_func("/model/reject-non-array", test_rejects_non_array);
    g_test_add_func("/render/waybar", test_waybar_rendering);
    g_test_add_func("/provider/openrouter-credits", test_openrouter_credits);
    g_test_add_func("/provider/simple-parsers", test_simple_provider_parsers);
    g_test_add_func("/provider/codex-rate-limits", test_codex_rate_limits);
    return g_test_run();
}
