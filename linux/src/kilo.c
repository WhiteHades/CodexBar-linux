#include "kilo.h"

#include "http.h"

#include <errno.h>
#include <gio/gio.h>
#include <glib/gstdio.h>
#include <json-c/json.h>
#include <limits.h>
#include <math.h>
#include <string.h>
#include <sys/stat.h>

#define KILO_API_BASE "https://app.kilo.ai/api/trpc"
#define KILO_PROCEDURES "user.getCreditBlocks,kiloPass.getState,user.getAutoTopUpPaymentMethod"
#define KILO_BATCH_INPUT "{\"0\":{\"json\":null},\"1\":{\"json\":null},\"2\":{\"json\":null}}"
#define KILO_TIMEOUT_SECONDS 30L
#define KILO_MAXIMUM_RESPONSE_BYTES (1024U * 1024U)
#define KILO_MAXIMUM_AUTH_BYTES (1024U * 1024U)

typedef struct {
    gboolean has_used;
    double used;
    gboolean has_total;
    double total;
    gboolean has_remaining;
    double remaining;
    gboolean has_bonus;
    double bonus;
    gboolean has_reset;
    gint64 reset_ms;
} KiloFields;

static json_object *object_member(json_object *object, const char *name) {
    json_object *value = NULL;
    return object && json_object_get_type(object) == json_type_object &&
                   json_object_object_get_ex(object, name, &value)
               ? value
               : NULL;
}

static json_object *parse_json(const char *json, GError **error) {
    if (!json) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Kilo response is empty");
        return NULL;
    }
    size_t length = strlen(json);
    if (length > INT_MAX) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Kilo response is too large");
        return NULL;
    }
    struct json_tokener *tokener = json_tokener_new_ex(32);
    json_object *root = json_tokener_parse_ex(tokener, json, (int)length);
    enum json_tokener_error json_error = json_tokener_get_error(tokener);
    size_t end = json_tokener_get_parse_end(tokener);
    while (end < length && g_ascii_isspace((guchar)json[end])) end++;
    json_tokener_free(tokener);
    if (json_error != json_tokener_success || !root || end != length) {
        if (root) json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Kilo response is not valid JSON");
        return NULL;
    }
    return root;
}

static gboolean json_number(json_object *value, double *result) {
    if (!value) return FALSE;
    enum json_type type = json_object_get_type(value);
    if (type != json_type_int && type != json_type_double) return FALSE;
    double number = json_object_get_double(value);
    if (!isfinite(number)) return FALSE;
    *result = number;
    return TRUE;
}

static json_object *find_member_recursive(
    json_object *value, const char *const *names, size_t count, unsigned depth) {
    if (!value || depth > 8) return NULL;
    if (json_object_get_type(value) == json_type_object) {
        for (size_t index = 0; index < count; index++) {
            json_object *member = object_member(value, names[index]);
            if (member && json_object_get_type(member) != json_type_null) return member;
        }
        json_object_object_foreach(value, key, child) {
            (void)key;
            json_object *found = find_member_recursive(child, names, count, depth + 1);
            if (found) return found;
        }
    } else if (json_object_get_type(value) == json_type_array) {
        size_t length = json_object_array_length(value);
        for (size_t index = 0; index < length; index++) {
            json_object *found =
                find_member_recursive(json_object_array_get_idx(value, index), names, count, depth + 1);
            if (found) return found;
        }
    }
    return NULL;
}

static gboolean number_for_keys(
    json_object *value, const char *const *names, size_t count, double divisor, double *result) {
    double raw = 0;
    if (!json_number(find_member_recursive(value, names, count, 0), &raw)) return FALSE;
    *result = MAX(0.0, raw / divisor);
    return TRUE;
}

static char *string_for_keys(json_object *value, const char *const *names, size_t count) {
    json_object *member = find_member_recursive(value, names, count, 0);
    if (!member || json_object_get_type(member) != json_type_string) return NULL;
    char *text = g_strdup(json_object_get_string(member));
    g_strstrip(text);
    if (text[0] == '\0') {
        g_free(text);
        return NULL;
    }
    return text;
}

