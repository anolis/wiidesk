/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WIIDESK_WIFI_CONTROL_H
#define WIIDESK_WIFI_CONTROL_H
#include <signal.h>
#include <stddef.h>
int wifi_status(const char *socket_path, char *reply, size_t size);
/* WPA/WPA2 personal ASCII profile; rolls back the new profile on failure/cancel. */
int wifi_connect(const char *socket_path, const char *ssid, const char *password,
                 volatile sig_atomic_t *cancelled, char *message, size_t size);
#endif
