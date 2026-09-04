/*
 * va_expbuf_hold.c — DPB-live EXPBUF hold.
 *
 * Decode frame 0 onto surface 0, vaGetImage a reference, vaExportSurfaceHandle
 * and keep the dma-buf fd + an EGLImage alive. Decode frames 1..N onto
 * surfaces 1,2,3 cycling (never re-BeginPicture on surface 0). Then sample
 * the held fd through a fresh EGL R8 import and require byte-exact Y vs the
 * saved GetImage. If the capture index was recycled/stomped, pixels change.
 *
 * usage: va_expbuf_hold <render-node> <stream.h264>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>
#include <va/va_drmcommon.h>
#include <gbm.h>
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>

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
#endif

#define W 1280
#define H 720
#define N_SURF 4
#define N_FRAMES 8

typedef void (GL_APIENTRYP PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)(GLenum, void *);

static void die(const char *msg, VAStatus vas)
{
    fprintf(stderr, "va_expbuf_hold: %s (va=%d %s)\n", msg, vas,
            vaErrorStr(vas));
    exit(1);
}

#define CHECK(x) do { VAStatus _s = (x); if (_s != VA_STATUS_SUCCESS) die(#x, _s); } while (0)

static const char *vsrc =
    "attribute vec2 pos;\n"
    "varying vec2 uv;\n"
    "void main() { uv = vec2(pos.x * 0.5 + 0.5, 0.5 - pos.y * 0.5);\n"
    "  gl_Position = vec4(pos, 0.0, 1.0); }\n";
static const char *fsrc =
    "precision mediump float;\n"
    "varying vec2 uv;\n"
    "uniform sampler2D texY;\n"
    "void main() { gl_FragColor = vec4(texture2D(texY, uv).r); }\n";

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

static PFNEGLCREATEIMAGEKHRPROC g_create_img;
static PFNEGLDESTROYIMAGEKHRPROC g_destroy_img;
static PFNGLEGLIMAGETARGETTEXTURE2DOESPROC g_img2tex;
static GLuint g_prog, g_fbo, g_rtex;
static const GLfloat g_quad[] = { -1, -1, 1, -1, -1, 1, 1, 1 };

static void egl_setup(const char *node)
{
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_plat_dpy;
    int fd = open(node, O_RDWR | O_CLOEXEC);
    struct gbm_device *gdev;
    EGLDisplay dpy;
    EGLConfig cfg;
    EGLint n = 0;
    EGLContext ctx;
    EGLint cfgat[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR,
                       EGL_NONE };
    EGLint cattribs[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    GLint pos;

    if (fd < 0) { perror("open render node (egl)"); exit(2); }
    gdev = gbm_create_device(fd);
    if (!gdev) { fprintf(stderr, "gbm_create_device failed\n"); exit(2); }

    get_plat_dpy = (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress(
        "eglGetPlatformDisplayEXT");
    g_create_img = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress(
        "eglCreateImageKHR");
    g_img2tex = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress(
        "glEGLImageTargetTexture2DOES");
    g_destroy_img = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress(
        "eglDestroyImageKHR");
    if (!get_plat_dpy || !g_create_img || !g_img2tex || !g_destroy_img) {
        fprintf(stderr, "EGL ext procs missing\n");
        exit(2);
    }
    eglBindAPI(EGL_OPENGL_ES_API);
    dpy = get_plat_dpy(EGL_PLATFORM_GBM_KHR, (void *)gdev, NULL);
    if (dpy == EGL_NO_DISPLAY || !eglInitialize(dpy, NULL, NULL)) {
        fprintf(stderr, "EGL display init failed\n");
        exit(2);
    }
    if (!eglChooseConfig(dpy, cfgat, &cfg, 1, &n) || n < 1) {
        fprintf(stderr, "eglChooseConfig failed\n");
        exit(2);
    }
    ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, cattribs);
    if (ctx == EGL_NO_CONTEXT ||
        !eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx)) {
        fprintf(stderr, "EGL context failed (0x%x)\n", eglGetError());
        exit(2);
    }

    g_prog = glCreateProgram();
    glAttachShader(g_prog, compile(GL_VERTEX_SHADER, vsrc));
    glAttachShader(g_prog, compile(GL_FRAGMENT_SHADER, fsrc));
    glLinkProgram(g_prog);
    GLint lk = 0;
    glGetProgramiv(g_prog, GL_LINK_STATUS, &lk);
    if (!lk) { fprintf(stderr, "program link failed\n"); exit(2); }

    glGenFramebuffers(1, &g_fbo);
    glGenTextures(1, &g_rtex);
    glBindTexture(GL_TEXTURE_2D, g_rtex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, W, H, 0, GL_RGBA,
                 GL_UNSIGNED_BYTE, NULL);
    glBindFramebuffer(GL_FRAMEBUFFER, g_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                           GL_TEXTURE_2D, g_rtex, 0);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "FBO incomplete\n");
        exit(2);
    }
    glUseProgram(g_prog);
    glUniform1i(glGetUniformLocation(g_prog, "texY"), 0);
    pos = glGetAttribLocation(g_prog, "pos");
    glEnableVertexAttribArray(pos);
    glVertexAttribPointer(pos, 2, GL_FLOAT, GL_FALSE, 0, (void *)g_quad);
}

static int read_r8_view(int fd, uint32_t offset, uint32_t pitch,
                        int w, int h, uint8_t *out)
{
    EGLint at[] = {
        EGL_WIDTH, w, EGL_HEIGHT, h,
        EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)offset,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch,
        EGL_NONE,
    };
    EGLImage img = g_create_img(eglGetCurrentDisplay(), EGL_NO_CONTEXT,
                                EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL,
                                at);
    GLuint tex;
    uint32_t *rgba;

    if (img == EGL_NO_IMAGE_KHR)
        return -1;
    glGenTextures(1, &tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g_img2tex(GL_TEXTURE_2D, img);
    glViewport(0, 0, w, h);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);

    rgba = malloc((size_t)w * h * 4);
    if (!rgba) { glDeleteTextures(1, &tex); g_destroy_img(eglGetCurrentDisplay(), img); return -1; }
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, rgba);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            out[(size_t)y * w + x] =
                (uint8_t)(rgba[(size_t)(h - 1 - y) * w + x] & 0xff);
    free(rgba);
    glDeleteTextures(1, &tex);
    g_destroy_img(eglGetCurrentDisplay(), img);
    return glGetError() == GL_NO_ERROR ? 0 : -1;
}

static void decode_one(VADisplay dpy, VAContextID ctx, VASurfaceID s,
                       VAPictureParameterBufferH264 *pp,
                       VAIQMatrixBufferH264 *iq,
                       unsigned char *slice, int slice_size,
                       int is_idr, VASurfaceID ref, unsigned frame_num)
{
    VASliceParameterBufferH264 sp;
    VABufferID b_pp, b_iq, b_sp, b_sd;
    int i;

    memset(&sp, 0, sizeof(sp));
    sp.slice_data_size = slice_size;
    sp.slice_data_flag = VA_SLICE_DATA_FLAG_ALL;
    sp.first_mb_in_slice = 0;
    sp.slice_type = is_idr ? 7 : 0;
    pp->CurrPic.picture_id = s;
    pp->CurrPic.frame_idx = frame_num;
    pp->CurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    pp->frame_num = frame_num;
    pp->pic_fields.bits.reference_pic_flag = 1;
    for (i = 0; i < 16; i++) {
        pp->ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        pp->ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    if (is_idr || ref == VA_INVALID_SURFACE) {
        pp->num_ref_frames = 0;
    } else {
        pp->num_ref_frames = 1;
        pp->ReferenceFrames[0].picture_id = ref;
        pp->ReferenceFrames[0].frame_idx = frame_num ? frame_num - 1 : 0;
        pp->ReferenceFrames[0].flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    }

    CHECK(vaCreateBuffer(dpy, ctx, VAPictureParameterBufferType,
                         sizeof(*pp), 1, pp, &b_pp));
    CHECK(vaCreateBuffer(dpy, ctx, VAIQMatrixBufferType,
                         sizeof(*iq), 1, iq, &b_iq));
    CHECK(vaCreateBuffer(dpy, ctx, VASliceParameterBufferType,
                         sizeof(sp), 1, &sp, &b_sp));
    CHECK(vaCreateBuffer(dpy, ctx, VASliceDataBufferType,
                         slice_size, 1, slice, &b_sd));
    CHECK(vaBeginPicture(dpy, ctx, s));
    VABufferID r1[2] = { b_pp, b_iq };
    CHECK(vaRenderPicture(dpy, ctx, r1, 2));
    VABufferID r2[2] = { b_sp, b_sd };
    CHECK(vaRenderPicture(dpy, ctx, r2, 2));
    CHECK(vaEndPicture(dpy, ctx));
    CHECK(vaSyncSurface(dpy, s));
    vaDestroyBuffer(dpy, b_pp);
    vaDestroyBuffer(dpy, b_iq);
    vaDestroyBuffer(dpy, b_sp);
    vaDestroyBuffer(dpy, b_sd);
}

static void grab_ref(VADisplay dpy, VASurfaceID s, VAImageFormat *fmt,
                     uint8_t *ref)
{
    VAImage img;
    void *p = NULL;
    uint32_t rpitch, uvp;
    const uint8_t *uvsrc;

    CHECK(vaCreateImage(dpy, fmt, W, H, &img));
    CHECK(vaGetImage(dpy, s, 0, 0, W, H, img.image_id));
    CHECK(vaMapBuffer(dpy, img.buf, &p));
    rpitch = img.pitches[0];
    for (int y = 0; y < H; y++)
        memcpy(ref + (size_t)y * W, (uint8_t *)p + (size_t)y * rpitch, W);
    uvp = img.pitches[1] ? img.pitches[1] : img.pitches[0];
    uvsrc = (uint8_t *)p + img.offsets[1];
    for (int y = 0; y < H / 2; y++)
        memcpy(ref + (size_t)W * H + (size_t)y * W,
               uvsrc + (size_t)y * uvp, W);
    CHECK(vaUnmapBuffer(dpy, img.buf));
    CHECK(vaDestroyImage(dpy, img.image_id));
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s <render-node> <stream.h264>\n", argv[0]);
        return 2;
    }

    FILE *f = fopen(argv[2], "rb");
    if (!f) { perror("open stream"); return 2; }
    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *es = malloc(len);
    if (!es || fread(es, 1, len, f) != (size_t)len) { perror("read"); return 2; }
    fclose(f);

    unsigned char *slices[N_FRAMES];
    int slice_sizes[N_FRAMES], is_idr[N_FRAMES], n_slices = 0;
    for (long i = 0; i + 3 < len && n_slices < N_FRAMES; ) {
        if (es[i] == 0 && es[i + 1] == 0 && es[i + 2] == 1) {
            unsigned char type = es[i + 3] & 0x1f;
            long j = i + 3;
            while (j + 3 < len && !(es[j] == 0 && es[j + 1] == 0 && es[j + 2] == 1))
                j++;
            if (type == 1 || type == 5) {
                slices[n_slices] = es + i + 3;
                slice_sizes[n_slices] = (int)(j - i - 3);
                is_idr[n_slices] = (type == 5);
                n_slices++;
            }
            i = j;
        } else {
            i++;
        }
    }
    if (n_slices < 2) {
        fprintf(stderr, "need >=2 slice NALs, got %d\n", n_slices);
        return 2;
    }

    int drm_fd = open(argv[1], O_RDWR);
    if (drm_fd < 0) { perror("open render node"); return 2; }
    VADisplay dpy = vaGetDisplayDRM(drm_fd);
    if (!dpy) { fprintf(stderr, "vaGetDisplayDRM failed\n"); return 2; }
    int major, minor;
    CHECK(vaInitialize(dpy, &major, &minor));

    VAConfigAttrib rt = { .type = VAConfigAttribRTFormat,
                          .value = VA_RT_FORMAT_YUV420 };
    VAConfigID cfg;
    CHECK(vaCreateConfig(dpy, VAProfileH264High, VAEntrypointVLD, &rt, 1, &cfg));

    VASurfaceAttrib sattr = {
        .type = VASurfaceAttribPixelFormat,
        .flags = VA_SURFACE_ATTRIB_SETTABLE,
        .value = { VAGenericValueTypeInteger, .value = { .i = VA_FOURCC_NV12 } },
    };
    VASurfaceID surf[N_SURF];
    CHECK(vaCreateSurfaces(dpy, rt.value, W, H, surf, N_SURF, &sattr, 1));

    VAContextID ctx;
    CHECK(vaCreateContext(dpy, cfg, W, H, VA_PROGRESSIVE, surf, N_SURF, &ctx));

    VAPictureParameterBufferH264 pp;
    memset(&pp, 0, sizeof(pp));
    pp.CurrPic.picture_id = VA_INVALID_SURFACE;
    pp.CurrPic.frame_idx = 0;
    pp.CurrPic.flags = VA_PICTURE_H264_SHORT_TERM_REFERENCE;
    for (int i = 0; i < 16; i++) {
        pp.ReferenceFrames[i].picture_id = VA_INVALID_SURFACE;
        pp.ReferenceFrames[i].flags = VA_PICTURE_H264_INVALID;
    }
    pp.picture_width_in_mbs_minus1 = W / 16 - 1;
    pp.picture_height_in_mbs_minus1 = H / 16 - 1;
    pp.num_ref_frames = 0;
    pp.seq_fields.bits.chroma_format_idc = 1;
    pp.seq_fields.bits.frame_mbs_only_flag = 1;
    pp.seq_fields.bits.direct_8x8_inference_flag = 1;
    pp.seq_fields.bits.log2_max_frame_num_minus4 = 0;
    pp.seq_fields.bits.pic_order_cnt_type = 2;
    pp.seq_fields.bits.log2_max_pic_order_cnt_lsb_minus4 = 0;
    pp.pic_init_qp_minus26 = -3;
    pp.pic_fields.bits.deblocking_filter_control_present_flag = 1;
    pp.pic_fields.bits.reference_pic_flag = 1;
    pp.frame_num = 0;

    VAIQMatrixBufferH264 iq;
    memset(&iq, 0, sizeof(iq));
    for (int i = 0; i < 6; i++)
        for (int j = 0; j < 16; j++)
            iq.ScalingList4x4[i][j] = 16;
    for (int i = 0; i < 2; i++)
        for (int j = 0; j < 64; j++)
            iq.ScalingList8x8[i][j] = 64;

    VAImageFormat fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.fourcc = VA_FOURCC_NV12;

    egl_setup(argv[1]);
    uint8_t *ybuf = malloc((size_t)W * H);
    uint8_t *ref = malloc((size_t)W * H * 2);
    if (!ybuf || !ref) { fprintf(stderr, "oom\n"); return 2; }

    /* Frame 0 → surface 0. Never re-BeginPicture on surf[0] after this. */
    decode_one(dpy, ctx, surf[0], &pp, &iq, slices[0], slice_sizes[0],
               is_idr[0], VA_INVALID_SURFACE, 0);
    grab_ref(dpy, surf[0], &fmt, ref);

    VADRMPRIMESurfaceDescriptor desc;
    VAStatus st = vaExportSurfaceHandle(dpy, surf[0],
                                        VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2,
                                        VA_EXPORT_SURFACE_COMPOSED_LAYERS,
                                        &desc);
    if (st != VA_STATUS_SUCCESS)
        die("vaExportSurfaceHandle", st);
    if (desc.num_objects != 1 || desc.fourcc != VA_FOURCC_NV12) {
        fprintf(stderr, "bad descriptor: objects=%u fourcc=%u\n",
                desc.num_objects, desc.fourcc);
        return 1;
    }

    int held_fd = desc.objects[0].fd;
    uint32_t off0 = desc.layers[0].offset[0];
    uint32_t pitch0 = desc.layers[0].pitch[0];

    /* Keep an EGLImage + texture bound for the whole later-decode window. */
    EGLint held_at[] = {
        EGL_WIDTH, W, EGL_HEIGHT, H,
        EGL_LINUX_DRM_FOURCC_EXT, GBM_FORMAT_R8,
        EGL_DMA_BUF_PLANE0_FD_EXT, held_fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, (EGLint)off0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)pitch0,
        EGL_NONE,
    };
    EGLImage held_img = g_create_img(eglGetCurrentDisplay(), EGL_NO_CONTEXT,
                                     EGL_LINUX_DMA_BUF_EXT, (EGLClientBuffer)NULL,
                                     held_at);
    GLuint held_tex = 0;
    if (held_img == EGL_NO_IMAGE_KHR) {
        fprintf(stderr, "held eglCreateImage failed (0x%x)\n", eglGetError());
        close(held_fd);
        return 1;
    }
    glGenTextures(1, &held_tex);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, held_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    g_img2tex(GL_TEXTURE_2D, held_img);

    VASurfaceID last = surf[0];
    unsigned fn = 0;
    int n_later = n_slices < N_FRAMES ? n_slices : N_FRAMES;
    for (int n = 1; n < n_later; n++) {
        VASurfaceID s = surf[1 + (n - 1) % (N_SURF - 1)];
        if (is_idr[n])
            fn = 0;
        else
            fn++;
        decode_one(dpy, ctx, s, &pp, &iq, slices[n], slice_sizes[n],
                   is_idr[n], last, fn);
        last = s;
        fprintf(stderr, "decoded later frame %d onto surf slot %d idr=%d\n",
                n, 1 + (n - 1) % (N_SURF - 1), is_idr[n]);
    }

    /* Fresh import of the held fd after later pictures — current dma-buf. */
    if (read_r8_view(held_fd, off0, pitch0, W, H, ybuf) < 0) {
        fprintf(stderr, "held fd re-import failed after later pictures\n");
        g_destroy_img(eglGetCurrentDisplay(), held_img);
        glDeleteTextures(1, &held_tex);
        close(held_fd);
        return 1;
    }

    int bad = 0;
    for (int y = 0; y < H && !bad; y++) {
        for (int x = 0; x < W; x++) {
            if (ybuf[(size_t)y * W + x] != ref[(size_t)y * W + x]) {
                fprintf(stderr,
                        "MISMATCH HOLD-Y at (%d,%d): expected %u got %u\n",
                        x, y, ref[(size_t)y * W + x],
                        ybuf[(size_t)y * W + x]);
                bad = 1;
                break;
            }
        }
    }

    g_destroy_img(eglGetCurrentDisplay(), held_img);
    glDeleteTextures(1, &held_tex);
    close(held_fd);
    free(ybuf);
    free(ref);
    free(es);
    vaDestroyContext(dpy, ctx);
    vaDestroySurfaces(dpy, surf, N_SURF);
    vaDestroyConfig(dpy, cfg);
    vaTerminate(dpy);
    close(drm_fd);

    if (bad)
        return 1;
    printf("HOLD_EXACT\n");
    return 0;
}