static gboolean bool_for_keys(json_object *value, const char *const *names, size_t count, gboolean *result) {
    json_object *member = find_member_recursive(value, names, count, 0);
    if (!member) return FALSE;
    enum json_type type = json_object_get_type(member);
    if (type == json_type_boolean) {
        *result = json_object_get_boolean(member);
        return TRUE;
    }
    if (type == json_type_int) {
        gint64 number = json_object_get_int64(member);
        if (number == 0 || number == 1) {
            *result = number == 1;
            return TRUE;
        }
    }
    if (type == json_type_string) {
        const char *text = json_object_get_string(member);
        if (g_ascii_strcasecmp(text, "true") == 0 || g_ascii_strcasecmp(text, "enabled") == 0 ||
            g_ascii_strcasecmp(text, "active") == 0 || g_ascii_strcasecmp(text, "on") == 0) {
            *result = TRUE;
            return TRUE;
        }
        if (g_ascii_strcasecmp(text, "false") == 0 || g_ascii_strcasecmp(text, "disabled") == 0 ||
            g_ascii_strcasecmp(text, "inactive") == 0 || g_ascii_strcasecmp(text, "off") == 0) {
            *result = FALSE;
            return TRUE;
        }
    }
    return FALSE;
}

static gboolean date_for_keys(json_object *value, const char *const *names, size_t count, gint64 *result) {
    char *text = string_for_keys(value, names, count);
    if (!text) return FALSE;
    GDateTime *date = g_date_time_new_from_iso8601(text, NULL);
    g_free(text);
    if (!date) return FALSE;
    *result = g_date_time_to_unix(date) * 1000 + g_date_time_get_microsecond(date) / 1000;
    g_date_time_unref(date);
    return TRUE;
}

static json_object *batch_entry(json_object *root, guint index) {
    if (json_object_get_type(root) == json_type_array) {
        return index < json_object_array_length(root) ? json_object_array_get_idx(root, index) : NULL;
    }
    if (json_object_get_type(root) != json_type_object) return NULL;
    char key[16];
    g_snprintf(key, sizeof(key), "%u", index);
    json_object *entry = object_member(root, key);
    if (entry) return entry;
    return index == 0 && (object_member(root, "result") || object_member(root, "error")) ? root : NULL;
}

static gboolean contains_ascii(const char *text, const char *needle) {
    if (!text) return FALSE;
    char *lower = g_ascii_strdown(text, -1);
    gboolean found = strstr(lower, needle) != NULL;
    g_free(lower);
    return found;
}

static gboolean entry_payload(
    json_object *root, guint index, gboolean optional, json_object **payload, GError **error) {
    *payload = NULL;
    json_object *entry = batch_entry(root, index);
    if (!entry || json_object_get_type(entry) != json_type_object) return TRUE;
    json_object *entry_error = object_member(entry, "error");
    if (entry_error && json_object_get_type(entry_error) != json_type_null) {
        if (optional) return TRUE;
        static const char *const code_names[] = {"code"};
        static const char *const message_names[] = {"message"};
        char *code = string_for_keys(entry_error, code_names, G_N_ELEMENTS(code_names));
        char *message = string_for_keys(entry_error, message_names, G_N_ELEMENTS(message_names));
        if (contains_ascii(code, "unauthorized") || contains_ascii(code, "forbidden") ||
            contains_ascii(message, "unauthorized") || contains_ascii(message, "forbidden")) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Kilo authentication failed");
        } else if (contains_ascii(code, "not_found") || contains_ascii(message, "not found")) {
            g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Kilo tRPC procedure was not found");
        } else {
            g_set_error(error,
                        G_IO_ERROR,
                        G_IO_ERROR_FAILED,
                        "Kilo tRPC procedure failed%s%s",
                        message ? ": " : "",
                        message ? message : "");
        }
        g_free(code);
        g_free(message);
        return FALSE;
    }
    json_object *result = object_member(entry, "result");
    if (!result || json_object_get_type(result) != json_type_object) return TRUE;
    json_object *data = object_member(result, "data");
    if (data && json_object_get_type(data) == json_type_object) {
        json_object *json_value = object_member(data, "json");
        *payload = json_value && json_object_get_type(json_value) != json_type_null ? json_value : data;
        return TRUE;
    }
    json_object *json_value = object_member(result, "json");
    if (json_value && json_object_get_type(json_value) != json_type_null) *payload = json_value;
    return TRUE;
}

