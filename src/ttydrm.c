/*
 * ttydrm.c — DRM/TTY direct-render mode ("unescapable" prank).
 *
 * Renders the fake Windows update screen straight to the physical display via
 * DRM/KMS, bypassing the desktop entirely — no GTK window, no WebKit, no
 * compositor. Because the desktop is suspended and the display is owned by
 * this process, the victim cannot Alt+Tab or switch windows to escape
 * (works on Wayland too, where a normal window could always be escaped).
 *
 * Requires root (VT ioctls + DRM master).
 *
 * Flow:
 *   1. find the active VT, switch to an idle VT (desktop suspends, releases DRM)
 *   2. open /dev/dri/card* and drmSetMaster()
 *   3. create a dumb framebuffer and draw the Win11-style update UI (FreeType)
 *   4. with 50/50 probability the update either succeeds (progress to 100%%,
 *      waits for a real background apt update if --real-update, then reboots)
 *      or fails (progress freezes at 35%%..42%%, then the embedded bsod from
 *      heyManNice/bsod takes over the screen with an error message)
 *   5. restore CRTC, drop master, switch back to the desktop VT
 *
 * Design canvas 1920x1080, scaled to the real resolution.
 */
#include "ttydrm.h"
#include "bsod_data.h"

#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
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
/* UI drawing (Windows-style update screen on a 1920x1080 canvas)      */
/* ================================================================== */
static void draw_spinner(uint32_t *b, uint32_t w, uint32_t h, uint32_t sp,
                         int cx, int cy, double s, int64_t now)
{
    /* Windows style: 6 dots clustered in one arc, orbiting the center.
       Speed is position-dependent — slowest at the top-left (dots closest,
       yet visibly spaced) and fastest at the bottom-right (dots widest). */
    const int N = 6;
    const double TAU = 6.28318530717958647692;
    const double period = 2200.0;  /* ms per revolution (average) */
    const double speed_amp = 0.55; /* how much speed varies by position */
    const double slow_ang = 0.625 * TAU; /* top-left, 225 deg: slowest */
    const double gap0 = 0.40;      /* average angular gap between dots */
    const double gap_amp = 0.15;   /* gap shrinks at top-left, widens bottom-right */
    double w0 = TAU / period;
    int orbit = (int)(34 * s);
    int dot_r = (int)(4 * s);
    double theta;
    int j;

    if (dot_r < 2)
        dot_r = 2;

    /* non-uniform rotation: slow near top-left, fast near bottom-right */
    theta = w0 * (double)now + speed_amp * sin(w0 * (double)now - 0.125 * TAU);

    for (j = 0; j < N; j++) {
        double gap = gap0 * (1.0 - gap_amp * cos(theta - slow_ang));
        double ang = theta + gap * (double)(j - (N - 1) / 2);
        int dx = (int)(cx + cos(ang) * orbit);
        int dy = (int)(cy + sin(ang) * orbit);
        fb_fill_circle(b, w, h, sp, dx, dy, dot_r, COL_FG, 255);
    }
}

/* return 1 when the environment is a Chinese locale (zh*) */
static int is_chinese_lang(void)
{
    const char *lang = getenv("LANG");
    return lang && strncmp(lang, "zh", 2) == 0;
}

