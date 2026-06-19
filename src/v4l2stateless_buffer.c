/*
 * v4l2stateless — V4L2 buffer management (request API)
 *
 * Handles:
 * - MEDIA_IOC_REQUEST_ALLOC for request fd
 * - VIDIOC_QBUF / VIDIOC_DQBUF for capture/output queues
 * - V4L2_CTRL_WHICH_REQUEST_VAL for request-scoped controls
 */
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <linux/media.h>
#include <linux/videodev2.h>
#include "v4l2stateless.h"

/* TODO: Phase 2 — implement V4L2 request buffer management */