static void resolve_fields(KiloFields *fields) {
    if (!fields->has_total && fields->has_used && fields->has_remaining) {
        fields->total = fields->used + fields->remaining;
        fields->has_total = TRUE;
    }
    if (!fields->has_used && fields->has_total && fields->has_remaining) {
        fields->used = MAX(0.0, fields->total - fields->remaining);
        fields->has_used = TRUE;
    }
    if (!fields->has_remaining && fields->has_total && fields->has_used) {
        fields->remaining = MAX(0.0, fields->total - fields->used);
        fields->has_remaining = TRUE;
    }
}

static void generic_credit_fields(json_object *value, KiloFields *fields) {
    static const char *const used_names[] = {"used", "usedCredits", "consumed", "spent", "creditsUsed"};
    static const char *const total_names[] = {"total", "totalCredits", "creditsTotal", "limit"};
    static const char *const remaining_names[] = {"remaining", "remainingCredits", "creditsRemaining"};
    fields->has_used = number_for_keys(value, used_names, G_N_ELEMENTS(used_names), 1.0, &fields->used);
    fields->has_total = number_for_keys(value, total_names, G_N_ELEMENTS(total_names), 1.0, &fields->total);
    fields->has_remaining =
        number_for_keys(value, remaining_names, G_N_ELEMENTS(remaining_names), 1.0, &fields->remaining);
    resolve_fields(fields);
}

static KiloFields credit_fields(json_object *payload) {
    KiloFields fields = {0};
    if (!payload) return fields;
    static const char *const canonical_names[] = {"creditBlocks"};
    json_object *blocks = find_member_recursive(payload, canonical_names, G_N_ELEMENTS(canonical_names), 0);
    if (blocks && json_object_get_type(blocks) == json_type_array) {
        static const char *const amount_names[] = {"amount_mUsd"};
        static const char *const balance_names[] = {"balance_mUsd"};
        size_t length = json_object_array_length(blocks);
        for (size_t index = 0; index < length; index++) {
            json_object *block = json_object_array_get_idx(blocks, index);
            double amount = 0;
            double balance = 0;
            if (number_for_keys(block, amount_names, G_N_ELEMENTS(amount_names), 1000000.0, &amount)) {
                fields.total += amount;
                fields.has_total = TRUE;
            }
            if (number_for_keys(block, balance_names, G_N_ELEMENTS(balance_names), 1000000.0, &balance)) {
                fields.remaining += balance;
                fields.has_remaining = TRUE;
            }
        }
        if (!fields.has_total && !fields.has_remaining) {
            static const char *const balance_names[] = {"totalBalance_mUsd"};
            double balance = 0;
            if (number_for_keys(payload, balance_names, G_N_ELEMENTS(balance_names), 1000000.0, &balance)) {
                fields.has_total = TRUE;
                fields.total = balance;
                fields.has_remaining = TRUE;
                fields.remaining = balance;
            }
        }
        resolve_fields(&fields);
        return fields;
    }
    static const char *const fallback_names[] = {"blocks"};
    blocks = find_member_recursive(payload, fallback_names, G_N_ELEMENTS(fallback_names), 0);
    if (blocks && json_object_get_type(blocks) == json_type_array && json_object_array_length(blocks) > 0) {
        for (size_t index = 0; index < json_object_array_length(blocks); index++) {
            KiloFields block = {0};
            generic_credit_fields(json_object_array_get_idx(blocks, index), &block);
            if (block.has_used) {
                fields.used += block.used;
                fields.has_used = TRUE;
            }
            if (block.has_total) {
                fields.total += block.total;
                fields.has_total = TRUE;
            }
            if (block.has_remaining) {
                fields.remaining += block.remaining;
                fields.has_remaining = TRUE;
            }
        }
        resolve_fields(&fields);
        return fields;
    }
    generic_credit_fields(payload, &fields);
    return fields;
}

