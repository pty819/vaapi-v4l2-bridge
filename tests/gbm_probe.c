/*
 * gbm_probe — platform capability probe for GBM-backed display surfaces.
 *
 * Platform truth (verified 2026-09-02, RK3588 panthor / Mesa 26.0.8):
 *   - the render node refuses every multiplanar YUV gbm bo (NV12/P010/Y210
 *     → EINVAL under all flag combinations)
 *   - single-plane linear R8/R16/GR88/GR1616 bos work (modifier LINEAR,
 *     tight stride, CPU map round-trip exact)
 *   - Mesa per-plane GR88 dmabuf import samples (0,0) — never hand clients
 *     a GR88 single-plane image on this box
 *   - the classic single-object NV12 shape (ONE linear bo, Y at offset 0,
 *     UV at offset stride*h, both planes on one fd) imports bit-exactly
 *     through eglCreateImage at 720p/1080p/4K/odd sizes
 *
 * That last shape is exactly what Chromium requires
 * (VaapiWrapper::ExportVASurfaceAsNativePixmapDmaBuf rejects
 * num_objects != 1), so this probe proves the full chain the bridge will
 * use: bo alloc → CPU map write → per-plane fd export → NV12 EGL import →
 * GPU sampling of both planes.
 *
 * Usage: gbm_probe <width> <height>   (exit 0 = platform ready)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <gbm.h>
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

/* Extension constants that may be missing from older headers */
#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D6
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

typedef void (GL_APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, void *);

#define CHECK(cond, msg) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL: %s (errno=%s egl=0x%x gl=0x%x)\n", msg, \
                strerror(errno), eglGetError(), glGetError()); \
        exit(1); \
    } \
    printf("ok: %s\n", msg); \
} while (0)

static const char *vsrc =
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main() { uv = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);\n"
    "  gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fsrc =
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "uniform sampler2D texY;\n"
    "void main() { gl_FragColor = vec4(texture2D(texY, uv).rgb, 1.0); }\n";

static GLuint compile(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    GLint ok = 0;
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(sh, sizeof(log), NULL, log);
        fprintf(stderr, "shader compile: %s\n", log);
        exit(1);
    }
    return sh;
}

