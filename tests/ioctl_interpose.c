/* ioctl_interpose.c — log DRM_IOCTL_PRIME_FD_TO_HANDLE calls with the fd
 * being imported and what it points to (LD_PRELOAD into the GPU process). */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <drm/drm.h>

static int (*real_ioctl)(int, unsigned long, void *);

int ioctl(int fd, unsigned long req, void *arg)
{
    if (!real_ioctl)
        real_ioctl = dlsym(RTLD_NEXT, "ioctl");
    if (req == (unsigned long)DRM_IOCTL_PRIME_FD_TO_HANDLE && arg) {
        /* struct drm_prime_handle { __u32 handle; __u32 flags; __s32 fd; } */
        int pfd = ((int *)arg)[2];
        char lnk[64], tgt[512];
        ssize_t n;
        int r;
        snprintf(lnk, sizeof(lnk), "/proc/self/fd/%d", pfd);
        n = readlink(lnk, tgt, sizeof(tgt) - 1);
        if (n > 0)
            tgt[n] = 0;
        else
            snprintf(tgt, sizeof(tgt), "ERRNO_%zd", (ssize_t)n);
        r = real_ioctl(fd, req, arg);
        fprintf(stderr,
                "INTERPOSE prime_fd2h dev_fd=%d import_fd=%d target=%s -> %s\n",
                fd, pfd, tgt, r == 0 ? "OK" : strerror(-r));
        fflush(stderr);
        return r;
    }
    return real_ioctl(fd, req, arg);
}
