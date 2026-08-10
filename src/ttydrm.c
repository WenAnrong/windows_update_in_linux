/*
 * ttydrm.c — DRM/TTY direct-render mode ("unescapable" prank).
 *
 * Renders the fake Windows update screen straight to the physical display via
 * DRM/KMS, bypassing the desktop entirely — no GTK window, no WebKit, no
 * compositor. Because the desktop is suspended and the display is owned by
 * this process, the victim cannot Alt+Tab or switch windows to escape
 * (works on Wayland too, where a normal window could always be escaped).
 *
 * Requires root (VT ioctls + DRM master). Keeps the same safety model as the
 * GUI mode: after `timeout_sec` the saved CRTC is restored, the desktop VT is
 * switched back and the process exits. It NEVER reboots.
 *
 * Flow:
 *   1. find the active VT, switch to an idle VT (desktop suspends, releases DRM)
 *   2. open /dev/dri/card* and drmSetMaster()
 *   3. create a dumb framebuffer and draw the Win11-style update UI (FreeType)
 *   4. animate progress 0 -> 35% (then stuck) with a CSS-style dot spinner
 *   5. after timeout_sec: restore CRTC, drop master, switch back to desktop VT
 *
 * The layout mirrors ui/update.html (design canvas 1920x1080, scaled).
 */
#include "ttydrm.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <linux/vt.h>
#include <xf86drm.h>
#include <xf86drmMode.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#ifdef HAVE_FONTCONFIG
#include <fontconfig/fontconfig.h>
#endif

#define DEFAULT_ORIGIN_VT 2   /* fallback restore target if VT query fails */
#define TARGET_VT         6   /* idle VT to switch to */
#define STUCK_AT          35  /* fake progress freeze point */
#define FRAME_MS          120

/* Colors (0x00RRGGBB) — matches ui/update.css */
#define COL_BG   0x00000000UL
#define COL_FG   0x00FFFFFFUL
#define COL_SUB  0x00CCCCCCUL
#define COL_HINT 0x00999999UL
#define COL_FOOT 0x008F8F8FUL
#define COL_BAR  0x00333333UL

static const char *font_family_for_lang(void);

/* ================================================================== */
/* monotonic milliseconds                                              */
/* ================================================================== */
static int64_t now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ================================================================== */
/* VT helpers                                                          */
/* ================================================================== */
static int vt_open(void)
{
    return open("/dev/tty0", O_RDWR);
}

static int vt_get_active(int fd, int *vt)
{
    struct vt_stat st;
    if (ioctl(fd, VT_GETSTATE, &st) < 0)
        return -1;
    *vt = (int)st.v_active;
    return 0;
}

static int vt_switch_to(int fd, int vt)
{
    if (ioctl(fd, VT_ACTIVATE, vt) < 0)
        return -1;
    if (ioctl(fd, VT_WAITACTIVE, vt) < 0)
        return -1;
    return 0;
}

/* ================================================================== */
/* DRM context                                                         */
/* ================================================================== */
typedef struct {
    int fd;
    drmModeRes *resources;
    drmModeConnector *connector;
    drmModeCrtc *saved_crtc;
    drmModeModeInfo mode;
    uint32_t crtc_id;
    uint32_t width, height, pitch;
    uint32_t fb_id;
    uint32_t dumb_handle, dumb_size;
    uint32_t *map_ptr;
} DrmCtx;

static void drm_ctx_init(DrmCtx *c)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->map_ptr = (uint32_t *)MAP_FAILED;
}

static int has_connected_display(int fd)
{
    drmModeRes *res = drmModeGetResources(fd);
    int i, ok = 0;
    if (!res)
        return 0;
    for (i = 0; i < res->count_connectors; i++) {
        drmModeConnector *conn = drmModeGetConnector(fd, res->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED &&
            conn->count_modes > 0) {
            ok = 1;
            drmModeFreeConnector(conn);
            break;
        }
        if (conn)
            drmModeFreeConnector(conn);
    }
    drmModeFreeResources(res);
    return ok;
}

