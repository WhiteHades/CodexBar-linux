#include "runtime.h"

#include "backend.h"
#include "config.h"
#include "history.h"
#include "hooks.h"
#include "notifications.h"
#include "pace.h"
#include "provider_registry.h"
#include "transitions.h"

#include <json-c/json.h>

typedef struct {
    const CodexBarProviderDescriptor *descriptor;
    CodexBarStatusTransport transport;
    GCancellable *cancellable;
    CodexBarStatusSummary *summary;
    CodexBarServiceStatus *workspace_status;
    GError *error;
    GThread *thread;
} StatusRequest;

struct CodexBarRuntime {
    GMutex lock;
    GHashTable *last_good_statuses;
    CodexBarRuntimeUsageFetcher usage_fetcher;
    gpointer usage_fetcher_data;
    CodexBarStatusTransport status_transport;
    char *config_digest;
    CodexBarTransitionState *transitions;
    CodexBarRuntimeHookDispatcher hook_dispatcher;
    gpointer hook_dispatcher_data;
    CodexBarRuntimeNotificationDispatcher notification_dispatcher;
    gpointer notification_dispatcher_data;
    GHashTable *session_states;
    GHashTable *warning_states;
    GHashTable *failure_states;
    GHashTable *command_code_monthly_states;
    CodexBarProvider *last_good_claude;
    CodexBarHistoryStore *history;
};

typedef struct {
    json_object *hooks;
    CodexBarHookEvent event;
    char *event_name;
    char *provider;
    char *account;
    char *window;
    char *status;
    char *timestamp;
    char *reset_at;
} HookDispatch;

typedef struct {
    CodexBarSessionQuotaState state;
} SessionState;

typedef struct {
    guint streak;
    gboolean had_success;
} FailureState;

typedef struct {
    gboolean has_last_remaining;
    double last_remaining;
    guint64 fired_low;
    guint64 fired_high;
} WarningState;

typedef struct {
    gboolean confirmed_paid_depletion;
    gboolean has_reset;
    gint64 reset_ms;
    gint64 window_minutes;
    char *reset_description;
} CommandCodeMonthlyState;

static CodexBarSnapshot *default_usage_fetcher(GCancellable *cancellable,
                                               gpointer user_data,
                                               GError **error) {
    (void)user_data;
    return codexbar_backend_fetch_with_cancellable(cancellable, error);
}

static CodexBarServiceStatus *status_copy(const CodexBarServiceStatus *status) {
    if (!status) return NULL;
    CodexBarServiceStatus *copy = g_new0(CodexBarServiceStatus, 1);
    *copy = *status;
    copy->description = g_strdup(status->description);
    copy->url = g_strdup(status->url);
    return copy;
}

static void hook_dispatch_free(HookDispatch *dispatch) {
    json_object_put(dispatch->hooks);
    g_free(dispatch->event_name);
    g_free(dispatch->provider);
    g_free(dispatch->account);
    g_free(dispatch->window);
    g_free(dispatch->status);
    g_free(dispatch->timestamp);
    g_free(dispatch->reset_at);
    g_free(dispatch);
}

static void session_state_free(SessionState *state) {
    if (!state) return;
    codexbar_session_quota_state_clear(&state->state);
    g_free(state);
}

static void command_code_monthly_state_free(CommandCodeMonthlyState *state) {
    if (!state) return;
    g_free(state->reset_description);
    g_free(state);
}

static gpointer hook_worker(gpointer data) {
    HookDispatch *dispatch = data;
    json_object *results = codexbar_hooks_dispatch(dispatch->hooks, &dispatch->event, NULL, NULL);
    json_object_put(results);
    hook_dispatch_free(dispatch);
    return NULL;
}

static void default_hook_dispatcher(json_object *hooks,
                                    const CodexBarHookEvent *event,
                                    gpointer user_data) {
    (void)user_data;
    HookDispatch *dispatch = g_new0(HookDispatch, 1);
    dispatch->hooks = json_object_get(hooks);
    dispatch->event_name = g_strdup(event->event);
    dispatch->provider = g_strdup(event->provider);
    dispatch->account = g_strdup(event->account);
    dispatch->window = g_strdup(event->window);
    dispatch->status = g_strdup(event->status);
    dispatch->timestamp = g_strdup(event->timestamp);
    dispatch->reset_at = g_strdup(event->reset_at);
    dispatch->event = *event;
    dispatch->event.event = dispatch->event_name;
    dispatch->event.provider = dispatch->provider;
    dispatch->event.account = dispatch->account;
    dispatch->event.window = dispatch->window;
    dispatch->event.status = dispatch->status;
    dispatch->event.timestamp = dispatch->timestamp;
    dispatch->event.reset_at = dispatch->reset_at;
    GThread *thread = g_thread_new("provider-hook", hook_worker, dispatch);
    g_thread_unref(thread);
}

static void default_notification_dispatcher(const CodexBarHookEvent *event, gpointer user_data) {
    (void)user_data;
    codexbar_notification_send(event);
}

CodexBarRuntime *codexbar_runtime_new_with_transports(CodexBarRuntimeUsageFetcher usage_fetcher,
                                                      gpointer usage_fetcher_data,
                                                      CodexBarStatusTransport status_transport) {
    CodexBarRuntime *runtime = g_new0(CodexBarRuntime, 1);
    g_mutex_init(&runtime->lock);
    runtime->last_good_statuses = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)codexbar_service_status_free);
    runtime->usage_fetcher = usage_fetcher ? usage_fetcher : default_usage_fetcher;
    runtime->usage_fetcher_data = usage_fetcher_data;
    runtime->status_transport = status_transport;
    runtime->transitions = codexbar_transition_state_new();
    runtime->hook_dispatcher = default_hook_dispatcher;
    runtime->notification_dispatcher = default_notification_dispatcher;
    runtime->session_states = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)session_state_free);
    runtime->warning_states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    runtime->failure_states = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    runtime->command_code_monthly_states = g_hash_table_new_full(
        g_str_hash, g_str_equal, g_free, (GDestroyNotify)command_code_monthly_state_free);
    runtime->history = codexbar_history_store_new(NULL);
    return runtime;
}

