/* about.c -- "About LoricaOS" information window for Lumen compositor.
 *
 * Displays system version, hardware info from /proc, and credits.
 * Renders as a frosted Glyph window with labels.
 */
#include "about.h"
/* Injected by the build from git (exact tag → "1.5.0", else dev-<hash>). */
#ifndef AEGIS_VERSION
#define AEGIS_VERSION "untracked"
#endif
#include <glyph.h>
#include <font.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/utsname.h>

/* ---- System info helpers ---- */

static int
read_file_str(const char *path, char *buf, int bufsz)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int n = (int)read(fd, buf, (size_t)(bufsz - 1));
    close(fd);
    if (n < 0) return -1;
    buf[n] = '\0';
    /* Strip trailing whitespace */
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == '\r' || buf[n-1] == ' '))
        buf[--n] = '\0';
    return n;
}

/* Parse "MemTotal: XXXXX kB" from /proc/meminfo */
static void
get_mem_total(char *out, int outsz)
{
    char buf[512];
    if (read_file_str("/proc/meminfo", buf, sizeof(buf)) <= 0) {
        snprintf(out, outsz, "Unknown");
        return;
    }
    char *p = strstr(buf, "MemTotal:");
    if (!p) { snprintf(out, outsz, "Unknown"); return; }
    p += 9;
    while (*p == ' ') p++;
    /* Extract number */
    long kb = 0;
    while (*p >= '0' && *p <= '9') {
        kb = kb * 10 + (*p - '0');
        p++;
    }
    if (kb > 1024)
        snprintf(out, outsz, "%ld MB", kb / 1024);
    else
        snprintf(out, outsz, "%ld kB", kb);
}

/* Count CPUs from /proc/meminfo (look for "CPUs:" line) or fallback */
static void
get_cpu_info(char *out, int outsz)
{
    /* /proc doesn't have cpuinfo yet — read from SMP init message */
    char buf[512];
    if (read_file_str("/proc/meminfo", buf, sizeof(buf)) > 0) {
        char *p = strstr(buf, "CPUs:");
        if (p) {
            p += 5;
            while (*p == ' ') p++;
            int n = 0;
            while (*p >= '0' && *p <= '9') {
                n = n * 10 + (*p - '0');
                p++;
            }
            if (n > 0) {
                snprintf(out, outsz, "%d core%s", n, n > 1 ? "s" : "");
                return;
            }
        }
    }
    /* No core count available — report the machine arch from uname
     * (aarch64 / x86_64) instead of a hardcoded label. */
    struct utsname u;
    if (uname(&u) == 0 && u.machine[0])
        snprintf(out, outsz, "%s", u.machine);
    else
        snprintf(out, outsz, "unknown");
}

/* ---- Logo loading (reuse Bastion's format) ---- */

static uint32_t *s_logo_px;
static int s_logo_w, s_logo_h;