static json_object *subscription_data(json_object *payload, gboolean *present) {
    *present = FALSE;
    if (!payload || json_object_get_type(payload) != json_type_object) return NULL;
    json_object *subscription = object_member(payload, "subscription");
    if (subscription) {
        *present = TRUE;
        return json_object_get_type(subscription) == json_type_object ? subscription : NULL;
    }
    static const char *const shape_names[] = {
        "currentPeriodUsageUsd", "currentPeriodBaseCreditsUsd", "currentPeriodBonusCreditsUsd", "tier"};
    if (find_member_recursive(payload, shape_names, G_N_ELEMENTS(shape_names), 0)) {
        *present = TRUE;
        return payload;
    }
    return NULL;
}

static gboolean money_amount(json_object *payload,
                             const char *const *cents,
                             size_t cents_count,
                             const char *const *micro,
                             size_t micro_count,
                             const char *const *plain,
                             size_t plain_count,
                             double *result) {
    return number_for_keys(payload, cents, cents_count, 100.0, result) ||
           number_for_keys(payload, micro, micro_count, 1000000.0, result) ||
           number_for_keys(payload, plain, plain_count, 1.0, result);
}

static KiloFields pass_fields(json_object *payload) {
    KiloFields fields = {0};
    gboolean has_subscription = FALSE;
    json_object *subscription = subscription_data(payload, &has_subscription);
    if (has_subscription) {
        if (!subscription) return fields;
        static const char *const used_names[] = {"currentPeriodUsageUsd"};
        static const char *const base_names[] = {"currentPeriodBaseCreditsUsd"};
        static const char *const bonus_names[] = {"currentPeriodBonusCreditsUsd"};
        static const char *const reset_names[] = {"nextBillingAt", "nextRenewalAt", "renewsAt", "renewAt"};
        double base = 0;
        fields.has_used = number_for_keys(subscription, used_names, G_N_ELEMENTS(used_names), 1.0, &fields.used);
        gboolean has_base = number_for_keys(subscription, base_names, G_N_ELEMENTS(base_names), 1.0, &base);
        fields.has_bonus =
            number_for_keys(subscription, bonus_names, G_N_ELEMENTS(bonus_names), 1.0, &fields.bonus) &&
            fields.bonus > 0;
        if (has_base) {
            fields.total = base + (fields.has_bonus ? fields.bonus : 0);
            fields.has_total = TRUE;
        }
        fields.has_reset = date_for_keys(subscription, reset_names, G_N_ELEMENTS(reset_names), &fields.reset_ms);
        resolve_fields(&fields);
        return fields;
    }

    static const char *const total_cents[] = {
        "amountCents", "totalCents", "planAmountCents", "monthlyAmountCents", "limitCents", "includedCents", "valueCents"};
    static const char *const total_micro[] = {
        "amount_mUsd", "total_mUsd", "planAmount_mUsd", "limit_mUsd", "included_mUsd", "value_mUsd"};
    static const char *const total_plain[] = {
        "amount", "total", "limit", "included", "value", "creditsTotal", "totalCredits", "planAmount"};
    static const char *const used_cents[] = {
        "usedCents", "spentCents", "consumedCents", "usedAmountCents", "consumedAmountCents"};
    static const char *const used_micro[] = {"used_mUsd", "spent_mUsd", "consumed_mUsd", "usedAmount_mUsd"};
    static const char *const used_plain[] = {
        "used", "spent", "consumed", "usage", "creditsUsed", "usedAmount", "consumedAmount"};
    static const char *const remaining_cents[] = {
        "remainingCents", "remainingAmountCents", "availableCents", "leftCents", "balanceCents"};
    static const char *const remaining_micro[] = {
        "remaining_mUsd", "available_mUsd", "left_mUsd", "balance_mUsd"};
    static const char *const remaining_plain[] = {
        "remaining", "available", "left", "balance", "creditsRemaining", "remainingAmount", "availableAmount"};
    static const char *const bonus_cents[] = {
        "bonusCents", "bonusAmountCents", "includedBonusCents", "bonusRemainingCents"};
    static const char *const bonus_micro[] = {"bonus_mUsd", "bonusAmount_mUsd"};
    static const char *const bonus_plain[] = {"bonus", "bonusAmount", "bonusCredits", "includedBonus"};
    static const char *const reset_names[] = {
        "resetAt", "resetsAt", "nextResetAt", "renewAt", "renewsAt", "nextRenewalAt", "currentPeriodEnd",
        "periodEndsAt", "expiresAt", "expiryAt"};
    fields.has_total = money_amount(payload,
                                    total_cents,
                                    G_N_ELEMENTS(total_cents),
                                    total_micro,
                                    G_N_ELEMENTS(total_micro),
                                    total_plain,
                                    G_N_ELEMENTS(total_plain),
                                    &fields.total);
    fields.has_used = money_amount(payload,
                                   used_cents,
                                   G_N_ELEMENTS(used_cents),
                                   used_micro,
                                   G_N_ELEMENTS(used_micro),
                                   used_plain,
                                   G_N_ELEMENTS(used_plain),
                                   &fields.used);
    fields.has_remaining = money_amount(payload,
                                        remaining_cents,
                                        G_N_ELEMENTS(remaining_cents),
                                        remaining_micro,
                                        G_N_ELEMENTS(remaining_micro),
                                        remaining_plain,
                                        G_N_ELEMENTS(remaining_plain),
                                        &fields.remaining);
    fields.has_bonus = money_amount(payload,
                                    bonus_cents,
                                    G_N_ELEMENTS(bonus_cents),
                                    bonus_micro,
                                    G_N_ELEMENTS(bonus_micro),
                                    bonus_plain,
                                    G_N_ELEMENTS(bonus_plain),
                                    &fields.bonus) &&
                       fields.bonus > 0;
    fields.has_reset = date_for_keys(payload, reset_names, G_N_ELEMENTS(reset_names), &fields.reset_ms);
    resolve_fields(&fields);
    return fields;
}