void codexbar_runtime_set_notification_dispatcher(CodexBarRuntime *runtime,
                                                  CodexBarRuntimeNotificationDispatcher dispatcher,
                                                  gpointer user_data) {
    g_return_if_fail(runtime != NULL);
    runtime->notification_dispatcher = dispatcher ? dispatcher : default_notification_dispatcher;
    runtime->notification_dispatcher_data = user_data;
}

CodexBarRuntime *codexbar_runtime_new(void) {
    return codexbar_runtime_new_with_transports(NULL, NULL, NULL);
}

void codexbar_runtime_free(CodexBarRuntime *runtime) {
    if (!runtime) return;
    g_hash_table_unref(runtime->last_good_statuses);
    codexbar_transition_state_free(runtime->transitions);
    g_hash_table_unref(runtime->session_states);
    g_hash_table_unref(runtime->warning_states);
    g_hash_table_unref(runtime->failure_states);
    g_hash_table_unref(runtime->command_code_monthly_states);
    codexbar_provider_free(runtime->last_good_claude);
    codexbar_history_store_free(runtime->history);
    g_free(runtime->config_digest);
    g_mutex_clear(&runtime->lock);
    g_free(runtime);
}

void codexbar_runtime_set_hook_dispatcher(CodexBarRuntime *runtime,
                                          CodexBarRuntimeHookDispatcher dispatcher,
                                          gpointer user_data) {
    g_return_if_fail(runtime != NULL);
    runtime->hook_dispatcher = dispatcher ? dispatcher : default_hook_dispatcher;
    runtime->hook_dispatcher_data = user_data;
}

static gboolean environment_flag(const char *name) {
    const char *value = g_getenv(name);
    return value && (g_str_equal(value, "1") || g_ascii_strcasecmp(value, "true") == 0 ||
                     g_ascii_strcasecmp(value, "yes") == 0);
}

static gpointer status_worker(gpointer data) {
    StatusRequest *request = data;
    switch (codexbar_provider_status_source(request->descriptor)) {
    case CODEXBAR_PROVIDER_STATUS_STATUSPAGE:
        request->summary = codexbar_statuspage_fetch_summary(
            codexbar_provider_status_source_value(request->descriptor),
            request->transport,
            request->cancellable,
            &request->error);
        break;
    case CODEXBAR_PROVIDER_STATUS_GOOGLE_WORKSPACE:
        request->workspace_status = codexbar_workspace_fetch_status(
            codexbar_provider_status_source_value(request->descriptor),
            request->transport,
            request->cancellable,
            &request->error);
        break;
    case CODEXBAR_PROVIDER_STATUS_NONE:
        break;
    }
    return NULL;
}

static void status_request_free(StatusRequest *request) {
    if (!request) return;
    if (request->thread) g_thread_join(request->thread);
    codexbar_status_summary_free(request->summary);
    codexbar_service_status_free(request->workspace_status);
    g_clear_error(&request->error);
    g_clear_object(&request->cancellable);
    g_free(request);
}

static GPtrArray *start_status_requests(CodexBarRuntime *runtime,
                                       CodexBarConfig *config,
                                       GCancellable *cancellable) {
    GPtrArray *requests = g_ptr_array_new_with_free_func((GDestroyNotify)status_request_free);
    for (guint index = 0; index < config->providers->len; index++) {
        CodexBarProviderConfig *provider = g_ptr_array_index(config->providers, index);
        if (!provider->enabled) continue;
        const CodexBarProviderDescriptor *descriptor = codexbar_provider_registry_find(provider->id);
        if (!codexbar_provider_status_is_pollable(descriptor)) continue;
        StatusRequest *request = g_new0(StatusRequest, 1);
        request->descriptor = descriptor;
        request->transport = runtime->status_transport;
        request->cancellable = cancellable ? g_object_ref(cancellable) : NULL;
        request->thread = g_thread_new("provider-status", status_worker, request);
        g_ptr_array_add(requests, request);
    }
    return requests;
}

static void reconcile_config_revision(CodexBarRuntime *runtime, const char *digest) {
    g_mutex_lock(&runtime->lock);
    if (runtime->config_digest && g_strcmp0(runtime->config_digest, digest) != 0) {
        g_hash_table_remove_all(runtime->last_good_statuses);
        g_hash_table_remove_all(runtime->session_states);
        g_hash_table_remove_all(runtime->warning_states);
        g_hash_table_remove_all(runtime->failure_states);
        g_hash_table_remove_all(runtime->command_code_monthly_states);
    }
    g_free(runtime->config_digest);
    runtime->config_digest = g_strdup(digest);
    codexbar_transition_state_set_config_revision(runtime->transitions, digest);
    g_mutex_unlock(&runtime->lock);
}

static CodexBarServiceStatus *request_status(StatusRequest *request) {
    if (request->thread) {
        g_thread_join(request->thread);
        request->thread = NULL;
    }
    return request->summary ? request->summary->status : request->workspace_status;
}

static CodexBarServiceStatus *published_status(CodexBarRuntime *runtime, StatusRequest *request) {
    CodexBarServiceStatus *fresh = request_status(request);
    g_mutex_lock(&runtime->lock);
    if (fresh) {
        CodexBarServiceStatus *cached = status_copy(fresh);
        g_free(cached->url);
        cached->url = g_strdup(request->descriptor->status_url);
        g_hash_table_replace(runtime->last_good_statuses, g_strdup(request->descriptor->id), cached);
    }
    CodexBarServiceStatus *published = status_copy(
        g_hash_table_lookup(runtime->last_good_statuses, request->descriptor->id));
    g_mutex_unlock(&runtime->lock);
    if (published || !request->error) return published;

    published = g_new0(CodexBarServiceStatus, 1);
    published->indicator = CODEXBAR_STATUS_UNKNOWN;
    published->description = g_strdup(request->error->message);
    published->url = g_strdup(request->descriptor->status_url);
    return published;
}

