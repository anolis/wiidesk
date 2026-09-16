/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef WIIDESK_X11_SESSION_H
#define WIIDESK_X11_SESSION_H
#define SESSION_ACTION "_WIIDESK_SESSION_ACTION"
#define SESSION_CAPS "_WIIDESK_SESSION_CAPS"
#define SESSION_STATUS "_WIIDESK_SESSION_STATUS"
enum session_action { SESSION_LOCK = 1, SESSION_LOGOUT, SESSION_CANCEL_LOGOUT, SESSION_FORCE_LOGOUT };
/* CAPS: managed-login-session, locker-available, logout-pending, locker-running. */
#endif