static char *plan_name(json_object *payload) {
    gboolean has_subscription = FALSE;
    json_object *subscription = subscription_data(payload, &has_subscription);
    if (has_subscription) {
        if (!subscription) return NULL;
        static const char *const tier_names[] = {"tier"};
        char *tier = string_for_keys(subscription, tier_names, G_N_ELEMENTS(tier_names));
        if (!tier) return g_strdup("Kilo Pass");
        char *plan = NULL;
        if (g_str_equal(tier, "tier_19")) plan = g_strdup("Starter");
        else if (g_str_equal(tier, "tier_49")) plan = g_strdup("Pro");
        else if (g_str_equal(tier, "tier_199")) plan = g_strdup("Expert");
        else plan = g_strdup(tier);
        g_free(tier);
        return plan;
    }
    static const char *const direct_names[] = {"planName", "tier", "tierName", "passName", "subscriptionName"};
    char *plan = string_for_keys(payload, direct_names, G_N_ELEMENTS(direct_names));
    if (plan) return plan;
    static const char *const container_names[] = {"plan", "subscription", "pass", "state"};
    json_object *container = find_member_recursive(payload, container_names, G_N_ELEMENTS(container_names), 0);
    static const char *const name_names[] = {"name"};
    plan = string_for_keys(container, name_names, G_N_ELEMENTS(name_names));
    if (plan) return plan;
    return NULL;
}

static char *currency_label(double amount) {
    if (fabs(amount - round(amount)) < 0.0000001) return g_strdup_printf("$%.0f", amount);
    return g_strdup_printf("$%.2f", amount);
}