static const char *status_id(CodexBarServiceStatusIndicator indicator) {
    switch (indicator) {
    case CODEXBAR_STATUS_NONE: return "none";
    case CODEXBAR_STATUS_MINOR: return "minor";
    case CODEXBAR_STATUS_MAJOR: return "major";
    case CODEXBAR_STATUS_CRITICAL: return "critical";
    case CODEXBAR_STATUS_MAINTENANCE: return "maintenance";
    case CODEXBAR_STATUS_UNKNOWN: return "unknown";
    }
    return "unknown";
}

static void dispatch_status_hook(CodexBarRuntime *runtime,
                                 json_object *hooks,
                                 StatusRequest *request,
                                 const CodexBarServiceStatus *fresh) {
    if (!fresh || !hooks || !codexbar_hooks_enabled(hooks)) return;
    g_mutex_lock(&runtime->lock);
    CodexBarProviderTransition transition = codexbar_transition_provider_status(
        runtime->transitions, request->descriptor->id, fresh->indicator);
    gboolean allowed = transition != CODEXBAR_PROVIDER_TRANSITION_UNAVAILABLE ||
                       codexbar_transition_rate_limit_allow(runtime->transitions,
                                                            "provider_unavailable",
                                                            request->descriptor->id,
                                                            NULL,
                                                            NULL,
                                                            g_get_real_time() / 1000);
    g_mutex_unlock(&runtime->lock);
    if (transition == CODEXBAR_PROVIDER_TRANSITION_NONE || !allowed) return;
    CodexBarHookEvent event = {
        .event = transition == CODEXBAR_PROVIDER_TRANSITION_UNAVAILABLE ? "provider_unavailable"
                                                                        : "provider_recovered",
        .provider = request->descriptor->id,
        .status = status_id(fresh->indicator),
    };
    runtime->hook_dispatcher(hooks, &event, runtime->hook_dispatcher_data);
}

static json_object *copy_hooks(const CodexBarConfig *config) {
    json_object *hooks = NULL;
    if (!config->raw || !json_object_object_get_ex(config->raw, "hooks", &hooks) ||
        !json_object_is_type(hooks, json_type_object)) {
        return NULL;
    }
    return json_tokener_parse(json_object_to_json_string_ext(hooks, JSON_C_TO_STRING_PLAIN));
}

static json_object *provider_warning_lane(const CodexBarProviderConfig *provider, const char *lane) {
    json_object *warnings = NULL;
    json_object *configured = NULL;
    return provider->raw && json_object_object_get_ex(provider->raw, "quotaWarnings", &warnings) &&
                   json_object_is_type(warnings, json_type_object) &&
                   json_object_object_get_ex(warnings, lane, &configured) &&
                   json_object_is_type(configured, json_type_object)
               ? configured
               : NULL;
}

static json_object *resolved_warning_thresholds(json_object *override,
                                                const int *global,
                                                guint global_count) {
    json_object *configured = NULL;
    gboolean has_override = override && json_object_object_get_ex(override, "thresholds", &configured) &&
                            json_object_is_type(configured, json_type_array);
    gboolean seen[100] = {0};
    int thresholds[100];
    guint count = 0;
    if (has_override) {
        for (size_t index = 0; index < json_object_array_length(configured); index++) {
            json_object *entry = json_object_array_get_idx(configured, index);
            if (!json_object_is_type(entry, json_type_int)) continue;
            int threshold = CLAMP(json_object_get_int(entry), 0, 99);
            if (!seen[threshold]) thresholds[count++] = threshold;
            seen[threshold] = TRUE;
        }
        if (count == 0) {
            thresholds[0] = 50;
            thresholds[1] = 20;
            count = 2;
        }
    } else {
        memcpy(thresholds, global, global_count * sizeof(global[0]));
        count = global_count;
    }
    for (guint left = 0; left < count; left++) {
        for (guint right = left + 1; right < count; right++) {
            if (thresholds[right] > thresholds[left]) {
                int temporary = thresholds[left];
                thresholds[left] = thresholds[right];
                thresholds[right] = temporary;
            }
        }
    }
    json_object *array = json_object_new_array_ext((int)count);
    for (guint index = 0; index < count; index++) {
        json_object_array_add(array, json_object_new_int(thresholds[index]));
    }
    return array;
}

static json_object *resolved_warning_lane(const CodexBarProviderConfig *provider,
                                          const char *lane,
                                          gboolean global_enabled,
                                          const int *global_thresholds,
                                          guint global_threshold_count) {
    json_object *override = provider_warning_lane(provider, lane);
    json_object *enabled_value = NULL;
    json_object *thresholds_value = NULL;
    gboolean has_threshold_override = override &&
        json_object_object_get_ex(override, "thresholds", &thresholds_value) &&
        json_object_is_type(thresholds_value, json_type_array);
    gboolean enabled = override && json_object_object_get_ex(override, "enabled", &enabled_value) &&
                               json_object_is_type(enabled_value, json_type_boolean)
                           ? json_object_get_boolean(enabled_value)
                           : has_threshold_override ? TRUE : global_enabled;
    if (!enabled) return NULL;
    json_object *resolved = json_object_new_object();
    json_object_object_add(resolved, "enabled", json_object_new_boolean(TRUE));
    json_object_object_add(resolved,
                           "thresholds",
                           resolved_warning_thresholds(
                               override, global_thresholds, global_threshold_count));
    return resolved;
}

static json_object *copy_quota_warnings(const CodexBarConfig *config) {
    json_object *warnings = json_object_new_object();
    if (!config->quota_warning_notifications_enabled) return warnings;
    for (guint index = 0; index < config->providers->len; index++) {
        CodexBarProviderConfig *provider = g_ptr_array_index(config->providers, index);
        json_object *provider_warnings = json_object_new_object();
        json_object *session = resolved_warning_lane(provider,
                                                     "session",
                                                     config->quota_warning_session_enabled,
                                                     config->quota_warning_session_thresholds,
                                                     config->quota_warning_session_threshold_count);
        json_object *weekly = resolved_warning_lane(provider,
                                                    "weekly",
                                                    config->quota_warning_weekly_enabled,
                                                    config->quota_warning_weekly_thresholds,
                                                    config->quota_warning_weekly_threshold_count);
        if (session) json_object_object_add(provider_warnings, "session", session);
        if (weekly) json_object_object_add(provider_warnings, "weekly", weekly);
        if (json_object_object_length(provider_warnings) > 0) {
            json_object_object_add(warnings, provider->id, provider_warnings);
        } else {
            json_object_put(provider_warnings);
        }
    }
    return warnings;
}