static void draw_frame(FontCtx *fc, DrmCtx *drm, int progress, int64_t now)
{
    uint32_t *b = drm->map_ptr;
    uint32_t W = drm->width, H = drm->height, sp = drm->pitch / 4;
    double s = (double)H / 1080.0;
    int cx = (int)W / 2;
    int sy, ty, ty2;
    int is_zh = is_chinese_lang();
    const char *hint = is_zh ? "请保持计算机打开状态。"
                             : "Please keep your computer on.";
    const char *foot = is_zh ? "计算机可能会重启几次"
                             : "Your PC may restart several times";

    if (s > 2.0)
        s = 2.0;

    fb_clear(b, W, H, sp, COL_BG);

    /* spinner + progress text */
    sy = (int)(H * 0.40);
    draw_spinner(b, W, H, sp, cx, sy, s, now);

    ty = sy + (int)(66 * s);
    {
        char pct[48];
        snprintf(pct, sizeof(pct),
                 is_zh ? "正在进行更新 %d%%" : "Working on updates %d%%",
                 progress);
        draw_text(fc, b, W, H, sp, pct, cx, ty,
                  COL_FG, (unsigned)(24 * s), 1);
    }

    /* hint line */
    ty2 = ty + (int)(40 * s);
    draw_text(fc, b, W, H, sp, hint, cx, ty2,
              COL_FG, (unsigned)(24 * s), 1);

    /* footnote (same size as the middle lines) */
    draw_text(fc, b, W, H, sp, foot, cx,
              (int)(H - 90 * s), COL_FG, (unsigned)(24 * s), 1);
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

/* Fork a fully detached child that runs a real "apt update && apt upgrade"
 * in the background while the fake screen is showing. Returns the child
 * pid, or -1 if not spawned. Logs to windows-update-real.log in the
 * current directory. */
static pid_t spawn_real_update(void)
{
    pid_t pid;

    if (access("/usr/bin/apt-get", X_OK) != 0) {
        fprintf(stderr, "[real-update] 未检测到 apt-get，跳过真实更新\n");
        return -1;
    }

    pid = fork();
    if (pid < 0) {
        perror("[real-update] fork 失败");
        return -1;
    }
    if (pid > 0) {
        fprintf(stderr,
                "[real-update] 已在后台执行 apt update && apt upgrade (pid %d)\n",
                (int)pid);
        return pid;
    }

    /* child: detach from session, silence stdin, log to cwd */
    if (setsid() < 0)
        _exit(1);
    {
        int devnull = open("/dev/null", O_RDONLY);
        int logfd = open("windows-update-real.log",
                         O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (devnull >= 0) {
            dup2(devnull, 0);
            if (devnull > 2)
                close(devnull);
        }
        if (logfd < 0)
            logfd = open("/dev/null", O_WRONLY);
        if (logfd >= 0) {
            dup2(logfd, 1);
            dup2(logfd, 2);
            if (logfd > 2)
                close(logfd);
        }
    }
    setenv("DEBIAN_FRONTEND", "noninteractive", 1);
    execl("/bin/sh", "sh", "-c",
          "apt-get -y update && apt-get -y upgrade", (char *)NULL);
    _exit(127);
}

/* Write the embedded bsod binary to /tmp and exec it, replacing this
 * process. With no_reboot the bsod restores the desktop instead of
 * rebooting. Returns -1 if the bsod could not be started. */
static int launch_bsod(const char *reason, int no_reboot)
{
    const char *path = "/tmp/.windows-update-bsod";
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    size_t off = 0;

    if (fd < 0)
        return -1;
    while (off < bsod_bin_len) {
        ssize_t n = write(fd, bsod_bin + off, bsod_bin_len - off);
        if (n < 0) {
            close(fd);
            unlink(path);
            return -1;
        }
        off += (size_t)n;
    }
    close(fd);
    chmod(path, 0755);

    fprintf(stderr, "[tty] 更新失败，启动蓝屏：%s\n", reason);
    if (no_reboot)
        execl(path, "bsod", "--show", reason, "--restore", (char *)NULL);
    else
        execl(path, "bsod", "--show", reason, (char *)NULL);
    perror("[tty] exec bsod 失败");
    return -1;
}

/* Reboot the machine (does not return on success). */
static void do_reboot(void)
{
    fprintf(stderr, "[tty] 更新完成，2 秒后重启系统...\n");
    sleep(2);
    execl("/sbin/reboot", "reboot", (char *)NULL);
    execl("/usr/sbin/reboot", "reboot", (char *)NULL);
    perror("[tty] reboot 失败，直接退出");
}

int fake_update_ttydrm_run(unsigned int timeout_sec, int no_reboot)
{
    DrmCtx drm;
    FontCtx fc;
    int tty_fd = -1, origin_vt = -1, ret = 1;
    pid_t apt_pid = -1;
    int success = 1; /* 1 = success (to 100%, then reboot), 0 = failure (BSOD) */

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

    /* decide the outcome: 50/50 by default, overridable via WINDOWS_UPDATE_MODE */
    {
        const char *mode = getenv("WINDOWS_UPDATE_MODE");
        if (mode && strcmp(mode, "success") == 0)
            success = 1;
        else if (mode && strcmp(mode, "failure") == 0)
            success = 0;
        else
            success = (rand() % 2) == 0;
    }
    fprintf(stderr, success ? "[tty] 本次为：更新成功\n"
                            : "[tty] 本次为：更新失败（超时后交给蓝屏）\n");

    if (success)
        apt_pid = spawn_real_update(); /* real apt update only on success */

    /* animation */
    {
        int64_t start = now_ms();
        int progress = 0, stuck_at = 0, done = 0;
        int64_t hold = 0;
        const int64_t min_ms = (int64_t)timeout_sec * 1000;

        if (!success)
            stuck_at = 35 + (int)(rand() % 8); /* freeze at 35%%..42%% */

        for (;;) {
            int64_t now = now_ms();
            int64_t elapsed = now - start;

            if (success) {
                /* fast at first, then slower: asymptote to 99%% */
                double p = 100.0 * (1.0 - exp(-(double)elapsed / 7000.0));
                progress = (int)p;
                if (progress > 99)
                    progress = 99;
                /* only reach 100%% after the real apt update finished and
                 * the minimum runtime elapsed */
                if (!done) {
                    int apt_done = 1;
                    if (apt_pid > 0 &&
                        waitpid(apt_pid, NULL, WNOHANG) != apt_pid)
                        apt_done = 0;
                    if (apt_done && elapsed >= min_ms) {
                        progress = 100;
                        done = 1;
                        hold = now;
                    }
                }
                draw_frame(&fc, &drm, progress, now);
                drm_flip(&drm);
                if (done && now - hold >= 2000)
                    break; /* show 100%% for 2s, then finish */
            } else {
                /* failure: progress climbs from 0% up to the random cap
                 * (35%..42%), evenly over the minimum runtime, then sticks */
                progress = (int)((double)stuck_at *
                                 (double)elapsed / (double)min_ms);
                if (progress > stuck_at)
                    progress = stuck_at;
                draw_frame(&fc, &drm, progress, now);
                drm_flip(&drm);
                if (elapsed >= min_ms)
                    break; /* hand over to bsod after the minimum time */
            }
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

    /* final action: reboot after a successful update, or hand the failure
     * over to the embedded bsod (which replaces this process) */
    if (success) {
        if (no_reboot) {
            fprintf(stderr, "[tty] 更新完成（--no-reboot，不重启）\n");
        } else {
            do_reboot(); /* does not return on success */
        }
    } else {
        const char *reason = is_chinese_lang()
                                 ? "Linux 在更新时出错"
                                 : "An error occurred while updating Linux";
        if (launch_bsod(reason, no_reboot) < 0)
            fprintf(stderr, "[tty] 启动蓝屏失败，直接退出\n");
    }

    fprintf(stderr, ret == 0 ? "[tty] 已释放并恢复桌面\n"
                             : "[tty] 异常退出，已尽力恢复桌面\n");
    return ret;
}