static char *auto_top_up_label(json_object *credit_payload, json_object *auto_payload) {
    static const char *const enabled_names[] = {"enabled", "isEnabled", "active"};
    static const char *const status_names[] = {"status"};
    static const char *const credit_enabled_names[] = {"autoTopUpEnabled"};
    gboolean enabled = FALSE;
    gboolean has_enabled = bool_for_keys(auto_payload, enabled_names, G_N_ELEMENTS(enabled_names), &enabled) ||
                           bool_for_keys(auto_payload, status_names, G_N_ELEMENTS(status_names), &enabled) ||
                           bool_for_keys(
                               credit_payload, credit_enabled_names, G_N_ELEMENTS(credit_enabled_names), &enabled);
    if (!has_enabled) return NULL;
    if (!enabled) return g_strdup("Auto top-up: off");
    static const char *const method_names[] = {"paymentMethod", "paymentMethodType", "method", "cardBrand"};
    char *method = string_for_keys(auto_payload, method_names, G_N_ELEMENTS(method_names));
    if (!method) {
        static const char *const cents_names[] = {"amountCents"};
        static const char *const plain_names[] = {"amount", "topUpAmount", "amountUsd"};
        double amount = 0;
        static const char *const no_micro[] = {"__never__"};
        if (money_amount(auto_payload,
                         cents_names,
                         G_N_ELEMENTS(cents_names),
                         no_micro,
                         0,
                         plain_names,
                         G_N_ELEMENTS(plain_names),
                         &amount) &&
            amount > 0) {
            method = currency_label(amount);
        }
    }
    char *label = method ? g_strdup_printf("Auto top-up: %s", method) : g_strdup("Auto top-up: enabled");
    g_free(method);
    return label;
}

static char *compact_number(double value) {
    return fabs(value - round(value)) < 0.0000001 ? g_strdup_printf("%.0f", value)
                                                  : g_strdup_printf("%.2f", value);
}

static CodexBarQuotaWindow *quota_window(const char *id, const char *title, const KiloFields *fields) {
    if (!fields->has_total) return NULL;
    double used = fields->has_used ? fields->used : 0;
    CodexBarQuotaWindow *window = codexbar_quota_window_new(id, title);
    window->usage_known = TRUE;
    window->used_percent = fields->total > 0
                               ? codexbar_usage_percent_display(
                                     codexbar_usage_percent_from_ratio(used, fields->total))
                               : 100.0;
    if (fields->has_reset) {
        window->has_resets_at = TRUE;
        window->resets_at_ms = fields->reset_ms;
    }
    return window;
}

CodexBarProvider *codexbar_kilo_parse_usage(
    const char *json, const char *source, const char *organization_id, GError **error) {
    json_object *root = parse_json(json, error);
    if (!root) return NULL;
    if (json_object_get_type(root) != json_type_array && json_object_get_type(root) != json_type_object) {
        json_object_put(root);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Kilo response has an invalid batch shape");
        return NULL;
    }
    json_object *credit_payload = NULL;
    json_object *pass_payload = NULL;
    json_object *auto_payload = NULL;
    if (!entry_payload(root, 0, FALSE, &credit_payload, error) ||
        !entry_payload(root, 1, FALSE, &pass_payload, error) ||
        !entry_payload(root, 2, TRUE, &auto_payload, error)) {
        json_object_put(root);
        return NULL;
    }
    KiloFields credits = credit_fields(credit_payload);
    KiloFields pass = pass_fields(pass_payload);
    char *plan = plan_name(pass_payload);
    char *top_up = auto_top_up_label(credit_payload, auto_payload);
    char *login = plan && top_up ? g_strdup_printf("%s \xC2\xB7 %s", plan, top_up)
                                : g_strdup(plan ? plan : top_up);

    CodexBarProvider *provider = codexbar_provider_new();
    provider->provider = g_strdup("kilo");
    provider->source = g_strdup(source ? source : "api");
    provider->plan = g_strdup(plan);
    provider->identity = g_new0(CodexBarProviderIdentity, 1);
    provider->identity->organization = g_strdup(organization_id);
    provider->identity->login_method = login;
    provider->has_updated_at = TRUE;
    provider->updated_at_ms = g_get_real_time() / 1000;
    provider->explicit_quota_slots = TRUE;

    CodexBarQuotaWindow *primary = quota_window("credits", "Credits", &credits);
    if (primary) {
        char *used = compact_number(credits.has_used ? credits.used : 0);
        char *total = compact_number(credits.total);
        primary->reset_description = g_strdup_printf("%s/%s credits", used, total);
        g_free(used);
        g_free(total);
        codexbar_provider_add_quota_window(provider, primary);
    }
    CodexBarQuotaWindow *secondary = quota_window("pass", "Kilo Pass", &pass);
    if (secondary) {
        double used = pass.has_used ? pass.used : 0;
        double bonus = pass.has_bonus ? pass.bonus : 0;
        double base = MAX(0.0, pass.total - bonus);
        secondary->reset_description = bonus > 0
                                             ? g_strdup_printf("$%.2f / $%.2f (+ $%.2f bonus)", used, base, bonus)
                                             : g_strdup_printf("$%.2f / $%.2f", used, base);
        codexbar_provider_add_quota_window(provider, secondary);
    }
    g_free(plan);
    g_free(top_up);
    json_object_put(root);
    return provider;
}