static struct gbm_bo *create_any_flags(struct gbm_device *gdev, uint32_t fmt,
                                       uint32_t w, uint32_t h)
{
    const uint32_t uses[] = { GBM_BO_USE_LINEAR, GBM_BO_USE_RENDERING,
                              GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING,
                              GBM_BO_USE_WRITE, 0 };
    struct gbm_bo *bo = NULL;
    for (unsigned i = 0; i < sizeof(uses) / sizeof(uses[0]) && !bo; i++)
        bo = gbm_bo_create(gdev, w, h, fmt, uses[i]);
    return bo;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc != 3) {
        fprintf(stderr, "usage: %s <width> <height>\n", argv[0]);
        return 2;
    }
    int w = atoi(argv[1]);
    int h = atoi(argv[2]);

    /* ---- GBM device and format survey (informational) ---- */
    int drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    CHECK(drm_fd >= 0, "open /dev/dri/renderD128");
    struct gbm_device *gdev = gbm_create_device(drm_fd);
    CHECK(gdev != NULL, "gbm_create_device");

    const uint32_t sfmts[] = { GBM_FORMAT_NV12, GBM_FORMAT_R8, GBM_FORMAT_R16,
                               GBM_FORMAT_GR88, GBM_FORMAT_GR1616 };
    const char *snames[] = { "NV12", "R8", "R16", "GR88", "GR1616" };
    for (unsigned i = 0; i < sizeof(sfmts) / sizeof(sfmts[0]); i++) {
        struct gbm_bo *t = create_any_flags(gdev, sfmts[i], w, h);
        printf("survey %-7s: create=%s\n", snames[i], t ? "YES" : "no");
        if (t)
            gbm_bo_destroy(t);
    }

    /* ---- the shipping shape: ONE linear bo holding both planes ---- */
    uint32_t rows = h + (h + 1) / 2;
    struct gbm_bo *bo = create_any_flags(gdev, GBM_FORMAT_R8, w, rows);
    CHECK(bo != NULL, "gbm_bo_create R8 (w x h*3/2 rows)");
    uint32_t sA = gbm_bo_get_stride_for_plane(bo, 0);
    uint64_t mod = gbm_bo_get_modifier(bo);
    printf("bo: stride=%u modifier=0x%llx\n", sA, (unsigned long long)mod);
    CHECK(sA >= (uint32_t)w, "sane stride");
    CHECK(mod == 0, "LINEAR modifier");

    int fdA = gbm_bo_get_fd_for_plane(bo, 0);
    CHECK(fdA >= 0, "gbm_bo_get_fd_for_plane");

    /* CPU map, write two-band luma + neutral chroma, read back and verify */
    void *map_data = NULL;
    uint32_t mA = 0;
    uint8_t *pA = gbm_bo_map(bo, 0, 0, w, rows, GBM_BO_TRANSFER_WRITE,
                             &mA, &map_data);
    CHECK(pA != NULL && mA >= sA, "gbm_bo_map WRITE");
    memset(pA, 64, (size_t)mA * h);                          /* dark top  */
    memset(pA + (size_t)mA * (h / 2), 220, (size_t)mA * (h - h / 2));
    memset(pA + (size_t)mA * h, 128, (size_t)mA * (rows - h));
    gbm_bo_unmap(bo, map_data);

    void *v2 = NULL;
    uint8_t *rA = gbm_bo_map(bo, 0, 0, w, rows, GBM_BO_TRANSFER_READ, &mA, &v2);
    CHECK(rA != NULL, "gbm_bo_map READ");
    CHECK(rA[0] == 64 && rA[(size_t)mA * (h - 1) + w / 2] == 220 &&
          rA[(size_t)mA * h + 1] == 128, "CPU map round-trip (Y bands + UV)");
    gbm_bo_unmap(bo, v2);

    /* ---- EGL: import the bo as one NV12 image, sample both planes ---- */
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_plat_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
            "eglGetPlatformDisplayEXT");
    PFNEGLCREATEIMAGEKHRPROC create_img =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC img2tex =
        (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
            "glEGLImageTargetTexture2DOES");
    CHECK(get_plat_dpy != NULL && create_img != NULL && img2tex != NULL,
          "resolve EGL ext procs");
    eglBindAPI(EGL_OPENGL_ES_API);
    EGLDisplay dpy = get_plat_dpy(EGL_PLATFORM_GBM_KHR, (void *)gdev, NULL);
    CHECK(dpy != EGL_NO_DISPLAY, "eglGetPlatformDisplayEXT(GBM)");
    CHECK(eglInitialize(dpy, NULL, NULL), "eglInitialize");

    EGLint cfgat[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                       EGL_NONE };
    EGLConfig cfg;
    EGLint n = 0;
    CHECK(eglChooseConfig(dpy, cfgat, &cfg, 1, &n) && n >= 1,
          "eglChooseConfig");
    EGLint cattribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cattribs);
    CHECK(ctx != EGL_NO_CONTEXT, "eglCreateContext (ES3)");
    CHECK(eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx),
          "eglMakeCurrent (surfaceless)");

    EGLint atNV[] = {
        EGL_WIDTH, w, EGL_HEIGHT, h,
        EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_NV12,
        EGL_DMA_BUF_PLANE0_FD_EXT, fdA,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)sA,
        EGL_DMA_BUF_PLANE1_FD_EXT, fdA,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT, (EGLint)(sA * h),
        EGL_DMA_BUF_PLANE1_PITCH_EXT, (EGLint)sA,
        EGL_NONE,
    };
    EGLImage imgNV = create_img(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                                (EGLClientBuffer)NULL, atNV);
    CHECK(imgNV != EGL_NO_IMAGE_KHR, "eglCreateImageKHR NV12 single object");

    GLuint prog = glCreateProgram();
    glAttachShader(prog, compile(GL_VERTEX_SHADER, vsrc));
    glAttachShader(prog, compile(GL_FRAGMENT_SHADER, fsrc));
    glLinkProgram(prog);
    GLint lk = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &lk);
    CHECK(lk, "program link");

    GLuint fbo, rtex, tex;
    glGenFramebuffers(1, &fbo);
    glGenTextures(1, &rtex);
    glBindTexture(GL_TEXTURE_2D, rtex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, rtex, 0);
    CHECK(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE,
          "FBO complete");

    glViewport(0, 0, w, h);
    glUseProgram(prog);
    glUniform1i(glGetUniformLocation(prog, "texY"), 0);
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    img2tex(GL_TEXTURE_2D, imgNV);
    CHECK(glGetError() == GL_NO_ERROR, "EGLImage -> texture import");
    GLint pos = glGetAttribLocation(prog, "pos");
    GLfloat quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };
    glEnableVertexAttribArray(pos);
    glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 0, quad);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    /* Mesa NV12 textures expose raw YUV in .rgb */
    unsigned char top[3] = { 9, 9, 9 }, bottom[3] = { 9, 9, 9 };
    glReadPixels(w / 2, h - 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, top);
    glReadPixels(w / 2, 4, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, bottom);
    printf("readback: top=(%d,%d,%d) bottom=(%d,%d,%d) "
           "[expect (64,128,128)/(220,128,128)]\n",
           top[0], top[1], top[2], bottom[0], bottom[1], bottom[2]);
    CHECK(top[0] == 64 && top[1] == 128 && top[2] == 128 &&
          bottom[0] == 220 && bottom[1] == 128 && bottom[2] == 128,
          "single-object NV12 import is bit-exact (Y and UV)");

    printf("PASS: gbm single-object NV12 bo: alloc + CPU map + fd export + "
           "EGL import + GPU sampling all bit-exact at %dx%d\n", w, h);
    return 0;
}