static int drm_open_device(DrmCtx *c)
{
    int i;
    for (i = 0; i < 8; i++) {
        char dev[64];
        snprintf(dev, sizeof(dev), "/dev/dri/card%d", i);
        c->fd = open(dev, O_RDWR | O_CLOEXEC);
        if (c->fd < 0)
            continue;
        if (!has_connected_display(c->fd)) {
            close(c->fd);
            c->fd = -1;
            continue;
        }
        break;
    }
    if (c->fd < 0) {
        fprintf(stderr, "[tty] 未找到带显示器的 DRM 设备（ls /dev/dri）\n");
        return -1;
    }
    if (drmSetMaster(c->fd) < 0) {
        perror("[tty] drmSetMaster");
        close(c->fd);
        c->fd = -1;
        return -1;
    }
    return 0;
}

static int drm_find_connector(DrmCtx *c)
{
    int i;
    c->resources = drmModeGetResources(c->fd);
    if (!c->resources) {
        perror("[tty] drmModeGetResources");
        return -1;
    }
    for (i = 0; i < c->resources->count_connectors; i++) {
        drmModeConnector *conn =
            drmModeGetConnector(c->fd, c->resources->connectors[i]);
        if (conn && conn->connection == DRM_MODE_CONNECTED &&
            conn->count_modes > 0) {
            c->connector = conn;
            break;
        }
        if (conn)
            drmModeFreeConnector(conn);
    }
    if (!c->connector) {
        fprintf(stderr, "[tty] 未找到已连接的显示器\n");
        return -1;
    }
    c->mode = c->connector->modes[0];
    c->width = c->mode.hdisplay;
    c->height = c->mode.vdisplay;

    if (c->connector->encoder_id) {
        drmModeEncoder *enc =
            drmModeGetEncoder(c->fd, c->connector->encoder_id);
        if (enc) {
            c->crtc_id = enc->crtc_id;
            drmModeFreeEncoder(enc);
        }
    }
    if (c->crtc_id == 0 && c->resources->count_crtcs > 0)
        c->crtc_id = c->resources->crtcs[0];
    return 0;
}

static int drm_create_fb(DrmCtx *c)
{
    struct drm_mode_create_dumb creq;
    struct drm_mode_map_dumb mreq;

    memset(&creq, 0, sizeof(creq));
    creq.width = c->width;
    creq.height = c->height;
    creq.bpp = 32;
    if (ioctl(c->fd, DRM_IOCTL_MODE_CREATE_DUMB, &creq) < 0) {
        perror("[tty] CREATE_DUMB");
        return -1;
    }
    c->dumb_handle = creq.handle;
    c->dumb_size = creq.size;
    c->pitch = creq.pitch;

    if (drmModeAddFB(c->fd, c->width, c->height, 24, 32,
                     c->pitch, c->dumb_handle, &c->fb_id) < 0) {
        perror("[tty] drmModeAddFB");
        return -1;
    }

    memset(&mreq, 0, sizeof(mreq));
    mreq.handle = c->dumb_handle;
    if (ioctl(c->fd, DRM_IOCTL_MODE_MAP_DUMB, &mreq) < 0) {
        perror("[tty] MAP_DUMB");
        return -1;
    }
    c->map_ptr = (uint32_t *)mmap(NULL, c->dumb_size, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, c->fd, mreq.offset);
    if (c->map_ptr == (uint32_t *)MAP_FAILED) {
        perror("[tty] mmap");
        return -1;
    }
    return 0;
}

static int drm_flip(DrmCtx *c)
{
    if (drmModeSetCrtc(c->fd, c->crtc_id, c->fb_id, 0, 0,
                       &c->connector->connector_id, 1, &c->mode) < 0) {
        perror("[tty] drmModeSetCrtc");
        return -1;
    }
    return 0;
}

static int drm_set_mode(DrmCtx *c)
{
    c->saved_crtc = drmModeGetCrtc(c->fd, c->crtc_id);
    if (!c->saved_crtc) {
        perror("[tty] drmModeGetCrtc (save)");
        return -1;
    }
    return drm_flip(c);
}

static int drm_restore(DrmCtx *c)
{
    if (!c->saved_crtc)
        return 0;
    return drmModeSetCrtc(c->fd, c->saved_crtc->crtc_id,
                          c->saved_crtc->buffer_id,
                          c->saved_crtc->x, c->saved_crtc->y,
                          &c->connector->connector_id, 1,
                          &c->saved_crtc->mode);
}

