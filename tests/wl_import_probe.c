/*
 * wl_import_probe — replicate Chrome's dmabuf import on a WAYLAND-platform
 * EGL display (what ANGLE uses under ozone-wayland), versus the GBM-platform
 * display that gbm_probe uses. Isolates whether Mesa's Wayland platform
 * accepts the bridge's single-object NV12 export.
 *
 * Allocates the shipping-shape bo (R8 w x h*3/2, Y@0 UV@stride*h), then
 * tries on the Wayland display:
 *   1. single NV12 image (both planes, one fd)
 *   2. per-plane Y (R8) and UV (GR88) like NativePixmapEGLBinding
 * both with and without LINEAR modifier attrs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <gbm.h>
#include <wayland-client.h>
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D6
#endif
#ifndef EGL_PLATFORM_WAYLAND_KHR
#define EGL_PLATFORM_WAYLAND_KHR 0x31D8
#endif
#ifndef EGL_PLATFORM_SURFACELESS_MESA
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#endif
#ifndef EGL_LINUX_DMA_BUF_EXT
#define EGL_LINUX_DMA_BUF_EXT 0x3270
#endif
#ifndef EGL_DMA_BUF_PLANE0_FD_EXT
#define EGL_DMA_BUF_PLANE0_FD_EXT 0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT 0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT 0x3274
#define EGL_DMA_BUF_PLANE1_FD_EXT 0x3275
#define EGL_DMA_BUF_PLANE1_OFFSET_EXT 0x3276
#define EGL_DMA_BUF_PLANE1_PITCH_EXT 0x3277
#endif
#ifndef EGL_YUV_COLOR_SPACE_HINT_EXT
#define EGL_YUV_COLOR_SPACE_HINT_EXT 0x327B
#define EGL_SAMPLE_RANGE_HINT_EXT 0x327C
#define EGL_ITU_REC601_EXT 0x327F
#define EGL_YUV_NARROW_RANGE_EXT 0x3283
#endif
#ifndef EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT 0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT 0x3444
#define EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT 0x3445
#define EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT 0x3446
#endif

typedef void (GL_APIENTRYP GLTEX2DOESPROC)(GLenum, void *);

static PFNEGLCREATEIMAGEKHRPROC g_create_img;
static GLTEX2DOESPROC g_img2tex;

#define TRY(name, at) do { \
    EGLImage im = g_create_img(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, \
                               (EGLClientBuffer)NULL, (at)); \
    printf("%-42s %s (egl=0x%x)\n", (name), \
           im != EGL_NO_IMAGE_KHR ? "OK" : "FAIL", eglGetError()); \
    if (im != EGL_NO_IMAGE_KHR) { \
        GLuint t; glGenTextures(1, &t); \
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, t); \
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST); \
        g_img2tex(GL_TEXTURE_2D, im); \
        printf("%-42s   tex-import glerr=0x%x\n", (name), glGetError()); \
        glDeleteTextures(1, &t); \
    } \
} while (0)

int main(int argc, char **argv)
{
    int w = argc > 2 ? atoi(argv[1]) : 1920;
    int h = argc > 2 ? atoi(argv[2]) : 1080;
    setvbuf(stdout, NULL, _IONBF, 0);

    int drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0) { perror("render node"); return 2; }
    struct gbm_device *gdev = gbm_create_device(drm_fd);
    if (!gdev) { fprintf(stderr, "gbm_create_device failed\n"); return 2; }

    uint32_t rows = h + (h + 1) / 2;
    struct gbm_bo *bo = gbm_bo_create(gdev, w, rows, GBM_FORMAT_R8,
                                      GBM_BO_USE_LINEAR);
    if (!bo) { fprintf(stderr, "bo create failed\n"); return 2; }
    uint32_t s = gbm_bo_get_stride_for_plane(bo, 0);
    void *md = NULL; uint32_t ms = 0;
    uint8_t *p = gbm_bo_map(bo, 0, 0, w, rows, GBM_BO_TRANSFER_WRITE, &ms, &md);
    if (!p) { fprintf(stderr, "map failed\n"); return 2; }
    memset(p, 100, (size_t)ms * rows);
    gbm_bo_unmap(bo, md);
    int fd = gbm_bo_get_fd_for_plane(bo, 0);
    if (fd < 0) { fprintf(stderr, "fd export failed\n"); return 2; }
    printf("bo %dx%d stride=%u fd=%d len=%ld\n", w, rows, s, fd,
           (long)lseek(fd, 0, SEEK_END));

    struct wl_display *wd = wl_display_connect(NULL);
    if (!wd) { fprintf(stderr, "wl_display_connect failed\n"); return 2; }
    g_create_img = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    g_img2tex = (GLTEX2DOESPROC)eglGetProcAddress(
        "glEGLImageTargetTexture2DOES");
    if (!g_create_img || !g_img2tex) { fprintf(stderr, "procs\n"); return 2; }

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
            "eglGetPlatformDisplayEXT");

    for (int plat = 0; plat < 3; plat++) {
        const char *pname = plat == 0 ? "WAYLAND" : (plat == 1 ? "GBM" : "SURFACELESS");
        EGLDisplay dpy = plat == 0
            ? get_dpy(EGL_PLATFORM_WAYLAND_KHR, (void *)wd, NULL)
            : (plat == 1 ? get_dpy(EGL_PLATFORM_GBM_KHR, (void *)gdev, NULL)
                         : get_dpy(EGL_PLATFORM_SURFACELESS_MESA, (void *)0, NULL));
        if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
            printf("=== %s: display init FAILED (0x%x)\n", pname, eglGetError());
            continue;
        }
        eglBindAPI(EGL_OPENGL_ES_API);
        EGLint cfgat[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                           EGL_NONE };
        EGLConfig cfg; EGLint n = 0;
        EGLContext ctx = EGL_NO_CONTEXT;
        if (eglChooseConfig(dpy, cfgat, &cfg, 1, &n) && n >= 1) {
            EGLint ca[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
            ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ca);
        }
        printf("=== %s (ctx=%s) exts:%s dmabuf_import=%s dmabuf_mod=%s\n",
               pname, ctx != EGL_NO_CONTEXT ? "ok" : "FAIL",
               strstr(eglQueryString(dpy, EGL_EXTENSIONS),
                      "EGL_EXT_image_dma_buf_import") ? "yes" : "no",
               strstr(eglQueryString(dpy, EGL_EXTENSIONS),
                      "EGL_EXT_image_dma_buf_import") ? "yes" : "no",
               strstr(eglQueryString(dpy, EGL_EXTENSIONS),
                      "EGL_EXT_image_dma_buf_import_modifiers") ? "yes" : "no");
        if (ctx == EGL_NO_CONTEXT)
            continue;
        if (!eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
            printf("   makeCurrent failed 0x%x\n", eglGetError());
            continue;
        }

        EGLint nv12[] = {
            EGL_WIDTH, w, EGL_HEIGHT, h,
            EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_NV12,
            EGL_DMA_BUF_PLANE0_FD_EXT, fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)s,
            EGL_DMA_BUF_PLANE1_FD_EXT, fd,
            EGL_DMA_BUF_PLANE1_OFFSET_EXT, (EGLint)(s * h),
            EGL_DMA_BUF_PLANE1_PITCH_EXT, (EGLint)s,
            EGL_NONE,
        };
        TRY("NV12 single-object (no modifier)", nv12);

        EGLint nv12m[] = {
            EGL_WIDTH, w, EGL_HEIGHT, h,
            EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_NV12,
            EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC601_EXT,
            EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT,
            EGL_DMA_BUF_PLANE0_FD_EXT, fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)s,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0,
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
            EGL_DMA_BUF_PLANE1_FD_EXT, fd,
            EGL_DMA_BUF_PLANE1_OFFSET_EXT, (EGLint)(s * h),
            EGL_DMA_BUF_PLANE1_PITCH_EXT, (EGLint)s,
            EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT, 0,
            EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT, 0,
            EGL_NONE,
        };
        TRY("NV12 chrome-style (hints+modifier)", nv12m);

        EGLint yplane[] = {
            EGL_WIDTH, w, EGL_HEIGHT, h,
            EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_R8,
            EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC601_EXT,
            EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT,
            EGL_DMA_BUF_PLANE0_FD_EXT, fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)s,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0,
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
            EGL_NONE,
        };
        TRY("Y plane R8 chrome-style", yplane);

        EGLint uvplane[] = {
            EGL_WIDTH, w / 2, EGL_HEIGHT, h / 2,
            EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_GR88,
            EGL_YUV_COLOR_SPACE_HINT_EXT, EGL_ITU_REC601_EXT,
            EGL_SAMPLE_RANGE_HINT_EXT, EGL_YUV_NARROW_RANGE_EXT,
            EGL_DMA_BUF_PLANE0_FD_EXT, fd,
            EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)(s * h),
            EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)s,
            EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0,
            EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0,
            EGL_NONE,
        };
        TRY("UV plane GR88 chrome-style", uvplane);
    }
    return 0;
}