static json_object *warning_lane(json_object *warnings, const char *provider, const char *lane) {
    json_object *provider_warnings = NULL;
    json_object *configured = NULL;
    if (!warnings || !json_object_object_get_ex(warnings, provider, &provider_warnings) ||
        !json_object_is_type(provider_warnings, json_type_object) ||
        !json_object_object_get_ex(provider_warnings, lane, &configured) ||
        !json_object_is_type(configured, json_type_object)) {
        return NULL;
    }
    json_object *enabled = NULL;
    if (json_object_object_get_ex(configured, "enabled", &enabled) &&
        json_object_is_type(enabled, json_type_boolean) && !json_object_get_boolean(enabled)) {
        return NULL;
    }
    return configured;
}

static gboolean rule_matches_event_provider(json_object *rule,
                                            const char *event,
                                            const char *provider) {
    if (!rule || !json_object_is_type(rule, json_type_object)) return FALSE;
    json_object *enabled = NULL;
    if (json_object_object_get_ex(rule, "enabled", &enabled) &&
        (!json_object_is_type(enabled, json_type_boolean) || !json_object_get_boolean(enabled))) {
        return FALSE;
    }
    json_object *rule_event = NULL;
    if (!json_object_object_get_ex(rule, "event", &rule_event) ||
        !json_object_is_type(rule_event, json_type_string) ||
        !g_str_equal(json_object_get_string(rule_event), event)) {
        return FALSE;
    }
    json_object *rule_provider = NULL;
    if (!json_object_object_get_ex(rule, "provider", &rule_provider) ||
        json_object_is_type(rule_provider, json_type_null)) {
        return TRUE;
    }
    return json_object_is_type(rule_provider, json_type_string) &&
           g_str_equal(json_object_get_string(rule_provider), provider);
}

static gboolean has_hook_rule(json_object *hooks, const char *event, const char *provider) {
    json_object *events = codexbar_hook_events(hooks);
    if (!codexbar_hooks_enabled(hooks) || !events ||
        json_object_array_length(events) > CODEXBAR_MAX_HOOK_RULES) {
        return FALSE;
    }
    for (size_t index = 0; index < json_object_array_length(events); index++) {
        if (rule_matches_event_provider(json_object_array_get_idx(events, index), event, provider)) return TRUE;
    }
    return FALSE;
}

static char *iso8601_milliseconds(gint64 milliseconds) {
    GDateTime *date = g_date_time_new_from_unix_utc(milliseconds / 1000);
    if (!date) return NULL;
    char *text = g_date_time_format(date, "%Y-%m-%dT%H:%M:%SZ");
    g_date_time_unref(date);
    return text;
}

static const char *provider_account_key(const CodexBarProvider *provider) {
    if (provider->identity && provider->identity->account_id && provider->identity->account_id[0] != '\0') {
        return provider->identity->account_id;
    }
    return provider->account;
}

static char *transition_lane_key(const CodexBarProvider *provider) {
    const char *account = provider_account_key(provider);
    return g_strjoin("\x1f", provider->provider, account ? account : "", NULL);
}

static gboolean usage_extension_boolean(const CodexBarProvider *provider, const char *key) {
    json_object *value = NULL;
    return provider->usage_extensions &&
           json_object_object_get_ex(provider->usage_extensions, key, &value) &&
           json_object_is_type(value, json_type_boolean) && json_object_get_boolean(value);
}

static CodexBarQuotaWindow *quota_window_by_id(CodexBarProvider *provider, const char *id) {
    for (guint index = 0; index < provider->quota_windows->len; index++) {
        CodexBarQuotaWindow *window = g_ptr_array_index(provider->quota_windows, index);
        if (g_str_equal(window->id, id)) return window;
    }
    return NULL;
}

static gboolean crof_credits_only(const CodexBarProvider *provider) {
    if (!g_str_equal(provider->provider, "crof") || provider->quota_windows->len != 1) return FALSE;
    const CodexBarQuotaWindow *window = g_ptr_array_index(provider->quota_windows, 0);
    return !window->has_window_minutes &&
           (g_str_equal(window->id, "balance") || g_ascii_strcasecmp(window->title, "balance") == 0);
}

static void stabilize_command_code_monthly(CodexBarRuntime *runtime, CodexBarProvider *provider) {
    if (!g_str_equal(provider->provider, "commandcode") || provider->error) return;
    const char *account = provider_account_key(provider);
    char *key = g_strjoin("\x1f",
                          provider->provider,
                          account ? account : "",
                          provider->source ? provider->source : "",
                          NULL);
    char *scoped_key = g_strjoin("\x1f",
                                 key,
                                 provider->runtime_scope ? provider->runtime_scope : "",
                                 NULL);
    g_free(key);
    key = scoped_key;
    CodexBarQuotaWindow *monthly = quota_window_by_id(provider, "tertiary");
    gboolean unavailable = usage_extension_boolean(
        provider, "commandCodeSubscriptionEnrichmentUnavailable");
    gboolean has_plan = usage_extension_boolean(provider, "commandCodeHasSubscriptionPlan");
    gboolean depleted = usage_extension_boolean(provider, "commandCodeMonthlyGrantDepleted");
    g_mutex_lock(&runtime->lock);
    CommandCodeMonthlyState *state = g_hash_table_lookup(runtime->command_code_monthly_states, key);
    if (!state) {
        state = g_new0(CommandCodeMonthlyState, 1);
        g_hash_table_insert(runtime->command_code_monthly_states, g_strdup(key), state);
    }
    if (unavailable && depleted && state->confirmed_paid_depletion) {
        if (!monthly) {
            monthly = codexbar_quota_window_new("tertiary", "Monthly credits");
            monthly->usage_known = TRUE;
            codexbar_provider_add_quota_window(provider, monthly);
        }
        monthly->used_percent = 100;
        monthly->has_resets_at = state->has_reset;
        monthly->resets_at_ms = state->reset_ms;
        monthly->has_window_minutes = state->window_minutes > 0;
        monthly->window_minutes = state->window_minutes;
        g_free(monthly->reset_description);
        monthly->reset_description = g_strdup(state->reset_description);
    } else if (!unavailable) {
        state->confirmed_paid_depletion = has_plan && depleted && monthly && monthly->used_percent >= 100;
        state->has_reset = state->confirmed_paid_depletion && monthly->has_resets_at;
        state->reset_ms = state->has_reset ? monthly->resets_at_ms : 0;
        state->window_minutes = state->confirmed_paid_depletion && monthly->has_window_minutes
                                    ? monthly->window_minutes
                                    : 0;
        g_free(state->reset_description);
        state->reset_description = state->confirmed_paid_depletion
                                       ? g_strdup(monthly->reset_description)
                                       : NULL;
    }
    g_mutex_unlock(&runtime->lock);
    g_free(key);
}