static void drm_cleanup(DrmCtx *c)
{
    struct drm_mode_destroy_dumb dreq;
    if (c->saved_crtc) {
        drmModeFreeCrtc(c->saved_crtc);
        c->saved_crtc = NULL;
    }
    if (c->map_ptr != (uint32_t *)MAP_FAILED && c->dumb_size > 0) {
        munmap(c->map_ptr, c->dumb_size);
        c->map_ptr = (uint32_t *)MAP_FAILED;
    }
    if (c->fb_id > 0) {
        drmModeRmFB(c->fd, c->fb_id);
        c->fb_id = 0;
    }
    if (c->dumb_handle > 0) {
        memset(&dreq, 0, sizeof(dreq));
        dreq.handle = c->dumb_handle;
        ioctl(c->fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dreq);
        c->dumb_handle = 0;
    }
    if (c->connector) {
        drmModeFreeConnector(c->connector);
        c->connector = NULL;
    }
    if (c->resources) {
        drmModeFreeResources(c->resources);
        c->resources = NULL;
    }
    if (c->fd >= 0) {
        drmDropMaster(c->fd);
        close(c->fd);
        c->fd = -1;
    }
}

/* ================================================================== */
/* framebuffer primitives (XRGB8888)                                   */
/* ================================================================== */
static void fb_clear(uint32_t *b, uint32_t w, uint32_t h, uint32_t sp,
                     uint32_t color)
{
    uint32_t y;
    for (y = 0; y < h; y++) {
        uint32_t *row = b + (uint64_t)y * sp;
        uint32_t x;
        for (x = 0; x < w; x++)
            row[x] = color;
    }
}

static void fb_fill_rect(uint32_t *b, uint32_t w, uint32_t h, uint32_t sp,
                         int x, int y, int rw, int rh, uint32_t color)
{
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = x + rw, y1 = y + rh;
    int yy, xx;
    if (x1 > (int)w)
        x1 = (int)w;
    if (y1 > (int)h)
        y1 = (int)h;
    if (x0 >= x1 || y0 >= y1)
        return;
    for (yy = y0; yy < y1; yy++) {
        uint32_t *row = b + (uint64_t)yy * sp;
        for (xx = x0; xx < x1; xx++)
            row[xx] = color;
    }
}

static void fb_blend_px(uint32_t *b, uint32_t w, uint32_t h, uint32_t sp,
                        int x, int y, uint32_t color, uint8_t alpha)
{
    uint32_t sr, sg, sb, dr, dg, db, a, inv;
    uint32_t *dst;
    if (x < 0 || y < 0 || x >= (int)w || y >= (int)h || alpha == 0)
        return;
    dst = b + (uint64_t)y * sp + (uint32_t)x;
    if (alpha == 255) {
        *dst = color;
        return;
    }
    sr = (color >> 16) & 0xFF;
    sg = (color >> 8) & 0xFF;
    sb = color & 0xFF;
    dr = (*dst >> 16) & 0xFF;
    dg = (*dst >> 8) & 0xFF;
    db = *dst & 0xFF;
    a = alpha;
    inv = 255 - a;
    *dst = (((sr * a + dr * inv) / 255) << 16) |
           (((sg * a + dg * inv) / 255) << 8) |
           ((sb * a + db * inv) / 255);
}

static void fb_fill_circle(uint32_t *b, uint32_t w, uint32_t h, uint32_t sp,
                           int cx, int cy, int r, uint32_t color, uint8_t alpha)
{
    int yy, xx, r2 = r * r;
    for (yy = cy - r; yy <= cy + r; yy++) {
        for (xx = cx - r; xx <= cx + r; xx++) {
            int dx = xx - cx, dy = yy - cy;
            if (dx * dx + dy * dy <= r2)
                fb_blend_px(b, w, h, sp, xx, yy, color, alpha);
        }
    }
}

/* ================================================================== */
/* FreeType text rendering                                             */
/* ================================================================== */
typedef struct {
    FT_Library lib;
    FT_Face face;
} FontCtx;

static int font_init(FontCtx *fc)
{
    memset(fc, 0, sizeof(*fc));
    return FT_Init_FreeType(&fc->lib);
}

