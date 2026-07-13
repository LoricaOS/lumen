/* lumen_server.h — Lumen external window server interface */
#ifndef LUMEN_SERVER_H
#define LUMEN_SERVER_H

#include "compositor.h"
#include <lumen_proto.h>   /* lumen_set_menu_t (app menu accessors below) */

#define LUMEN_MAX_CLIENTS 8

int lumen_server_init(void);
int lumen_server_tick(compositor_t *comp, int listen_fd);

/* Fill fds[] with every connected client fd (for the main loop's idle poll —
 * a client frame then wakes the loop instead of waiting out the idle sleep).
 * Returns the count written (≤ max). */
int lumen_server_collect_fds(int *fds, int max);

/* Notify a proxy window of focus change. win may be NULL or non-proxy (no-op). */
void lumen_proxy_notify_focus(glyph_window_t *win, int focused);

/* Ask a resizable proxy window to resize to a new client-area size. Sends
 * LUMEN_EV_RESIZED; the client answers with LUMEN_OP_RESIZE_BUFFER and both
 * sides remap. No-op for non-proxy/in-process windows. */
void lumen_proxy_request_resize(glyph_window_t *win, int cw, int ch);

/* Push the open-window list to the dock (panel clients). The compositor calls
 * this once per frame when comp->windows_changed is set. */
void lumen_server_push_window_list(compositor_t *comp);

/* Drag-and-drop event delivery to proxy windows (called from the
 * compositor while it brokers a drag). x,y are window-local; the
 * helpers convert to client-area coords like mouse events do. */
/* Top-bar app menu: the focused window's published menu (NULL if none), and a
 * way to deliver a chosen item back to that window's client. Defined in
 * lumen_proto.h (lumen_set_menu_t). */
const lumen_set_menu_t *lumen_window_menu(glyph_window_t *win);
void lumen_window_send_menu_invoke(glyph_window_t *win, uint32_t command);

void lumen_proxy_send_drag_over(glyph_window_t *win, int x, int y, uint8_t op);
void lumen_proxy_send_drag_leave(glyph_window_t *win);
void lumen_proxy_send_drop(glyph_window_t *win, int x, int y, uint8_t op,
                           const char *path);

/* Register a handler for LUMEN_OP_INVOKE messages.
 * The handler receives the compositor and the requested name.
 * Lumen's main.c registers a dispatcher: "applications" (/bin/applications
 * shell component) is special; every other name resolves through the /apps
 * bundle registry (glyph_apps_find) and spawns the bundle's ELF. */
typedef void (*lumen_invoke_fn)(compositor_t *comp, const char *name);
void lumen_server_set_invoke_handler(lumen_invoke_fn fn);

#endif /* LUMEN_SERVER_H */
