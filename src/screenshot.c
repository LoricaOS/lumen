/* screenshot.c — PrintScreen capture for the lumen compositor.
 *
 * On PrintScreen (kernel emits ESC[p; see main.c's CSI handler), grabs the live
 * framebuffer, writes it to $HOME/Pictures/screenshots/screenshot-<ts>.bmp
 * (mkdir -p the dir), then plays a brief white flash + shows a shrunk preview of
 * the shot in the top-right corner for ~2 s. The flash/thumbnail are drawn into
 * the back buffer each frame (screenshot_draw_overlay) while screenshot_active()
 * keeps the compositor's animation loop running.
 *
 * BMP (uncompressed 32bpp) keeps this dependency-free; the image viewer loads it
 * via stb_image. PNG is a future nicety.
 */
#define _GNU_SOURCE
#include "compositor.h"
#include <draw.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>

#define SHOT_FLASH_FRAMES  6      /* white flash fade-out length            */
#define SHOT_THUMB_FRAMES  120    /* ~2 s preview at ~60 fps                 */
#define SHOT_THUMB_W       260    /* preview width; height keeps aspect      */

static uint32_t *s_thumb;         /* scaled preview pixels (own buffer)      */
static int       s_thumb_w, s_thumb_h;
static int       s_flash;         /* remaining flash frames                  */
static int       s_thumb_frames;  /* remaining preview frames                */

/* mkdir -p */
static void
make_dirs(const char *path)
{
    char tmp[512];
    strncpy(tmp, path, sizeof tmp - 1);
    tmp[sizeof tmp - 1] = '\0';
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') { *p = '\0'; mkdir(tmp, 0755); *p = '/'; }
    }
    mkdir(tmp, 0755);
}

/* Write a 32bpp bottom-up BMP. src is 0x00RRGGBB; BMP wants B,G,R,A bytes. */
static int
save_bmp(const char *path, const uint32_t *px, int w, int h, int pitch)
{
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    uint32_t rowsz = (uint32_t)w * 4, imgsz = rowsz * (uint32_t)h;
    uint8_t hdr[54];
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t *)&hdr[2]  = 54 + imgsz;
    *(uint32_t *)&hdr[10] = 54;
    *(uint32_t *)&hdr[14] = 40;
    *(int32_t  *)&hdr[18] = w;
    *(int32_t  *)&hdr[22] = h;          /* positive → bottom-up rows */
    *(uint16_t *)&hdr[26] = 1;
    *(uint16_t *)&hdr[28] = 32;
    *(uint32_t *)&hdr[34] = imgsz;
    fwrite(hdr, 1, sizeof hdr, f);
    uint8_t *row = malloc(rowsz);
    if (!row) { fclose(f); return -1; }
    for (int y = h - 1; y >= 0; y--) {
        const uint32_t *s = &px[(size_t)y * pitch];
        for (int x = 0; x < w; x++) {
            uint32_t p = s[x];
            row[x * 4 + 0] = (uint8_t)(p & 0xFF);
            row[x * 4 + 1] = (uint8_t)((p >> 8) & 0xFF);
            row[x * 4 + 2] = (uint8_t)((p >> 16) & 0xFF);
            row[x * 4 + 3] = 0xFF;
        }
        fwrite(row, 1, rowsz, f);
    }
    free(row);
    fclose(f);
    return 0;
}

void
screenshot_take(compositor_t *c)
{
    const char *home = getenv("HOME");
    if (!home || !*home) home = "/home/live";
    char dir[512];
    snprintf(dir, sizeof dir, "%s/Pictures/screenshots", home);
    make_dirs(dir);

    time_t t = time(NULL);
    struct tm tmv;
    struct tm *tm = localtime_r(&t, &tmv);
    char path[600];
    snprintf(path, sizeof path, "%s/screenshot-%04d%02d%02d-%02d%02d%02d.bmp",
             dir, tm->tm_year + 1900, tm->tm_mon + 1, tm->tm_mday,
             tm->tm_hour, tm->tm_min, tm->tm_sec);
    if (save_bmp(path, c->fb.buf, c->fb.w, c->fb.h, c->fb.pitch) != 0)
        dprintf(2, "lumen: screenshot: could not write %s\n", path);

    /* Build the corner preview (nearest-neighbor shrink). */
    int tw = SHOT_THUMB_W;
    int th = tw * c->fb.h / c->fb.w;
    free(s_thumb);
    s_thumb = malloc((size_t)tw * th * sizeof(uint32_t));
    if (s_thumb) {
        for (int y = 0; y < th; y++)
            for (int x = 0; x < tw; x++) {
                int sx = x * c->fb.w / tw, sy = y * c->fb.h / th;
                s_thumb[y * tw + x] = c->fb.buf[(size_t)sy * c->fb.pitch + sx];
            }
        s_thumb_w = tw; s_thumb_h = th;
    }

    s_flash = SHOT_FLASH_FRAMES;
    s_thumb_frames = SHOT_THUMB_FRAMES;
    c->full_redraw = 1;
}

/* Drawn into the back buffer at the end of a full composite, before the flip. */
void
screenshot_draw_overlay(compositor_t *c)
{
    if (s_flash > 0) {
        int alpha = 230 * s_flash / SHOT_FLASH_FRAMES;
        draw_blend_rect(&c->back, 0, 0, c->back.w, c->back.h, 0x00FFFFFF, alpha);
        s_flash--;
    }
    if (s_thumb && s_thumb_frames > 0) {
        int m = 18, bd = 4;
        int x = c->back.w - s_thumb_w - m, y = m;
        draw_fill_rect(&c->back, x - bd, y - bd,
                       s_thumb_w + 2 * bd, s_thumb_h + 2 * bd, 0x00101010);
        draw_rect(&c->back, x - bd, y - bd,
                  s_thumb_w + 2 * bd, s_thumb_h + 2 * bd, 0x00FFFFFF);
        draw_blit(&c->back, x, y, s_thumb, s_thumb_w, s_thumb_h);
        s_thumb_frames--;
    }
}

int
screenshot_active(void)
{
    return s_flash > 0 || s_thumb_frames > 0;
}
