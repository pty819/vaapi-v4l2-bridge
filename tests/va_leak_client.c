/* va_leak_client — P0-2 reproducer: create surfaces + terminate WITHOUT
 * destroying anything, exactly what a session-cycling client (browser tab
 * close, ffmpeg exit path that skips cleanup) does. Expected under ASan:
 * the whole surface table + cpu_ptr backing + config leak via the driver's
 * v4l2sl_terminate.
 * usage: va_leak_client <render-node> [W H N]
 */
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <va/va.h>
#include <va/va_drm.h>

int main(int argc, char **argv)
{
    if (argc < 2) { fprintf(stderr, "usage: %s <render-node>\n", argv[0]); return 2; }
    int w = argc > 3 ? atoi(argv[2]) : 1280;
    int h = argc > 3 ? atoi(argv[3]) : 720;
    int n = argc > 4 ? atoi(argv[4]) : 8;

    int fd = open(argv[1], O_RDWR);
    if (fd < 0) { perror("open"); return 2; }
    VADisplay dpy = vaGetDisplayDRM(fd);
    if (!dpy) return 2;
    int major, minor;
    if (vaInitialize(dpy, &major, &minor) != VA_STATUS_SUCCESS) return 2;

    VAConfigAttrib rt = { .type = VAConfigAttribRTFormat, .value = VA_RT_FORMAT_YUV420 };
    VAConfigID cfg;
    if (vaCreateConfig(dpy, VAProfileH264High, VAEntrypointVLD, &rt, 1, &cfg) != VA_STATUS_SUCCESS)
        return 2;

    VASurfaceID surf[64];
    if (n > 64) n = 64;
    if (vaCreateSurfaces(dpy, VA_RT_FORMAT_YUV420, w, h, surf, n, NULL, 0) != VA_STATUS_SUCCESS)
        return 2;

    printf("created %d surfaces %dx%d, terminating WITHOUT destroy\n", n, w, h);
    vaTerminate(dpy);   /* <-- everything above must be freed HERE (P0-2) */
    close(fd);
    return 0;
}
