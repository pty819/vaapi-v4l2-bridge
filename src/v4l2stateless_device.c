/*
 * v4l2stateless — V4L2 device management
 */
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include "v4l2stateless.h"

int v4l2sl_open_device(const char *path)
{
    int fd = open(path, O_RDWR | O_NONBLOCK);
    if (fd < 0)
        fprintf(stderr, "v4l2stateless: open %s: %s\n", path, strerror(errno));
    return fd;
}

int v4l2sl_open_media_for_device(const char *video_path)
{
    /* /dev/videoN -> /dev/mediaN (same parent media device) */
    /* For now, just open /dev/media0 */
    return v4l2sl_open_device("/dev/media0");
}