static CodexBarQuotaWindow *session_window(const CodexBarProvider *provider) {
    if (!provider->quota_windows || provider->quota_windows->len == 0 ||
        g_str_equal(provider->provider, "mimo") || g_str_equal(provider->provider, "qoder")) {
        return NULL;
    }
    if (crof_credits_only(provider)) return NULL;
    if (g_str_equal(provider->provider, "antigravity")) {
        CodexBarQuotaWindow *selected = NULL;
        for (guint index = 0; index < provider->quota_windows->len; index++) {
            CodexBarQuotaWindow *window = g_ptr_array_index(provider->quota_windows, index);
            if (!window->usage_known || !window->has_window_minutes || window->window_minutes != 300) continue;
            if (!selected || window->used_percent > selected->used_percent) selected = window;
        }
        return selected;
    }
    if (g_str_equal(provider->provider, "zai") && provider->quota_windows->len > 2) {
        return g_ptr_array_index(provider->quota_windows, 2);
    }
    CodexBarQuotaWindow *primary = g_ptr_array_index(provider->quota_windows, 0);
    if (primary->usage_known && (!primary->has_window_minutes || primary->window_minutes <= 360)) return primary;
    if (g_str_equal(provider->provider, "copilot") && provider->quota_windows->len > 1) {
        CodexBarQuotaWindow *secondary = g_ptr_array_index(provider->quota_windows, 1);
        return secondary->usage_known ? secondary : NULL;
    }
    return NULL;
}

static void dispatch_session_transition(CodexBarRuntime *runtime,
                                        json_object *hooks,
                                        json_object *warnings,
                                        const CodexBarProvider *provider) {
    CodexBarQuotaWindow *window = session_window(provider);
    if (!window) return;
    gboolean reached_rule = has_hook_rule(hooks, "quota_reached", provider->provider);
    gboolean reset_rule = has_hook_rule(hooks, "quota_reset", provider->provider);
    gboolean notifications_enabled = warning_lane(warnings, provider->provider, "session") != NULL;
    char *key = transition_lane_key(provider);
    gint64 observed_at = provider->has_updated_at ? provider->updated_at_ms : g_get_real_time() / 1000;
    const char *owner = provider_account_key(provider);
    CodexBarSessionQuotaObservation observation = {
        .provider = provider->provider,
        .remaining = 100 - CLAMP(window->used_percent, 0, 100),
        .source = provider->source ? provider->source : "auto",
        .observed_at_milliseconds = observed_at,
        .evaluation_time_milliseconds = g_get_real_time() / 1000,
        .codex_owner_key = owner,
        .has_reset_boundary = window->has_resets_at,
        .reset_boundary_milliseconds = window->resets_at_ms,
    };
    SessionState *previous = NULL;
    SessionState *next = g_new0(SessionState, 1);
    g_mutex_lock(&runtime->lock);
    previous = g_hash_table_lookup(runtime->session_states, key);
    CodexBarSessionQuotaOutcome outcome = codexbar_session_quota_evaluate(
        previous ? &previous->state : NULL,
        &observation,
        reached_rule || reset_rule || notifications_enabled,
        FALSE,
        &next->state);
    g_hash_table_replace(runtime->session_states, key, next);
    g_mutex_unlock(&runtime->lock);

    const char *event_name = outcome == CODEXBAR_SESSION_QUOTA_DEPLETED &&
                                     (reached_rule || notifications_enabled)
                                 ? "quota_reached"
                             : outcome == CODEXBAR_SESSION_QUOTA_RESTORED &&
                                       (reset_rule || notifications_enabled)
                                 ? "quota_reset"
                                 : NULL;
    if (!event_name) return;
    char *reset_at = window->has_resets_at ? iso8601_milliseconds(window->resets_at_ms) : NULL;
    CodexBarHookEvent event = {
        .event = event_name,
        .provider = provider->provider,
        .account = provider->account,
        .window = "session",
        .has_usage_percent = TRUE,
        .usage_percent = CLAMP(window->used_percent / 100, 0, 1),
        .reset_at = reset_at,
    };
    if ((g_str_equal(event_name, "quota_reached") && reached_rule) ||
        (g_str_equal(event_name, "quota_reset") && reset_rule)) {
        runtime->hook_dispatcher(hooks, &event, runtime->hook_dispatcher_data);
    }
    if (notifications_enabled) {
        runtime->notification_dispatcher(&event, runtime->notification_dispatcher_data);
    }
    g_free(reset_at);
}