static void
load_logo(void)
{
    int fd = open("/usr/share/logo.raw", O_RDONLY);
    if (fd < 0) return;
    uint32_t hdr[2];
    if (read(fd, hdr, 8) != 8) { close(fd); return; }
    s_logo_w = (int)hdr[0];
    s_logo_h = (int)hdr[1];
    if (s_logo_w <= 0 || s_logo_h <= 0 || s_logo_w > 1200 || s_logo_h > 600) {
        close(fd); s_logo_w = s_logo_h = 0; return;
    }
    size_t sz = (size_t)(s_logo_w * s_logo_h) * 4;
    s_logo_px = malloc(sz);
    if (!s_logo_px) { close(fd); s_logo_w = s_logo_h = 0; return; }
    size_t got = 0;
    while (got < sz) {
        ssize_t n = read(fd, (char *)s_logo_px + got, sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got < sz) { free(s_logo_px); s_logo_px = NULL; s_logo_w = s_logo_h = 0; }
}

/* ---- Claude logo loading ---- */

static uint32_t *s_claude_px;
static int s_claude_w, s_claude_h;

static int s_screen_w, s_screen_h;  /* actual display resolution */

static void
load_claude_logo(void)
{
    int fd = open("/usr/share/claude.raw", O_RDONLY);
    if (fd < 0) return;
    uint32_t hdr[2];
    if (read(fd, hdr, 8) != 8) { close(fd); return; }
    s_claude_w = (int)hdr[0];
    s_claude_h = (int)hdr[1];
    if (s_claude_w <= 0 || s_claude_h <= 0 || s_claude_w > 400 || s_claude_h > 400) {
        close(fd); s_claude_w = s_claude_h = 0; return;
    }
    size_t sz = (size_t)(s_claude_w * s_claude_h) * 4;
    s_claude_px = malloc(sz);
    if (!s_claude_px) { close(fd); s_claude_w = s_claude_h = 0; return; }
    size_t got = 0;
    while (got < sz) {
        ssize_t n = read(fd, (char *)s_claude_px + got, sz - got);
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got < sz) { free(s_claude_px); s_claude_px = NULL; s_claude_w = s_claude_h = 0; }
}

/* ---- About panel palette + small inline UI icons (~16px, monochrome) ---- */
#define ABOUT_BG   0x00151824   /* opaque dark card body */

static void
about_center(surface_t *s, int cx, int cw, int y, int sz,
             const char *str, uint32_t col)
{
    if (!g_font_ui) { draw_text_t(s, cx + 20, y, str, col); return; }
    int tw = font_text_width(g_font_ui, sz, str);
    font_draw_text(s, g_font_ui, sz, cx + (cw - tw) / 2, y, str, col);
}

static void icon_cpu(surface_t *s, int x, int y, uint32_t c)
{
    draw_rounded_outline(s, x + 3, y + 3, 10, 10, 2, 1, c);
    draw_fill_rect(s, x + 6, y + 6, 4, 4, c);
    for (int i = 0; i < 3; i++) {
        int p = y + 4 + i * 3, q = x + 4 + i * 3;
        draw_fill_rect(s, x, p, 3, 1, c);  draw_fill_rect(s, x + 13, p, 3, 1, c);
        draw_fill_rect(s, q, y, 1, 3, c);  draw_fill_rect(s, q, y + 13, 1, 3, c);
    }
}
static void icon_mem(surface_t *s, int x, int y, uint32_t c)
{
    draw_rounded_outline(s, x + 1, y + 3, 14, 8, 1, 1, c);
    for (int i = 0; i < 4; i++) draw_fill_rect(s, x + 3 + i * 3, y + 5, 2, 4, c);
    draw_fill_rect(s, x + 3, y + 11, 2, 2, c);
    draw_fill_rect(s, x + 11, y + 11, 2, 2, c);
}
static void icon_disp(surface_t *s, int x, int y, uint32_t c)
{
    draw_rounded_outline(s, x + 1, y + 2, 14, 9, 2, 1, c);
    draw_fill_rect(s, x + 6, y + 11, 4, 2, c);
    draw_fill_rect(s, x + 4, y + 14, 8, 1, c);
}
static void icon_person(surface_t *s, int x, int y, uint32_t c)
{
    draw_circle_filled(s, x + 8, y + 5, 3, c);
    for (int i = -6; i <= 6; i++) {
        int h = 6 - (i * i) / 9;
        if (h > 0) draw_fill_rect(s, x + 8 + i, y + 14 - h, 1, h, c);
    }
}
static void icon_warn(surface_t *s, int x, int y, uint32_t c, uint32_t hole)
{
    for (int r = 0; r < 14; r++) {
        int half = (r + 1) * 7 / 14;
        draw_fill_rect(s, x + 8 - half, y + r, half * 2, 1, c);
    }
    draw_fill_rect(s, x + 7, y + 4, 2, 5, hole);
    draw_fill_rect(s, x + 7, y + 11, 2, 2, hole);
}

/* ---- Custom render for About window ---- */

static void
about_render(glyph_window_t *win)
{
    surface_t *s = &win->surface;
    int cx = GLYPH_BORDER_WIDTH;
    int cy = GLYPH_BORDER_WIDTH + GLYPH_TITLEBAR_HEIGHT;
    int cw = win->client_w;

    /* Opaque dark card — a solid panel, not frosted glass, so the wallpaper
     * (and its centered wordmark) no longer bleeds through the body. */
    draw_fill_rect(s, cx, cy, cw, win->client_h, ABOUT_BG);

    int y = cy + 18;

    /* LoricaOS logo (scaled to 25% — ~218x56) */
    if (s_logo_px && s_logo_w > 0) {
        int dw = s_logo_w / 4;
        int dh = s_logo_h / 4;
        int lx = cx + (cw - dw) / 2;
        draw_blit_alpha_scaled(s, lx, y, dw, dh, s_logo_px, s_logo_w, s_logo_h);
        y += dh + 12;
    }

    /* Version + tagline (centered). */
    about_center(s, cx, cw, y, 16, "Version " AEGIS_VERSION, 0x00C6CAD8);
    y += 22;
    about_center(s, cx, cw, y, 13,
                 "Capability-secure, POSIX-compatible OS", 0x008089A0);
    y += 26;

    /* v1 warning — the whole thing inside ONE amber callout: a warning glyph +
     * the two amber lines, then the dim explanation just below (no dangling
     * line outside the box). */
    {
        const char *a1 = "v1 software -- first public release";
        const char *a2 = "not production-hardened";
        const char *d1 = "The C kernel likely contains real,";
        const char *d2 = "exploitable vulnerabilities.";
        int bx = cx + 26, bw = cw - 52, bh = 88;
        draw_blend_rounded_rect(s, bx, y, bw, bh, R_MD, 0x00FFAA55, 24);
        draw_rounded_outline(s, bx, y, bw, bh, R_MD, 1, 0x009C7030);
        if (g_font_ui) {
            icon_warn(s, bx + 18, y + 16, 0x00FFB454, ABOUT_BG);
            int tx = bx + 46;
            font_draw_text(s, g_font_ui, 13, tx, y + 15, a1, 0x00FFC676);
            font_draw_text(s, g_font_ui, 13, tx, y + 32, a2, 0x00FFC676);
            int c3 = bx + (bw - font_text_width(g_font_ui, 13, d1)) / 2;
            int c4 = bx + (bw - font_text_width(g_font_ui, 13, d2)) / 2;
            font_draw_text(s, g_font_ui, 13, c3, y + 55, d1, 0x00B7A08C);
            font_draw_text(s, g_font_ui, 13, c4, y + 71, d2, 0x00B7A08C);
        }
        y += bh + 20;
    }

    /* Separator */
    draw_blend_rect(s, cx + 26, y, cw - 52, 1, 0x00FFFFFF, 20);
    y += 18;

    /* System info — icon + aligned label/value columns. */
    {
        char mem_str[64], cpu_str[64], disp[32];
        get_mem_total(mem_str, sizeof(mem_str));
        get_cpu_info(cpu_str, sizeof(cpu_str));
        snprintf(disp, sizeof(disp), "%dx%d", s_screen_w, s_screen_h);
        int ix = cx + 28, lx = ix + 28, vx = ix + 118;
        uint32_t ic = 0x008792A6, lc = 0x00A7AEC0;
        if (g_font_ui) {
            icon_cpu(s, ix, y, ic);
            font_draw_text(s, g_font_ui, 14, lx, y + 1, "CPU", lc);
            font_draw_text(s, g_font_ui, 14, vx, y + 1, cpu_str, C_TEXT);
            y += 26;
            icon_mem(s, ix, y, ic);
            font_draw_text(s, g_font_ui, 14, lx, y + 1, "Memory", lc);
            font_draw_text(s, g_font_ui, 14, vx, y + 1, mem_str, C_TEXT);
            y += 26;
            icon_disp(s, ix, y, ic);
            font_draw_text(s, g_font_ui, 14, lx, y + 1, "Display", lc);
            font_draw_text(s, g_font_ui, 14, vx, y + 1, disp, C_TEXT);
            y += 30;
        }
    }

    /* Separator */
    draw_blend_rect(s, cx + 26, y, cw - 52, 1, 0x00FFFFFF, 20);
    y += 18;

    /* Credits — maintained by the LoricaOS project (an org now). */
    if (g_font_ui) {
        icon_person(s, cx + 28, y, 0x008792A6);
        font_draw_text(s, g_font_ui, 14, cx + 56, y + 1,
                       "The LoricaOS project", 0x00B8BECE);
        font_draw_text(s, g_font_ui, 13, cx + 56, y + 19,
                       "github.com/LoricaOS", 0x008089A0);
        y += 40;
    }

    /* Claude logo + attribution — pinned to bottom of window */
    {
        int target_h = 24;
        int by = cy + win->client_h - target_h - 12;  /* 12px from bottom */

        if (s_claude_px && s_claude_w > 0) {
            int target_w = s_claude_w * target_h / s_claude_h;
            if (target_w <= 0) target_w = 24;

            /* Center the logo+text as a unit */
            int text_w = g_font_ui ? font_text_width(g_font_ui, 13, "Built with Claude Code") : 180;
            int total_unit = target_w + 8 + text_w;
            int lx = cx + (cw - total_unit) / 2;

            /* Pill button behind the logo+label (matches the desktop mockup):
             * a subtle raised chip on the frosted panel. */
            {
                int pad_x = 16, pad_y = 7;
                int pw = total_unit + 2 * pad_x;
                int ph = target_h + 2 * pad_y;
                int px = cx + (cw - pw) / 2;
                int py = by - pad_y;
                draw_blend_rounded_rect(s, px, py, pw, ph, ph / 2, 0x00FFFFFF, 22);
                draw_rounded_outline(s, px, py, pw, ph, ph / 2, 1, 0x002E3242);
            }

            draw_blit_alpha_scaled(s, lx, by, target_w, target_h,
                                   s_claude_px, s_claude_w, s_claude_h);
            if (g_font_ui) {
                int tx = lx + target_w + 8;
                int ty = by + (target_h - font_height(g_font_ui, 13)) / 2;
                font_draw_text(s, g_font_ui, 13, tx, ty,
                               "Built with Claude Code", 0x00808898);
            }
        } else {
            if (g_font_ui) {
                const char *cc = "Built with Claude Code";
                int tw = font_text_width(g_font_ui, 13, cc);
                font_draw_text(s, g_font_ui, 13, cx + (cw - tw) / 2, by,
                               cc, 0x00808898);
            }
        }
    }
}

/* ---- Public API ---- */

glyph_window_t *
about_create(int screen_w, int screen_h)
{
    s_screen_w = screen_w;
    s_screen_h = screen_h;

    /* Load logos on first call */
    if (!s_logo_px)
        load_logo();
    if (!s_claude_px)
        load_claude_logo();

    glyph_window_t *win = glyph_window_create("About LoricaOS", 400, 500);
    if (!win) return NULL;

    win->on_render = about_render;
    win->x = (screen_w - win->surf_w) / 2;
    win->y = 80;

    glyph_window_mark_all_dirty(win);
    return win;
}
