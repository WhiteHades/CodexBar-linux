#pragma once

#include "hooks.h"

#include <gio/gio.h>

typedef void (*CodexBarNotificationSender)(const char *title,
                                           const char *body,
                                           gpointer user_data);

void codexbar_notification_send(const CodexBarHookEvent *event);
void codexbar_notification_send_with_sender(const CodexBarHookEvent *event,
                                             CodexBarNotificationSender sender,
                                             gpointer user_data);