static gboolean threshold_crossed(json_object *rule, double previous, double current) {
    json_object *threshold = NULL;
    if (json_object_object_get_ex(rule, "threshold", &threshold) &&
        !json_object_is_type(threshold, json_type_null)) {
        if (!json_object_is_type(threshold, json_type_double) &&
            !json_object_is_type(threshold, json_type_int)) {
            return FALSE;
        }
        double value = json_object_get_double(threshold);
        return value > 0 && value <= 1 && previous < value && current >= value;
    }
    const double defaults[] = {0.5, 0.8};
    for (guint index = 0; index < G_N_ELEMENTS(defaults); index++) {
        if (previous < defaults[index] && current >= defaults[index]) return TRUE;
    }
    return FALSE;
}

static json_object *crossed_quota_low_hooks(json_object *hooks,
                                            const char *provider,
                                            double previous,
                                            double current) {
    json_object *events = codexbar_hook_events(hooks);
    if (!codexbar_hooks_enabled(hooks) || !events ||
        json_object_array_length(events) > CODEXBAR_MAX_HOOK_RULES) {
        return NULL;
    }
    json_object *crossed = json_object_new_array();
    for (size_t index = 0; index < json_object_array_length(events); index++) {
        json_object *rule = json_object_array_get_idx(events, index);
        if (rule_matches_event_provider(rule, "quota_low", provider) &&
            threshold_crossed(rule, previous, current)) {
            json_object_array_add(crossed, json_object_get(rule));
        }
    }
    if (json_object_array_length(crossed) == 0) {
        json_object_put(crossed);
        return NULL;
    }
    json_object *filtered = json_object_new_object();
    json_object_object_add(filtered, "enabled", json_object_new_boolean(TRUE));
    json_object_object_add(filtered, "events", crossed);
    return filtered;
}

static gboolean warning_fired(const WarningState *state, int threshold) {
    return threshold < 64 ? (state->fired_low & ((guint64)1 << threshold)) != 0
                          : (state->fired_high & ((guint64)1 << (threshold - 64))) != 0;
}

static void warning_set_fired(WarningState *state, int threshold, gboolean fired) {
    guint64 bit = (guint64)1 << (threshold < 64 ? threshold : threshold - 64);
    guint64 *bits = threshold < 64 ? &state->fired_low : &state->fired_high;
    if (fired) *bits |= bit;
    else *bits &= ~bit;
}

static int notification_crossed_threshold(CodexBarRuntime *runtime,
                                          json_object *lane,
                                          const CodexBarProvider *provider,
                                          const char *lane_name,
                                          const char *window_id,
                                          double current_used) {
    json_object *thresholds = NULL;
    if (!json_object_object_get_ex(lane, "thresholds", &thresholds) ||
        !json_object_is_type(thresholds, json_type_array)) {
        return 0;
    }
    char *key = g_strjoin("\x1f",
                          provider->provider,
                          provider_account_key(provider) ? provider_account_key(provider) : "",
                          lane_name,
                          window_id ? window_id : "",
                          NULL);
    double remaining = 100 - current_used * 100;
    int crossed = 0;
    g_mutex_lock(&runtime->lock);
    WarningState *state = g_hash_table_lookup(runtime->warning_states, key);
    if (!state) {
        state = g_new0(WarningState, 1);
        g_hash_table_insert(runtime->warning_states, g_strdup(key), state);
    }
    for (size_t index = 0; index < json_object_array_length(thresholds); index++) {
        json_object *entry = json_object_array_get_idx(thresholds, index);
        if (!json_object_is_type(entry, json_type_int)) continue;
        int threshold = CLAMP(json_object_get_int(entry), 0, 99);
        if (threshold == 0) continue;
        if (remaining > threshold) warning_set_fired(state, threshold, FALSE);
        gboolean crossed_now = remaining <= threshold && !warning_fired(state, threshold) &&
                               (!state->has_last_remaining || state->last_remaining > threshold);
        if (crossed_now && (crossed == 0 || threshold < crossed)) crossed = threshold;
    }
    if (crossed > 0) {
        for (size_t index = 0; index < json_object_array_length(thresholds); index++) {
            json_object *entry = json_object_array_get_idx(thresholds, index);
            if (!json_object_is_type(entry, json_type_int)) continue;
            int threshold = CLAMP(json_object_get_int(entry), 0, 99);
            if (threshold >= crossed && threshold > 0) warning_set_fired(state, threshold, TRUE);
        }
    }
    state->has_last_remaining = TRUE;
    state->last_remaining = remaining;
    g_mutex_unlock(&runtime->lock);
    g_free(key);
    return crossed;
}

static void dispatch_quota_low_transitions(CodexBarRuntime *runtime,
                                           json_object *hooks,
                                           json_object *warnings,
                                           const CodexBarProvider *provider) {
    gboolean hook_enabled = has_hook_rule(hooks, "quota_low", provider->provider);
    if (crof_credits_only(provider)) return;
    for (guint index = 0; index < provider->quota_windows->len; index++) {
        CodexBarQuotaWindow *window = g_ptr_array_index(provider->quota_windows, index);
        if (!window->usage_known) continue;
        const char *lane = window->has_window_minutes && window->window_minutes > 360 ? "weekly" : "session";
        json_object *notification_lane = warning_lane(warnings, provider->provider, lane);
        if (!hook_enabled && !notification_lane) continue;
        double current = CLAMP(window->used_percent / 100, 0, 1);
        double previous = 0;
        g_mutex_lock(&runtime->lock);
        gboolean had_previous = codexbar_transition_quota_observe(runtime->transitions,
                                                                  provider->provider,
                                                                  provider_account_key(provider),
                                                                  lane,
                                                                  window->id,
                                                                  current,
                                                                  &previous);
        g_mutex_unlock(&runtime->lock);
        json_object *filtered = hook_enabled && had_previous
                                    ? crossed_quota_low_hooks(hooks, provider->provider, previous, current)
                                    : NULL;
        int warning_threshold = notification_lane
                                    ? notification_crossed_threshold(runtime,
                                                                       notification_lane,
                                                                       provider,
                                                                       lane,
                                                                       window->id,
                                                                       current)
                                    : 0;
        gboolean notify = warning_threshold > 0;
        if (!filtered && !notify) continue;
        char *reset_at = window->has_resets_at ? iso8601_milliseconds(window->resets_at_ms) : NULL;
        CodexBarHookEvent event = {
            .event = "quota_low",
            .provider = provider->provider,
            .account = provider->account,
            .window = window->title ? window->title : lane,
            .has_usage_percent = TRUE,
            .usage_percent = current,
            .has_warning_threshold = notify,
            .warning_threshold = warning_threshold,
            .reset_at = reset_at,
        };
        if (filtered) runtime->hook_dispatcher(filtered, &event, runtime->hook_dispatcher_data);
        if (notify) runtime->notification_dispatcher(&event, runtime->notification_dispatcher_data);
        g_free(reset_at);
        if (filtered) json_object_put(filtered);
    }
}

