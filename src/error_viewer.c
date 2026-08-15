/* Built-in desktop failure log. Vigil appends managed-service failures to
 * /run/vigil-errors.log; Lumen also reports failures from its own launch path. */
#include "error_viewer.h"

#include <fcntl.h>
#include <font.h>
#include <glyph.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define ERROR_LOG_PATH "/run/vigil-errors.log"
#define ERROR_LOG_MAX  4096

static char s_log[ERROR_LOG_MAX];
static size_t s_log_len;
static size_t s_file_seen;
static glyph_window_t *s_window;
static compositor_t *s_comp;
static time_t s_last_poll;

static void
viewer_close(glyph_window_t *win)
{
    s_window = NULL;
    comp_remove_window(s_comp, win);
}

static void
viewer_render(glyph_window_t *win)
{
    surface_t *s = &win->surface;
    int x = GLYPH_BORDER_WIDTH + 20;
    int y = GLYPH_BORDER_WIDTH + GLYPH_TITLEBAR_HEIGHT + 18;
    int bottom = y + win->client_h - 24;

    draw_fill_rect(s, GLYPH_BORDER_WIDTH,
                   GLYPH_BORDER_WIDTH + GLYPH_TITLEBAR_HEIGHT,
                   win->client_w, win->client_h, 0x00151824);
    if (g_font_ui) {
        font_draw_text(s, g_font_ui, 17, x, y,
                       "A desktop component failed", 0x00FFB070);
        font_draw_text(s, g_font_ui, 13, x, y + 26,
                       "The failure log is preserved below.", 0x0099A2B5);
    }
    y += 62;

    /* Draw the newest complete lines that fit. The log format is deliberately
     * one event per line, so no scrolling widget or second text model is needed. */
    int line_h = 18;
    int max_lines = (bottom - y) / line_h;
    const char *lines[16];
    int nlines = 0;
    const char *p = s_log;
    while (*p) {
        if (nlines == (int)(sizeof(lines) / sizeof(lines[0]))) {
            memmove(lines, lines + 1, sizeof(lines) - sizeof(lines[0]));
            nlines--;
        }
        lines[nlines++] = p;
        const char *nl = strchr(p, '\n');
        if (!nl) break;
        p = nl + 1;
    }
    int first = nlines > max_lines ? nlines - max_lines : 0;
    for (int i = first; i < nlines; i++) {
        char line[96];
        size_t len = strcspn(lines[i], "\n");
        if (len >= sizeof(line)) len = sizeof(line) - 1;
        memcpy(line, lines[i], len);
        line[len] = '\0';
        if (g_font_mono)
            font_draw_text(s, g_font_mono, 13, x, y, line, 0x00D8DCE8);
        else
            draw_text_t(s, x, y, line, 0x00D8DCE8);
        y += line_h;
    }
}

static void
show_viewer(compositor_t *comp)
{
    if (!s_window) {
        s_window = glyph_window_create("Error Viewer", 620, 320);
        if (!s_window) return;
        s_window->on_render = viewer_render;
        s_window->on_close = viewer_close;
        s_window->x = (comp->fb.w - s_window->surf_w) / 2;
        s_window->y = (comp->fb.h - s_window->surf_h) / 2;
        s_comp = comp;
        comp_add_window(comp, s_window);
    } else {
        comp_raise_window(comp, s_window);
    }
    glyph_window_mark_all_dirty(s_window);
    comp->full_redraw = 1;
}

void
error_viewer_report(compositor_t *comp, const char *component,
                    const char *detail)
{
    char line[256];
    int n = snprintf(line, sizeof(line), "%s: %s\n",
                     component ? component : "desktop",
                     detail ? detail : "unknown failure");
    if (n <= 0) return;
    size_t len = (size_t)n < sizeof(line) ? (size_t)n : sizeof(line) - 1;
    if (len >= ERROR_LOG_MAX) return;
    if (s_log_len + len >= ERROR_LOG_MAX) {
        size_t drop = s_log_len + len - ERROR_LOG_MAX + 1;
        memmove(s_log, s_log + drop, s_log_len - drop);
        s_log_len -= drop;
    }
    memcpy(s_log + s_log_len, line, len);
    s_log_len += len;
    s_log[s_log_len] = '\0';
    dprintf(2, "[LUMEN-ERROR] %s", line);
    show_viewer(comp);
}

void
error_viewer_poll(compositor_t *comp)
{
    time_t now = time(NULL);
    if (now == s_last_poll) return;
    s_last_poll = now;

    int fd = open(ERROR_LOG_PATH, O_RDONLY);
    if (fd < 0) return;
    char buf[ERROR_LOG_MAX];
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0) return;
    size_t total = (size_t)n;
    if (total < s_file_seen) s_file_seen = 0;
    if (total == s_file_seen) return;
    buf[total] = '\0';
    error_viewer_report(comp, "Vigil", buf + s_file_seen);
    s_file_seen = total;
}