static int font_load(FontCtx *fc, const char *family, unsigned px)
{
    int i;
    fc->face = NULL;

#ifdef HAVE_FONTCONFIG
    if (family && *family) {
        FcPattern *pat = FcNameParse((const FcChar8 *)family);
        if (pat) {
            FcConfigSubstitute(NULL, pat, FcMatchPattern);
            FcDefaultSubstitute(pat);
            FcResult result = FcResultMatch;
            FcPattern *match = FcFontMatch(NULL, pat, &result);
            if (match) {
                FcChar8 *file = NULL;
                if (FcPatternGetString(match, FC_FILE, 0, &file) == FcResultMatch)
                    FT_New_Face(fc->lib, (const char *)file, 0, &fc->face);
                FcPatternDestroy(match);
            }
            FcPatternDestroy(pat);
        }
        if (fc->face) {
            FT_Set_Pixel_Sizes(fc->face, 0, px);
            return 0;
        }
    }
#endif

    static const char *const fallback[] = {
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
        "/usr/share/fonts/opentype/noto/NotoSansCJK-Medium.ttc",
        "/usr/share/fonts/truetype/droid/DroidSansFallbackFull.ttf",
        "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
        "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
        NULL,
    };
    for (i = 0; fallback[i]; i++) {
        if (FT_New_Face(fc->lib, fallback[i], 0, &fc->face) == 0) {
            FT_Set_Pixel_Sizes(fc->face, 0, px);
            return 0;
        }
    }
    return -1;
}