static const char *provider_failure_status(const CodexBarProvider *provider) {
    if (g_strcmp0(provider->error_kind, "timeout") == 0) return "timeout";
    return "error";
}

static void dispatch_refresh_failure(CodexBarRuntime *runtime,
                                     json_object *hooks,
                                     const CodexBarProvider *provider) {
    char *key = transition_lane_key(provider);
    gboolean should_dispatch = FALSE;
    g_mutex_lock(&runtime->lock);
    FailureState *state = g_hash_table_lookup(runtime->failure_states, key);
    if (!state) {
        state = g_new0(FailureState, 1);
        g_hash_table_insert(runtime->failure_states, g_strdup(key), state);
    }
    if (!provider->error) {
        state->streak = 0;
        state->had_success = TRUE;
    } else {
        state->streak++;
        gboolean surfaced = !state->had_success || state->streak > 1;
        should_dispatch = surfaced && has_hook_rule(hooks, "refresh_failed", provider->provider) &&
                          codexbar_transition_rate_limit_allow(runtime->transitions,
                                                               "refresh_failed",
                                                               provider->provider,
                                                               provider->account,
                                                               NULL,
                                                               g_get_real_time() / 1000);
    }
    g_mutex_unlock(&runtime->lock);
    g_free(key);
    if (!should_dispatch) return;
    CodexBarHookEvent event = {
        .event = "refresh_failed",
        .provider = provider->provider,
        .account = provider->account,
        .status = provider_failure_status(provider),
    };
    runtime->hook_dispatcher(hooks, &event, runtime->hook_dispatcher_data);
}

static void dispatch_snapshot_hooks(CodexBarRuntime *runtime,
                                    json_object *hooks,
                                    json_object *warnings,
                                    const CodexBarSnapshot *snapshot) {
    if ((!hooks || !codexbar_hooks_enabled(hooks)) &&
        (!warnings || json_object_object_length(warnings) == 0)) {
        return;
    }
    for (guint index = 0; index < snapshot->providers->len; index++) {
        const CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        dispatch_refresh_failure(runtime, hooks, provider);
        if (provider->error) continue;
        dispatch_quota_low_transitions(runtime, hooks, warnings, provider);
        dispatch_session_transition(runtime, hooks, warnings, provider);
    }
}

static void attach_status(CodexBarSnapshot *snapshot,
                          const char *provider_id,
                          const CodexBarServiceStatus *status) {
    if (!snapshot || !status) return;
    for (guint index = 0; index < snapshot->providers->len; index++) {
        CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        if (!g_str_equal(provider->provider, provider_id)) continue;
        codexbar_service_status_free(provider->status);
        provider->status = status_copy(status);
    }
}

static gboolean claude_limits_unavailable(const CodexBarProvider *provider) {
    return provider && provider->note && g_str_equal(provider->note, "Limits unavailable");
}

static gboolean claude_terminal_cli_transient(const CodexBarProvider *provider) {
    return provider && provider->error && g_str_equal(provider->source, "cli") && provider->error_kind &&
           (g_str_equal(provider->error_kind, "parse") || g_str_equal(provider->error_kind, "rateLimit"));
}

static CodexBarProvider *provider_copy(const CodexBarProvider *provider) {
    if (!provider) return NULL;
    CodexBarProvider *copy = codexbar_provider_new();
    copy->provider = g_strdup(provider->provider);
    copy->account = g_strdup(provider->account);
    copy->plan = g_strdup(provider->plan);
    copy->source = g_strdup(provider->source);
    copy->dashboard_url = g_strdup(provider->dashboard_url);
    copy->note = g_strdup(provider->note);
    copy->error = g_strdup(provider->error);
    copy->error_code = provider->error_code;
    copy->error_kind = g_strdup(provider->error_kind);
    copy->has_subscription_expires_at = provider->has_subscription_expires_at;
    copy->subscription_expires_at_ms = provider->subscription_expires_at_ms;
    copy->has_subscription_renews_at = provider->has_subscription_renews_at;
    copy->subscription_renews_at_ms = provider->subscription_renews_at_ms;
    copy->has_updated_at = provider->has_updated_at;
    copy->updated_at_ms = provider->updated_at_ms;
    copy->explicit_quota_slots = provider->explicit_quota_slots;
    copy->has_credits_updated_at = provider->has_credits_updated_at;
    copy->credits_updated_at_ms = provider->credits_updated_at_ms;
    copy->runtime_scope = g_strdup(provider->runtime_scope);
    if (provider->identity) {
        copy->identity = g_new0(CodexBarProviderIdentity, 1);
        copy->identity->organization = g_strdup(provider->identity->organization);
        copy->identity->account_id = g_strdup(provider->identity->account_id);
        copy->identity->login_method = g_strdup(provider->identity->login_method);
    }
    if (provider->status) copy->status = status_copy(provider->status);
    if (provider->provider_cost) {
        copy->provider_cost = g_new0(CodexBarProviderCost, 1);
        *copy->provider_cost = *provider->provider_cost;
        copy->provider_cost->currency = g_strdup(provider->provider_cost->currency);
        copy->provider_cost->period = g_strdup(provider->provider_cost->period);
    }
    if (provider->token_cost) {
        copy->token_cost = g_new0(CodexBarTokenCost, 1);
        *copy->token_cost = *provider->token_cost;
        copy->token_cost->currency = g_strdup(provider->token_cost->currency);
        copy->token_cost->history_label = g_strdup(provider->token_cost->history_label);
    }
    copy->credit_events = provider->credit_events ? json_object_get(provider->credit_events) : NULL;
    copy->usage_extensions = provider->usage_extensions ? json_object_get(provider->usage_extensions) : NULL;
    copy->raw = provider->raw ? json_object_get(provider->raw) : NULL;
    for (guint index = 0; index < provider->quota_windows->len; index++) {
        const CodexBarQuotaWindow *window = codexbar_provider_quota_window(provider, index);
        CodexBarQuotaWindow *window_copy = codexbar_quota_window_new(window->id, window->title);
        *window_copy = *window;
        window_copy->id = g_strdup(window->id);
        window_copy->output_id = g_strdup(window->output_id);
        window_copy->title = g_strdup(window->title);
        window_copy->detail = g_strdup(window->detail);
        window_copy->reset_description = g_strdup(window->reset_description);
        window_copy->pace = NULL;
        if (window->pace) {
            window_copy->pace = g_new0(CodexBarPace, 1);
            *window_copy->pace = *window->pace;
            window_copy->pace->summary = g_strdup(window->pace->summary);
        }
        codexbar_provider_add_quota_window(copy, window_copy);
    }
    for (guint index = 0; index < provider->balances->len; index++) {
        const CodexBarBalance *balance = codexbar_provider_balance(provider, index);
        CodexBarBalance *balance_copy = g_new0(CodexBarBalance, 1);
        *balance_copy = *balance;
        balance_copy->id = g_strdup(balance->id);
        balance_copy->title = g_strdup(balance->title);
        balance_copy->unit = g_strdup(balance->unit);
        codexbar_provider_add_balance(copy, balance_copy);
    }
    return copy;
}