char *codexbar_kilo_usage_url(void) {
    char *input = g_uri_escape_string(KILO_BATCH_INPUT, NULL, FALSE);
    char *url = g_strdup_printf("%s/%s?batch=1&input=%s", KILO_API_BASE, KILO_PROCEDURES, input);
    g_free(input);
    return url;
}

static char *clean_token(const char *raw) {
    if (!raw) return NULL;
    char *token = g_strdup(raw);
    g_strstrip(token);
    size_t length = strlen(token);
    if (length >= 2 && ((token[0] == '\"' && token[length - 1] == '\"') ||
                        (token[0] == '\'' && token[length - 1] == '\''))) {
        memmove(token, token + 1, length - 2);
        token[length - 2] = '\0';
        g_strstrip(token);
    }
    if (token[0] == '\0') {
        g_free(token);
        return NULL;
    }
    return token;
}

char *codexbar_kilo_parse_auth_token(const char *json, GError **error) {
    json_object *root = parse_json(json, error);
    if (!root) return NULL;
    json_object *kilo = object_member(root, "kilo");
    json_object *access = object_member(kilo, "access");
    char *token = access && json_object_get_type(access) == json_type_string
                      ? clean_token(json_object_get_string(access))
                      : NULL;
    json_object_put(root);
    if (!token) g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Kilo CLI session is invalid");
    return token;
}

char *codexbar_kilo_load_cli_token(const char *home_directory, GError **error) {
    const char *home = home_directory && home_directory[0] != '\0' ? home_directory : g_get_home_dir();
    char *path = g_build_filename(home, ".local", "share", "kilo", "auth.json", NULL);
    GStatBuf status;
    if (g_stat(path, &status) != 0) {
        int saved_errno = errno;
        g_set_error(error,
                    G_IO_ERROR,
                    g_io_error_from_errno(saved_errno),
                    "Kilo CLI session not found at %s. Run `kilo login`.",
                    path);
        g_free(path);
        return NULL;
    }
    if (!S_ISREG(status.st_mode) || status.st_size < 0 || (guint64)status.st_size > KILO_MAXIMUM_AUTH_BYTES) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_INVALID_DATA, "Kilo CLI session is invalid at %s", path);
        g_free(path);
        return NULL;
    }
    char *contents = NULL;
    gsize length = 0;
    if (!g_file_get_contents(path, &contents, &length, error)) {
        g_prefix_error(error, "Kilo CLI session is unreadable at %s: ", path);
        g_free(path);
        return NULL;
    }
    char *token = codexbar_kilo_parse_auth_token(contents, error);
    if (!token) g_prefix_error(error, "Kilo CLI session is invalid at %s: ", path);
    g_free(contents);
    g_free(path);
    return token;
}

