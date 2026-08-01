#include "notifications.h"

typedef struct {
    char *title;
    char *body;
} NotificationRequest;

static void notification_request_free(NotificationRequest *request) {
    g_free(request->title);
    g_free(request->body);
    g_free(request);
}

static void default_sender(const char *title, const char *body, gpointer user_data) {
    (void)user_data;
    GError *error = NULL;
    GDBusConnection *bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &error);
    if (!bus) {
        g_clear_error(&error);
        return;
    }
    GVariantBuilder actions;
    GVariantBuilder hints;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    GVariant *result = g_dbus_connection_call_sync(bus,
                                                   "org.freedesktop.Notifications",
                                                   "/org/freedesktop/Notifications",
                                                   "org.freedesktop.Notifications",
                                                   "Notify",
                                                   g_variant_new("(susssasa{sv}i)",
                                                                 "CodexBar",
                                                                 0U,
                                                                 "",
                                                                 title,
                                                                 body,
                                                                 &actions,
                                                                 &hints,
                                                                 -1),
                                                   G_VARIANT_TYPE("(u)"),
                                                   G_DBUS_CALL_FLAGS_NONE,
                                                   5000,
                                                   NULL,
                                                   &error);
    if (result) g_variant_unref(result);
    g_clear_error(&error);
    g_object_unref(bus);
}

static gpointer notification_worker(gpointer data) {
    NotificationRequest *request = data;
    default_sender(request->title, request->body, NULL);
    notification_request_free(request);
    return NULL;
}

static void event_text(const CodexBarHookEvent *event, char **title, char **body) {
    const char *provider = event->provider ? event->provider : "provider";
    const char *account = event->account ? event->account : "current account";
    if (g_str_equal(event->event, "quota_low")) {
        int remaining = (int)(100 - event->usage_percent * 100 + 0.5);
        const char *window = event->window ? event->window : "usage";
        *title = g_strdup_printf("%s %s quota low", provider, window);
        *body = event->has_warning_threshold
                    ? g_strdup_printf("Account %s. %d%% left. Reached your %d%% %s warning threshold.",
                                      account,
                                      CLAMP(remaining, 0, 100),
                                      event->warning_threshold,
                                      window)
                    : g_strdup_printf("Account %s. %d%% left in %s.",
                                      account,
                                      CLAMP(remaining, 0, 100),
                                      window);
    } else if (g_str_equal(event->event, "quota_reached")) {
        *title = g_strdup_printf("%s quota reached", provider);
        *body = g_strdup_printf("%s exhausted the %s quota.",
                                account,
                                event->window ? event->window : "session");
    } else if (g_str_equal(event->event, "quota_reset")) {
        *title = g_strdup_printf("%s quota reset", provider);
        *body = g_strdup_printf("%s can use %s again.",
                                account,
                                event->window ? event->window : "the session quota");
    } else {
        *title = g_strdup_printf("%s status changed", provider);
        *body = g_strdup(event->status ? event->status : event->event);
    }
}

void codexbar_notification_send_with_sender(const CodexBarHookEvent *event,
                                             CodexBarNotificationSender sender,
                                             gpointer user_data) {
    g_return_if_fail(event != NULL);
    g_return_if_fail(event->event != NULL);
    char *title = NULL;
    char *body = NULL;
    event_text(event, &title, &body);
    sender(title, body, user_data);
    g_free(title);
    g_free(body);
}

void codexbar_notification_send(const CodexBarHookEvent *event) {
    g_return_if_fail(event != NULL);
    NotificationRequest *request = g_new0(NotificationRequest, 1);
    event_text(event, &request->title, &request->body);
    GThread *thread = g_thread_new("desktop-notification", notification_worker, request);
    g_thread_unref(thread);
}