static void reconcile_claude_snapshot(CodexBarRuntime *runtime, CodexBarSnapshot *snapshot) {
    g_mutex_lock(&runtime->lock);
    for (guint index = 0; index < snapshot->providers->len; index++) {
        CodexBarProvider *provider = g_ptr_array_index(snapshot->providers, index);
        if (!g_str_equal(provider->provider, "claude")) continue;
        if (!provider->error) {
            if (claude_limits_unavailable(provider) && !provider->token_cost && runtime->last_good_claude &&
                runtime->last_good_claude->token_cost) {
                CodexBarProvider *prior = provider_copy(runtime->last_good_claude);
                provider->token_cost = prior->token_cost;
                prior->token_cost = NULL;
                codexbar_provider_free(prior);
            }
            codexbar_provider_free(runtime->last_good_claude);
            runtime->last_good_claude = provider_copy(provider);
        } else if (claude_terminal_cli_transient(provider) && runtime->last_good_claude) {
            CodexBarProvider *preserved = provider_copy(runtime->last_good_claude);
            g_ptr_array_index(snapshot->providers, index) = preserved;
            codexbar_provider_free(provider);
        } else {
            codexbar_provider_free(runtime->last_good_claude);
            runtime->last_good_claude = NULL;
        }
    }
    g_mutex_unlock(&runtime->lock);
}

CodexBarSnapshot *codexbar_runtime_fetch(CodexBarRuntime *runtime,
                                        GCancellable *cancellable,
                                        GError **error) {
    g_return_val_if_fail(runtime != NULL, NULL);
    const char *backend = g_getenv("CODEXBAR_BACKEND");
    gboolean skip_status = (backend && backend[0] != '\0') || environment_flag("CODEXBAR_DISABLE_STATUS");

    CodexBarConfig *config = codexbar_config_load(error);
    if (!config) return NULL;
    char *config_digest = g_strdup(config->loaded_digest);
    reconcile_config_revision(runtime, config_digest);
    json_object *hooks = copy_hooks(config);
    json_object *warnings = copy_quota_warnings(config);
    gboolean historical_tracking_enabled = config->historical_tracking_enabled;
    GPtrArray *requests = skip_status
                              ? g_ptr_array_new_with_free_func((GDestroyNotify)status_request_free)
                              : start_status_requests(runtime, config, cancellable);
    codexbar_config_free(config);

    CodexBarSnapshot *snapshot = runtime->usage_fetcher(cancellable, runtime->usage_fetcher_data, error);
    if (!snapshot) {
        g_free(config_digest);
        if (hooks) json_object_put(hooks);
        json_object_put(warnings);
        g_ptr_array_unref(requests);
        return NULL;
    }
    CodexBarConfig *publication_config = codexbar_config_load(NULL);
    gboolean publication_is_current = publication_config &&
                                      g_strcmp0(config_digest, publication_config->loaded_digest) == 0;
    codexbar_config_free(publication_config);
    g_free(config_digest);
    if (!publication_is_current) {
        if (hooks) json_object_put(hooks);
        json_object_put(warnings);
        g_ptr_array_unref(requests);
        return snapshot;
    }
    reconcile_claude_snapshot(runtime, snapshot);
    for (guint index = 0; index < requests->len; index++) {
        StatusRequest *request = g_ptr_array_index(requests, index);
        CodexBarServiceStatus *fresh = request_status(request);
        CodexBarServiceStatus *status = published_status(runtime, request);
        attach_status(snapshot, request->descriptor->id, status);
        dispatch_status_hook(runtime, hooks, request, fresh);
        codexbar_service_status_free(status);
    }
    codexbar_pace_attach_snapshot(snapshot, g_get_real_time() / 1000);
    for (guint index = 0; index < snapshot->providers->len; index++) {
        stabilize_command_code_monthly(runtime, g_ptr_array_index(snapshot->providers, index));
    }
    dispatch_snapshot_hooks(runtime, hooks, warnings, snapshot);
    if (!environment_flag("CODEXBAR_DISABLE_HISTORY")) {
        codexbar_history_store_record(runtime->history,
                                      snapshot,
                                      g_get_real_time() / 1000,
                                      historical_tracking_enabled,
                                      NULL);
    }
    if (hooks) json_object_put(hooks);
    json_object_put(warnings);
    g_ptr_array_unref(requests);
    return snapshot;
}
