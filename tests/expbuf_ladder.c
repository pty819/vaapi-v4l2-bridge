/*
 * expbuf_ladder — isolated EXPBUF experiment (does NOT patch the driver).
 *
 * After the PSU swap, re-test whether VIDIOC_EXPBUF of a VPU capture buffer
 * still hangs the RK3588 when the GPU touches the fd.
 *
 * Stages (run one at a time; each prints then _exit so a hang is locatable):
 *   1  REQBUFS + EXPBUF + close fd          (no GPU)
 *   2  + mmap the expbuf fd, read 64 bytes  (CPU only)
 *   3  + DRM PRIME_FD_TO_HANDLE + GEM_CLOSE (kernel import, no map)
 *   4  + gbm_bo_import, then destroy        (no CPU map)
 *   5  + gbm_bo_map 64 bytes
 *   6  + eglCreateImageKHR NV12 single-fd   (historical hang suspect)
 *
 * Usage: expbuf_ladder /dev/videoN STAGE
 * Always run under `timeout -k 2 15`. Unbuffered stdout.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <linux/videodev2.h>
#include <drm/drm.h>
#include <drm/drm_fourcc.h>
#include <xf86drm.h>
#include <gbm.h>
#define EGL_EGLEXT_PROTOTYPES 1
#include <EGL/egl.h>
#include <EGL/eglext.h>

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
#ifndef EGL_PLATFORM_GBM_KHR
#define EGL_PLATFORM_GBM_KHR 0x31D6
#endif

#define SAY(fmt, ...) do { \
    printf("expbuf: " fmt "\n", ##__VA_ARGS__); \
    fflush(stdout); \
} while (0)

#define DIE(fmt, ...) do { \
    fprintf(stderr, "expbuf FAIL: " fmt " (%s)\n", ##__VA_ARGS__, strerror(errno)); \
    fflush(stderr); \
    _exit(1); \
} while (0)

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);
    if (argc != 3)
        DIE("usage: %s /dev/videoN STAGE(1-6)", argv[0]);
    const char *dev = argv[1];
    int stage = atoi(argv[2]);
    if (stage < 1 || stage > 6)
        DIE("STAGE must be 1..6");
    SAY("start dev=%s stage=%d pid=%d", dev, stage, getpid());

    int vfd = open(dev, O_RDWR | O_CLOEXEC);
    if (vfd < 0)
        DIE("open %s", dev);

    struct v4l2_capability cap;
    memset(&cap, 0, sizeof(cap));
    if (xioctl(vfd, VIDIOC_QUERYCAP, &cap) < 0)
        DIE("QUERYCAP");
    SAY("card='%s' bus='%s' caps=0x%x", cap.card, cap.bus_info, cap.device_caps);

    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(vfd, VIDIOC_G_FMT, &fmt) < 0)
        DIE("G_FMT capture mplane");
    fmt.fmt.pix_mp.width = 1280;
    fmt.fmt.pix_mp.height = 720;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    if (xioctl(vfd, VIDIOC_S_FMT, &fmt) < 0)
        DIE("S_FMT 1280x720 NV12");
    if (xioctl(vfd, VIDIOC_G_FMT, &fmt) < 0)
        DIE("G_FMT after S_FMT");
    SAY("fmt %ux%u fourcc=%.4s planes=%u sizeimage=%u bytesperline=%u",
        fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
        (char *)&fmt.fmt.pix_mp.pixelformat,
        fmt.fmt.pix_mp.num_planes,
        fmt.fmt.pix_mp.plane_fmt[0].sizeimage,
        fmt.fmt.pix_mp.plane_fmt[0].bytesperline);

    struct v4l2_requestbuffers req;
    memset(&req, 0, sizeof(req));
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(vfd, VIDIOC_REQBUFS, &req) < 0)
        DIE("REQBUFS MMAP");
    SAY("REQBUFS count=%u", req.count);
    if (req.count < 1)
        DIE("REQBUFS gave 0 buffers");
    {
        enum v4l2_buf_type tcap = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        if (xioctl(vfd, VIDIOC_STREAMON, &tcap) == 0)
            SAY("STREAMON capture ok");
        else
            SAY("STREAMON capture skipped (%s)", strerror(errno));
    }

    struct v4l2_exportbuffer exp;
    memset(&exp, 0, sizeof(exp));
    exp.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    exp.index = 0;
    exp.plane = 0;
    exp.flags = O_CLOEXEC | O_RDWR;
    SAY("calling EXPBUF...");
    if (xioctl(vfd, VIDIOC_EXPBUF, &exp) < 0)
        DIE("EXPBUF");
    SAY("EXPBUF fd=%d flags=0x%x", exp.fd, exp.flags);

    struct stat st;
    if (fstat(exp.fd, &st) == 0)
        SAY("expbuf fstat size=%lld mode=0%o", (long long)st.st_size, st.st_mode);

    if (stage == 1) {
        close(exp.fd);
        close(vfd);
        SAY("STAGE 1 PASS (EXPBUF + close, no GPU)");
        return 0;
    }

    void *map = mmap(NULL, 4096, PROT_READ, MAP_SHARED, exp.fd, 0);
    if (map == MAP_FAILED)
        DIE("mmap expbuf fd");
    volatile unsigned char sink = 0;
    for (int i = 0; i < 64; i++)
        sink ^= ((unsigned char *)map)[i];
    SAY("mmap CPU read xor=%u", sink);
    munmap(map, 4096);
    if (stage == 2) {
        close(exp.fd);
        close(vfd);
        SAY("STAGE 2 PASS (CPU mmap of expbuf)");
        return 0;
    }

    int drm_fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (drm_fd < 0)
        DIE("open renderD128");
    SAY("calling PRIME_FD_TO_HANDLE...");
    struct drm_prime_handle ph;
    memset(&ph, 0, sizeof(ph));
    ph.fd = exp.fd;
    if (ioctl(drm_fd, DRM_IOCTL_PRIME_FD_TO_HANDLE, &ph) < 0)
        DIE("PRIME_FD_TO_HANDLE");
    SAY("PRIME handle=%u", ph.handle);
    struct drm_gem_close gc;
    memset(&gc, 0, sizeof(gc));
    gc.handle = ph.handle;
    if (ioctl(drm_fd, DRM_IOCTL_GEM_CLOSE, &gc) < 0)
        DIE("GEM_CLOSE");
    SAY("GEM_CLOSE ok");
    if (stage == 3) {
        close(exp.fd);
        close(drm_fd);
        close(vfd);
        SAY("STAGE 3 PASS (PRIME import + close, no map)");
        return 0;
    }

    struct gbm_device *gdev = gbm_create_device(drm_fd);
    if (!gdev)
        DIE("gbm_create_device");
    struct gbm_import_fd_data imp;
    memset(&imp, 0, sizeof(imp));
    imp.fd = exp.fd;
    imp.width = fmt.fmt.pix_mp.width;
    imp.height = fmt.fmt.pix_mp.height;
    imp.stride = fmt.fmt.pix_mp.plane_fmt[0].bytesperline;
    imp.format = GBM_FORMAT_NV12;
    SAY("calling gbm_bo_import NV12...");
    struct gbm_bo *bo = gbm_bo_import(gdev, GBM_BO_IMPORT_FD, &imp, GBM_BO_USE_LINEAR);
    if (!bo) {
        SAY("NV12 import failed (%s), retry R8 geometry", strerror(errno));
        imp.height = fmt.fmt.pix_mp.height + (fmt.fmt.pix_mp.height + 1) / 2;
        imp.format = GBM_FORMAT_R8;
        bo = gbm_bo_import(gdev, GBM_BO_IMPORT_FD, &imp, GBM_BO_USE_LINEAR);
    }
    if (!bo)
        DIE("gbm_bo_import");
    SAY("gbm_bo_import ok stride=%u", gbm_bo_get_stride(bo));
    if (stage == 4) {
        gbm_bo_destroy(bo);
        close(exp.fd);
        gbm_device_destroy(gdev);
        close(drm_fd);
        close(vfd);
        SAY("STAGE 4 PASS (gbm_bo_import, no map)");
        return 0;
    }

    void *md = NULL;
    uint32_t ms = 0;
    SAY("calling gbm_bo_map...");
    uint8_t *p = gbm_bo_map(bo, 0, 0, 16, 16, GBM_BO_TRANSFER_READ, &ms, &md);
    if (!p)
        DIE("gbm_bo_map");
    sink = 0;
    for (int i = 0; i < 16; i++)
        sink ^= p[i];
    SAY("gbm map xor=%u stride=%u", sink, ms);
    gbm_bo_unmap(bo, md);
    if (stage == 5) {
        gbm_bo_destroy(bo);
        close(exp.fd);
        gbm_device_destroy(gdev);
        close(drm_fd);
        close(vfd);
        SAY("STAGE 5 PASS (gbm_bo_map CPU)");
        return 0;
    }

    PFNEGLGETPLATFORMDISPLAYEXTPROC get_dpy =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    PFNEGLCREATEIMAGEKHRPROC create_img =
        (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    if (!get_dpy || !create_img)
        DIE("egl procs");
    EGLDisplay dpy = get_dpy(EGL_PLATFORM_GBM_KHR, gdev, NULL);
    if (dpy == EGL_NO_DISPLAY)
        DIE("eglGetPlatformDisplay GBM");
    if (!eglInitialize(dpy, NULL, NULL))
        DIE("eglInitialize");
    EGLint attrs[] = {
        EGL_WIDTH, (EGLint)fmt.fmt.pix_mp.width,
        EGL_HEIGHT, (EGLint)fmt.fmt.pix_mp.height,
        EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_NV12,
        EGL_DMA_BUF_PLANE0_FD_EXT, exp.fd,
        EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, (EGLint)fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
        EGL_DMA_BUF_PLANE1_FD_EXT, exp.fd,
        EGL_DMA_BUF_PLANE1_OFFSET_EXT,
            (EGLint)(fmt.fmt.pix_mp.plane_fmt[0].bytesperline * fmt.fmt.pix_mp.height),
        EGL_DMA_BUF_PLANE1_PITCH_EXT, (EGLint)fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
        EGL_NONE
    };
    SAY("calling eglCreateImageKHR NV12 single-fd...");
    EGLImage img = create_img(dpy, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT,
                              (EGLClientBuffer)NULL, attrs);
    SAY("eglCreateImage %s egl=0x%x",
        img != EGL_NO_IMAGE_KHR ? "OK" : "FAIL", eglGetError());
    if (img != EGL_NO_IMAGE_KHR)
        eglDestroyImage(dpy, img);
    eglTerminate(dpy);
    gbm_bo_destroy(bo);
    close(exp.fd);
    gbm_device_destroy(gdev);
    close(drm_fd);
    close(vfd);
    SAY("STAGE 6 done (EGL import attempted, process still alive)");
    return img != EGL_NO_IMAGE_KHR ? 0 : 3;
}