static uint32_t utf8_next(const char **pp)
{
    const unsigned char *s = (const unsigned char *)*pp;
    unsigned cp;
    int n, i;
    if (!*s)
        return 0;
    if ((s[0] & 0x80) == 0) {
        cp = s[0];
        n = 1;
    } else if ((s[0] & 0xE0) == 0xC0) {
        cp = s[0] & 0x1F;
        n = 2;
    } else if ((s[0] & 0xF0) == 0xE0) {
        cp = s[0] & 0x0F;
        n = 3;
    } else if ((s[0] & 0xF8) == 0xF0) {
        cp = s[0] & 0x07;
        n = 4;
    } else {
        *pp += 1;
        return 0xFFFFFFFF;
    }
    for (i = 1; i < n; i++) {
        if ((s[i] & 0xC0) != 0x80) {
            *pp += 1;
            return 0xFFFFFFFF;
        }
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    *pp += n;
    return cp;
}

static int text_width(FontCtx *fc, const char *text, unsigned px)
{
    int w = 0;
    if (!fc || !fc->face)
        return 0;
    FT_Set_Pixel_Sizes(fc->face, 0, px);
    while (*text) {
        uint32_t cp = utf8_next(&text);
        if (cp == 0 || cp == 0xFFFFFFFF)
            continue;
        if (FT_Load_Char(fc->face, cp, FT_LOAD_DEFAULT) == 0)
            w += fc->face->glyph->advance.x >> 6;
    }
    return w;
}

/* Draw UTF-8 text. `cx` is the center x when `centered`; else the left edge. */
static void draw_text(FontCtx *fc, uint32_t *b, uint32_t w, uint32_t h,
                      uint32_t sp, const char *text, int cx, int y_top,
                      uint32_t color, unsigned px, int centered)
{
    FT_GlyphSlot slot;
    const char *p;
    int min_left = 0, max_top = 0, first = 1, pen_x, baseline, row, col;
    int tw;

    if (!fc || !fc->face)
        return;

    tw = text_width(fc, text, px);
    if (centered)
        cx -= tw / 2;

    /* pass 1: measure */
    for (p = text; *p;) {
        uint32_t cp = utf8_next(&p);
        if (cp == 0xFFFFFFFF)
            continue;
        if (FT_Load_Char(fc->face, cp, FT_LOAD_DEFAULT) != 0)
            continue;
        slot = fc->face->glyph;
        if (first) {
            min_left = slot->bitmap_left;
            max_top = slot->bitmap_top;
            first = 0;
        } else {
            if (slot->bitmap_left < min_left)
                min_left = slot->bitmap_left;
            if (slot->bitmap_top > max_top)
                max_top = slot->bitmap_top;
        }
    }

    pen_x = cx - min_left;
    baseline = y_top + max_top;

    /* pass 2: render */
    for (p = text; *p;) {
        uint32_t cp = utf8_next(&p);
        if (cp == 0)
            break;
        if (cp == 0xFFFFFFFF) {
            pen_x += (int)px;
            continue;
        }
        if (FT_Load_Char(fc->face, cp, FT_LOAD_RENDER) != 0) {
            pen_x += (int)px;
            continue;
        }
        slot = fc->face->glyph;
        if (slot->bitmap.width > 0 && slot->bitmap.rows > 0) {
            int gx = pen_x + slot->bitmap_left;
            int gy = baseline - slot->bitmap_top;
            for (row = 0; row < (int)slot->bitmap.rows; row++) {
                for (col = 0; col < (int)slot->bitmap.width; col++) {
                    uint8_t a =
                        slot->bitmap.buffer[row * slot->bitmap.pitch + col];
                    if (a)
                        fb_blend_px(b, w, h, sp, gx + col, gy + row, color, a);
                }
            }
        }
        pen_x += slot->advance.x >> 6;
    }
}

/* ================================================================== */
/* UI drawing (mirrors ui/update.html on a 1920x1080 design canvas)    */
/* ================================================================== */
static void draw_spinner(uint32_t *b, uint32_t w, uint32_t h, uint32_t sp,
                         int cx, int cy, double s, int64_t now)
{
    const int N = 12;
    const double period = 1600.0;
    const double TAU = 6.28318530717958647692;
    int i;
    int orbit = (int)(38 * s);
    int dot_r = (int)(5 * s);
    double phase = fmod((double)now / period, 1.0);

    if (dot_r < 2)
        dot_r = 2;

    for (i = 0; i < N; i++) {
        double ang = i * (TAU / N);
        int dx = (int)(cx + cos(ang) * orbit);
        int dy = (int)(cy + sin(ang) * orbit);
        double d = fmod((double)i - phase * N + (double)N, (double)N);
        double a;
        uint8_t alpha;
        if (d <= 1.0)
            a = 1.0;
        else if (d <= 2.0)
            a = 2.0 - d; /* 1 -> 0 */
        else
            a = 0.10;
        alpha = (uint8_t)(a * 255);
        fb_fill_circle(b, w, h, sp, dx, dy, dot_r, COL_FG, alpha);
    }
}

static void draw_frame(FontCtx *fc, DrmCtx *drm, int progress, int64_t now)
{
    uint32_t *b = drm->map_ptr;
    uint32_t W = drm->width, H = drm->height, sp = drm->pitch / 4;
    double s = (double)H / 1080.0;
    int cx = (int)W / 2;
    int sy, ty, ty2, ty3, barw, barx, barh, by, fill;

    if (s > 2.0)
        s = 2.0;

    fb_clear(b, W, H, sp, COL_BG);

    /* spinner + headline block */
    sy = (int)(H * 0.40);
    draw_spinner(b, W, H, sp, cx, sy, s, now);

    ty = sy + (int)(66 * s);
    draw_text(fc, b, W, H, sp, "正在配置 Linux 更新", cx, ty,
              COL_FG, (unsigned)(30 * s), 1);
    ty2 = ty + (int)(48 * s);
    draw_text(fc, b, W, H, sp, "Configuring Linux updates…", cx, ty2,
              COL_SUB, (unsigned)(15 * s), 1);
    ty3 = ty2 + (int)(34 * s);
    draw_text(fc, b, W, H, sp, "请保持计算机开机", cx, ty3,
              COL_HINT, (unsigned)(14 * s), 1);
    draw_text(fc, b, W, H, sp, "Please keep your computer on",
              cx, ty3 + (int)(26 * s), COL_HINT, (unsigned)(14 * s), 1);

    /* progress bar */
    barw = (int)(0.34 * W);
    if (barw < (int)(260 * s))
        barw = (int)(260 * s);
    if (barw > (int)(440 * s))
        barw = (int)(440 * s);
    barx = cx - barw / 2;
    barh = (int)(5 * s);
    if (barh < 2)
        barh = 2;
    by = (int)(H - 120 * s);
    fb_fill_rect(b, W, H, sp, barx, by, barw, barh, COL_BAR);
    fill = (int)((int64_t)barw * progress / 100);
    if (fill > 0)
        fb_fill_rect(b, W, H, sp, barx, by, fill, barh, COL_FG);

    /* percent */
    {
        char pct[16];
        snprintf(pct, sizeof(pct), "%d%%", progress);
        draw_text(fc, b, W, H, sp, pct, cx, by + barh + (int)(16 * s),
                  COL_FG, (unsigned)(16 * s), 1);
    }

    /* footnote */
    draw_text(fc, b, W, H, sp, "您的 PC 将在完成更新后多次重启", cx,
              by + barh + (int)(56 * s), COL_FOOT, (unsigned)(13 * s), 1);
    draw_text(fc, b, W, H, sp,
              "Your PC will restart several times before it's done", cx,
              by + barh + (int)(82 * s), COL_FOOT, (unsigned)(13 * s), 1);
}

static const char *font_family_for_lang(void)
{
    const char *lang = getenv("LANG");
    if (!lang)
        return "Noto Sans CJK SC";
    if (strstr(lang, "zh_TW") || strstr(lang, "zh_HK"))
        return "Noto Sans CJK TC";
    if (strstr(lang, "ja"))
        return "Noto Sans CJK JP";
    if (strstr(lang, "ko"))
        return "Noto Sans CJK KR";
    return "Noto Sans CJK SC";
}

/* ================================================================== */
/* public API                                                          */
/* ================================================================== */
int fake_update_ttydrm_run(unsigned int timeout_sec)
{
    DrmCtx drm;
    FontCtx fc;
    int tty_fd = -1, origin_vt = -1, ret = 1;

    if (geteuid() != 0) {
        fprintf(stderr, "错误：需要 root 权限（sudo windows_update_in_linux）\n");
        return 2;
    }
    if (timeout_sec == 0)
        timeout_sec = 60;

    drm_ctx_init(&drm);
    memset(&fc, 0, sizeof(fc));
    srand((unsigned)time(NULL));

    tty_fd = vt_open();
    if (tty_fd < 0) {
        perror("打开 /dev/tty0 失败");
        return 1;
    }

    if (vt_get_active(tty_fd, &origin_vt) < 0)
        origin_vt = DEFAULT_ORIGIN_VT;
    fprintf(stderr, "[tty] 当前桌面在 tty%d，切到 tty%d...\n",
            origin_vt, TARGET_VT);

    if (origin_vt != TARGET_VT) {
        if (vt_switch_to(tty_fd, TARGET_VT) < 0) {
            perror("切换 VT 失败");
            goto cleanup;
        }
    }

    if (drm_open_device(&drm) < 0)
        goto cleanup;
    if (drm_find_connector(&drm) < 0)
        goto cleanup;
    fprintf(stderr, "[tty] 分辨率 %ux%u\n", drm.width, drm.height);
    if (drm_create_fb(&drm) < 0)
        goto cleanup;

    if (font_init(&fc) != 0) {
        fprintf(stderr, "[tty] FreeType 初始化失败\n");
    } else if (font_load(&fc, font_family_for_lang(), 48) != 0) {
        fprintf(stderr, "[tty] 未找到可用字体，仅显示图形\n");
        FT_Done_FreeType(fc.lib);
        memset(&fc, 0, sizeof(fc));
    }

    if (drm_set_mode(&drm) < 0)
        goto cleanup;

    /* animation: 0 -> 35% (stuck), spinner spins, then timeout releases */
    {
        int64_t start = now_ms();
        int progress = 0, stuck = 0;
        for (;;) {
            int64_t now = now_ms();
            int64_t elapsed = now - start;
            if (!stuck && progress < STUCK_AT) {
                progress += 1 + (int)(rand() % 3);
                if (progress > STUCK_AT)
                    progress = STUCK_AT;
                if (progress >= STUCK_AT)
                    stuck = 1;
            }
            draw_frame(&fc, &drm, progress, now);
            drm_flip(&drm);
            if (elapsed >= (int64_t)timeout_sec * 1000)
                break;
            usleep(FRAME_MS * 1000);
        }
    }

    drm_restore(&drm);
    ret = 0;

cleanup:
    if (fc.face)
        FT_Done_Face(fc.face);
    if (fc.lib)
        FT_Done_FreeType(fc.lib);
    drm_cleanup(&drm);
    if (tty_fd >= 0) {
        if (origin_vt > 0 && origin_vt != TARGET_VT) {
            fprintf(stderr, "[tty] 切回桌面 tty%d\n", origin_vt);
            vt_switch_to(tty_fd, origin_vt);
        }
        close(tty_fd);
    }
    fprintf(stderr, ret == 0 ? "[tty] 已释放并恢复桌面\n"
                             : "[tty] 异常退出，已尽力恢复桌面\n");
    return ret;
}