static CodexBarProvider *fetch_with_token(
    const char *token, const char *source, const char *organization_id, GError **error) {
    if (strpbrk(token, "\r\n") || (organization_id && strpbrk(organization_id, "\r\n"))) {
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_INVALID_ARGUMENT, "Kilo credentials contain invalid data");
        return NULL;
    }
    char *url = codexbar_kilo_usage_url();
    char *authorization = g_strdup_printf("Bearer %s", token);
    CodexBarHttpRequestHeader headers[3] = {
        {"Accept", "application/json"},
        {"Authorization", authorization},
        {"X-KILOCODE-ORGANIZATIONID", organization_id},
    };
    const CodexBarHttpRequest request = {
        .url = url,
        .method = "GET",
        .headers = headers,
        .header_count = organization_id && organization_id[0] != '\0' ? 3 : 2,
        .timeout_seconds = KILO_TIMEOUT_SECONDS,
        .maximum_response_bytes = KILO_MAXIMUM_RESPONSE_BYTES,
        .protocol_policy = CODEXBAR_HTTP_HTTPS_ONLY,
        .redirect_policy = CODEXBAR_HTTP_REDIRECT_DENY,
    };
    CodexBarHttpResponse *response = codexbar_http_send(&request, error);
    g_free(authorization);
    g_free(url);
    if (!response) return NULL;
    long status = response->status;
    if (status == 401 || status == 403) {
        codexbar_http_response_free(response);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED, "Kilo authentication failed");
        return NULL;
    }
    if (status == 404) {
        codexbar_http_response_free(response);
        g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_NOT_FOUND, "Kilo tRPC endpoint was not found");
        return NULL;
    }
    if (status < 200 || status >= 300) {
        codexbar_http_response_free(response);
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_FAILED, "Kilo request failed with HTTP %ld", status);
        return NULL;
    }
    CodexBarProvider *provider = codexbar_kilo_parse_usage(response->body, source, organization_id, error);
    codexbar_http_response_free(response);
    return provider;
}

static char *api_token(const CodexBarProviderConfig *config) {
    char *token = clean_token(config ? config->api_key : NULL);
    return token ? token : clean_token(g_getenv("KILO_API_KEY"));
}

CodexBarProvider *codexbar_kilo_fetch(
    const CodexBarProviderConfig *config, const char *source, GError **error) {
    const char *selected_source = source ? source : "auto";
    const char *organization_id = config && config->workspace_id && config->workspace_id[0] != '\0'
                                      ? config->workspace_id
                                      : NULL;
    if (g_str_equal(selected_source, "api")) {
        char *token = api_token(config);
        if (!token) {
            g_set_error_literal(error,
                                G_IO_ERROR,
                                G_IO_ERROR_NOT_FOUND,
                                "Kilo API credentials missing. Set KILO_API_KEY or configure api_key.");
            return NULL;
        }
        CodexBarProvider *provider = fetch_with_token(token, "api", organization_id, error);
        g_free(token);
        return provider;
    }
    if (g_str_equal(selected_source, "cli")) {
        char *token = codexbar_kilo_load_cli_token(g_getenv("HOME"), error);
        if (!token) return NULL;
        CodexBarProvider *provider = fetch_with_token(token, "cli", organization_id, error);
        g_free(token);
        return provider;
    }
    if (!g_str_equal(selected_source, "auto")) {
        g_set_error(error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED, "Unsupported Kilo source: %s", selected_source);
        return NULL;
    }

    char *token = api_token(config);
    if (token) {
        GError *api_error = NULL;
        CodexBarProvider *provider = fetch_with_token(token, "api", organization_id, &api_error);
        g_free(token);
        if (provider) return provider;
        if (!g_error_matches(api_error, G_IO_ERROR, G_IO_ERROR_PERMISSION_DENIED)) {
            g_propagate_error(error, api_error);
            return NULL;
        }
        g_clear_error(&api_error);
    }
    token = codexbar_kilo_load_cli_token(g_getenv("HOME"), error);
    if (!token) return NULL;
    CodexBarProvider *provider = fetch_with_token(token, "cli", organization_id, error);
    g_free(token);
    return provider;
}
