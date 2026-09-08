/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Small guest-side XRT/amdxdna lifetime probe.
 *
 * The public XRT C API can open/close a device without an xclbin.  Creating a
 * hardware context through that API requires a real xclbin, so the context
 * part deliberately uses the stock amdxdna DRM UAPI directly.  This is not a
 * replacement driver and it does not claim execution support: a non-zero
 * ioctl result is reported to the caller and causes the probe to fail.
 */

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <drm/amdxdna_accel.h>
#include <drm/drm.h>
#include <xrt/xrt_device.h>

#ifndef O_CLOEXEC
#define O_CLOEXEC 0
#endif

#define DEFAULT_DEVICE "/dev/accel/accel0"
#define DEFAULT_REPEATS 3
#define DEV_HEAP_SIZE (64ULL * 1024ULL * 1024ULL)

static void print_errno(const char *what)
{
    fprintf(stderr, "%s: errno=%d (%s)\n", what, errno, strerror(errno));
}

static int close_gem(int fd, uint32_t handle)
{
    struct drm_gem_close close_req = { .handle = handle };

    if (ioctl(fd, DRM_IOCTL_GEM_CLOSE, &close_req) < 0) {
        print_errno("DRM_IOCTL_GEM_CLOSE");
        return -1;
    }
    return 0;
}

static int create_context_once(const char *device_path, unsigned iteration)
{
    struct amdxdna_drm_create_bo bo = {
        .size = DEV_HEAP_SIZE,
        .type = AMDXDNA_BO_DEV_HEAP,
    };
    struct amdxdna_qos_info qos = {
        .priority = AMDXDNA_QOS_NORMAL_PRIORITY,
    };
    struct amdxdna_drm_create_hwctx context = {
        .qos_p = (uintptr_t)&qos,
        .max_opc = 2048,
        /* NPU1 metadata has four core rows; request one column. */
        .num_tiles = 4,
        .mem_size = 0,
    };
    struct amdxdna_drm_destroy_hwctx destroy = { 0 };
    int fd;
    int ret = -1;
    int saved_errno;
    int bo_created = 0;
    void *heap_map = MAP_FAILED;
    struct amdxdna_drm_get_bo_info heap_info = { 0 };

    fd = open(device_path, O_RDWR | O_CLOEXEC);
    if (fd < 0) {
        print_errno("open accelerator");
        return -1;
    }
    printf("client_open=ok path=%s\n", device_path);

    if (ioctl(fd, DRM_IOCTL_AMDXDNA_CREATE_BO, &bo) < 0) {
        print_errno("DRM_IOCTL_AMDXDNA_CREATE_BO (64 MiB dev heap)");
        goto out_close;
    }
    bo_created = 1;
    printf("context_iteration[%u]=begin\n", iteration);
    printf("dev_heap_create[%u]=ok handle=%" PRIu32 " size=%" PRIu64 "\n",
           iteration, bo.handle, (uint64_t)DEV_HEAP_SIZE);

    heap_info.handle = bo.handle;
    if (ioctl(fd, DRM_IOCTL_AMDXDNA_GET_BO_INFO, &heap_info) < 0) {
        print_errno("DRM_IOCTL_AMDXDNA_GET_BO_INFO (dev heap)");
        goto out_bo;
    }
    if (heap_info.map_offset == AMDXDNA_INVALID_ADDR) {
        fprintf(stderr, "dev heap has no mmap offset\n");
        goto out_bo;
    }
    heap_map = mmap(NULL, DEV_HEAP_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED,
                    fd, (off_t)heap_info.map_offset);
    if (heap_map == MAP_FAILED) {
        print_errno("mmap dev heap");
        goto out_bo;
    }
    printf("dev_heap_mmap[%u]=ok addr=%p size=%" PRIu64 "\n", iteration,
           heap_map, (uint64_t)DEV_HEAP_SIZE);

    if (ioctl(fd, DRM_IOCTL_AMDXDNA_CREATE_HWCTX, &context) < 0) {
        print_errno("DRM_IOCTL_AMDXDNA_CREATE_HWCTX");
        goto out_bo;
    }
    printf("context_create[%u]=ok handle=%" PRIu32 " syncobj=%" PRIu32
           " doorbell=%" PRIu32 "\n", iteration, context.handle,
           context.syncobj_handle, context.umq_doorbell);

    destroy.handle = context.handle;
    if (ioctl(fd, DRM_IOCTL_AMDXDNA_DESTROY_HWCTX, &destroy) < 0) {
        print_errno("DRM_IOCTL_AMDXDNA_DESTROY_HWCTX");
        goto out_bo;
    }
    printf("context_destroy[%u]=ok handle=%" PRIu32 "\n", iteration,
           context.handle);
    ret = 0;

out_bo:
    saved_errno = errno;
    if (heap_map != MAP_FAILED && munmap(heap_map, DEV_HEAP_SIZE) < 0 && ret == 0) {
        print_errno("munmap dev heap");
        ret = -1;
    }
    if (bo_created && close_gem(fd, bo.handle) < 0 && ret == 0) {
        ret = -1;
    }
    errno = saved_errno;
out_close:
    if (close(fd) < 0 && ret == 0) {
        print_errno("close accelerator");
        ret = -1;
    }
    return ret;
}

static int xrt_open_close_repeated(unsigned repeats)
{
    unsigned i;
    int failed = 0;

    for (i = 0; i < repeats; ++i) {
        xrtDeviceHandle handle = xrtDeviceOpen(0);

        if (!handle) {
            fprintf(stderr, "xrtDeviceOpen(0): returned NULL on iteration %u\n",
                    i);
            failed = 1;
            continue;
        }
        if (xrtDeviceClose(handle) != 0) {
            fprintf(stderr, "xrtDeviceClose: failure on iteration %u\n", i);
            failed = 1;
            continue;
        }
        printf("xrt_open_close[%u]=ok\n", i);
    }
    return failed ? -1 : 0;
}

int main(int argc, char **argv)
{
    const char *device_path = DEFAULT_DEVICE;
    unsigned repeats = DEFAULT_REPEATS;
    char *end;
    int ret;

    if (argc > 1) {
        device_path = argv[1];
    }
    if (argc > 2) {
        unsigned long value = strtoul(argv[2], &end, 10);

        if (*argv[2] == '\0' || *end != '\0' || value == 0 ||
            value > UINT32_MAX) {
            fprintf(stderr, "usage: %s [accel-node] [repeat-count]\n", argv[0]);
            return 2;
        }
        repeats = (unsigned)value;
    }
    if (argc > 3) {
        fprintf(stderr, "usage: %s [accel-node] [repeat-count]\n", argv[0]);
        return 2;
    }

    /* XRT's index-based handle must refer to the same DRM node we exercise.
     * This helper intentionally supports the single-device acceptance setup;
     * reject a different node instead of silently opening XRT device 0 while
     * probing another accelerator through DRM. */
    if (strcmp(device_path, DEFAULT_DEVICE) != 0) {
        fprintf(stderr, "device selector mismatch: this helper supports only %s\n",
                DEFAULT_DEVICE);
        return 2;
    }

    printf("xrt_probe_device=%s repeats=%u\n", device_path, repeats);
    ret = xrt_open_close_repeated(repeats);
    if (ret < 0) {
        fprintf(stderr, "xrt_open_close=fail\n");
        return 1;
    }
    for (unsigned i = 0; i < repeats; ++i) {
        ret = create_context_once(device_path, i);
        if (ret < 0) {
            fprintf(stderr, "context_lifetime=fail iteration=%u\n", i);
            return 1;
        }
    }
    printf("context_lifetime=ok iterations=%u\n", repeats);
    return 0;
}
