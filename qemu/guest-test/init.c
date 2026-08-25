/*
 * Minik init: surucunun probe sonucunu gozlemle, /dev/accel/accel0'i ac ve
 * stock UAPI ioctl'lerini cagir.
 *
 * Derleme (kernel kaynagindaki UAPI basliklari gerekiyor):
 *   gcc -static -O2 -I<linux>/include/uapi -o init init.c
 */
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

/*
 * Ham uapi basliklarini `make headers_install` yapmadan kullaniyoruz;
 * o adimin tek yaptigi bu isaretcileri silmek.
 */
#define __user
#include <drm/amdxdna_accel.h>

/*
 * Carveout bolgesi: run.sh cekirdek komut satirinda
 * "memmap=64M$0x60000000" ile ayiriyor. Boyut@adres bicimi surucunun
 * debugfs arayuzunun bekledigi bicim.
 */
#ifndef CARVEOUT_CFG
#define CARVEOUT_CFG "0x4000000@0x60000000"
#endif

#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>

static void cat(const char *path)
{
    char buf[512];
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) { printf("  %s: ACILAMADI\n", path); return; }
    n = read(fd, buf, sizeof(buf) - 1);
    if (n > 0) { buf[n] = 0; printf("  %s = %s", path, buf); if (buf[n-1] != '\n') printf("\n"); }
    close(fd);
}

static void ls(const char *path)
{
    DIR *d = opendir(path);
    struct dirent *e;

    if (!d) { printf("  %s: YOK\n", path); return; }
    printf("  %s:", path);
    while ((e = readdir(d))) {
        if (e->d_name[0] != '.') printf(" %s", e->d_name);
    }
    printf("\n");
    closedir(d);
}

/*
 * Stock surucunun UAPI'sini gercekten kullan: DRM ioctl'lerini cagirip
 * emulatorden donen degerleri bas. Bunlar surucude mailbox mesajlarina
 * ceviriliyor, yani yanit gercekten emulatorden geliyor.
 */
static int get_info(int fd, unsigned param, void *buf, unsigned size)
{
    struct amdxdna_drm_get_info arg;

    memset(&arg, 0, sizeof(arg));
    arg.param = param;
    arg.buffer_size = size;
    arg.buffer = (unsigned long long)(unsigned long)buf;
    return ioctl(fd, DRM_IOCTL_AMDXDNA_GET_INFO, &arg);
}

static void probe_ioctls(int fd)
{
    printf("  --- DRM ioctl'leri (stock UAPI) ---\n");

    {
        struct amdxdna_drm_query_aie_version v;

        memset(&v, 0, sizeof(v));
        if (get_info(fd, DRM_AMDXDNA_QUERY_AIE_VERSION, &v, sizeof(v)) == 0) {
            printf("  QUERY_AIE_VERSION      = %u.%u\n", v.major, v.minor);
        } else {
            printf("  QUERY_AIE_VERSION      : hata %d\n", errno);
        }
    }

    {
        struct amdxdna_drm_query_firmware_version v;

        memset(&v, 0, sizeof(v));
        if (get_info(fd, DRM_AMDXDNA_QUERY_FIRMWARE_VERSION, &v,
                     sizeof(v)) == 0) {
            printf("  QUERY_FIRMWARE_VERSION = %u.%u.%u.%u\n", v.major,
                   v.minor, v.patch, v.build);
        } else {
            printf("  QUERY_FIRMWARE_VERSION : hata %d\n", errno);
        }
    }

    {
        struct amdxdna_drm_query_aie_metadata m;

        memset(&m, 0, sizeof(m));
        if (get_info(fd, DRM_AMDXDNA_QUERY_AIE_METADATA, &m, sizeof(m)) == 0) {
            printf("  QUERY_AIE_METADATA     = %u sutun, sutun boyu %u\n",
                   m.cols, m.col_size);
            printf("    core: %u satir @%u, mem: %u satir @%u, "
                   "shim: %u satir @%u\n",
                   m.core.row_count, m.core.row_start,
                   m.mem.row_count, m.mem.row_start,
                   m.shim.row_count, m.shim.row_start);
        } else {
            printf("  QUERY_AIE_METADATA     : hata %d\n", errno);
        }
    }

    /*
     * Cihaz heap'i: surucu bunu MAP_HOST_BUFFER ile emulatore bildiriyor,
     * emulator de context heap'i olarak kullaniyor.
     */
    {
        struct amdxdna_drm_create_bo bo;
        unsigned heap_handle = 0;

        memset(&bo, 0, sizeof(bo));
        bo.type = AMDXDNA_BO_DEV_HEAP;
        bo.size = 64u << 20;
        if (ioctl(fd, DRM_IOCTL_AMDXDNA_CREATE_BO, &bo) == 0) {
            heap_handle = bo.handle;
            printf("  CREATE_BO(DEV_HEAP)    = handle %u, %llu bayt\n",
                   bo.handle, (unsigned long long)bo.size);
        } else {
            printf("  CREATE_BO(DEV_HEAP)    : hata %d\n", errno);
        }

        /*
         * Heap'in userspace'e mmap EDILMESI sart: surucu context
         * olustururken heap'in user VA'sini emulatore bildiriyor
         * (MAP_HOST_BUFFER). mmap edilmemis heap ile CREATE_HWCTX
         * "Heap N is not mapped" diyor.
         */
        if (heap_handle) {
            struct amdxdna_drm_get_bo_info info;

            memset(&info, 0, sizeof(info));
            info.handle = heap_handle;
            if (ioctl(fd, DRM_IOCTL_AMDXDNA_GET_BO_INFO, &info) != 0) {
                printf("  GET_BO_INFO            : hata %d\n", errno);
            } else {
                void *p = mmap(NULL, bo.size, PROT_READ | PROT_WRITE,
                               MAP_SHARED, fd, (off_t)info.map_offset);

                printf("  GET_BO_INFO            = xdna_addr 0x%llx, "
                       "map_offset 0x%llx\n",
                       (unsigned long long)info.xdna_addr,
                       (unsigned long long)info.map_offset);
                if (p == MAP_FAILED) {
                    printf("  heap mmap              : hata %d\n", errno);
                } else {
                    printf("  heap mmap              = %p\n", p);
                }
            }
        }

        if (heap_handle) {
            struct amdxdna_drm_create_hwctx ctx;
            struct amdxdna_qos_info qos;

            memset(&qos, 0, sizeof(qos));
            qos.priority = AMDXDNA_QOS_NORMAL_PRIORITY;
            qos.gops = 1;
            qos.fps = 30;

            memset(&ctx, 0, sizeof(ctx));
            ctx.qos_p = (unsigned long long)(unsigned long)&qos;
            ctx.max_opc = 0x800;
            ctx.num_tiles = 4;      /* bir sutun: 4 compute tile */
            ctx.mem_size = 0;
            ctx.umq_bo = 0;
            ctx.log_buf_bo = 0;
            if (ioctl(fd, DRM_IOCTL_AMDXDNA_CREATE_HWCTX, &ctx) == 0) {
                struct amdxdna_drm_destroy_hwctx del;

                printf("  CREATE_HWCTX           = handle %u, syncobj %u\n",
                       ctx.handle, ctx.syncobj_handle);

                memset(&del, 0, sizeof(del));
                del.handle = ctx.handle;
                if (ioctl(fd, DRM_IOCTL_AMDXDNA_DESTROY_HWCTX, &del) == 0) {
                    printf("  DESTROY_HWCTX          = tamam\n");
                } else {
                    printf("  DESTROY_HWCTX          : hata %d\n", errno);
                }
            } else {
                printf("  CREATE_HWCTX           : hata %d\n", errno);
            }
        }
    }
}

int main(void)
{
    int fd;

    mount("proc", "/proc", "proc", 0, NULL);
    mount("sysfs", "/sys", "sysfs", 0, NULL);
    mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);

    /* devtmpfs bagliandiktan sonra konsolu ac; yoksa printf kayboluyor. */
    {
        int c = open("/dev/console", O_RDWR);
        if (c >= 0) {
            dup2(c, 0);
            dup2(c, 1);
            dup2(c, 2);
            if (c > 2) {
                close(c);
            }
        }
    }
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("\n########## XDNA EMULATOR GUEST TESTI ##########\n");
    ls("/dev/accel");
    ls("/sys/class/accel");
    cat("/sys/class/accel/accel0/device/vbnv");
    cat("/sys/class/accel/accel0/device/fw_version");

    /* PCIe genisletilmis yetenek listesini dok: ATS(0x0F), PRI(0x13), PASID(0x1B) */
    {
        unsigned char cfg[4096];
        int cf = open("/sys/bus/pci/devices/0000:00:03.0/config", O_RDONLY);
        if (cf >= 0) {
            ssize_t n = read(cf, cfg, sizeof(cfg));
            close(cf);
            printf("  config space okundu: %ld bayt\n", (long)n);
            if (n > 0x100) {
                unsigned off = 0x100;
                while (off && off < (unsigned)n) {
                    unsigned hdr = cfg[off] | (cfg[off+1]<<8) |
                                   (cfg[off+2]<<16) | (cfg[off+3]<<24);
                    unsigned id = hdr & 0xFFFF;
                    if (!id) break;
                    printf("  ext cap @0x%03x id=0x%04x%s\n", off, id,
                           id==0x0F ? " (ATS)" : id==0x13 ? " (PRI)" :
                           id==0x1B ? " (PASID)" : "");
                    off = (hdr >> 20) & 0xFFF;
                }
            }
        } else {
            printf("  config space acilamadi\n");
        }
    }

    fd = open("/dev/accel/accel0", O_RDWR);
    if (fd >= 0) {
        printf("  /dev/accel/accel0 ACILDI (fd=%d)\n", fd);
        close(fd);
    } else {
        printf("  /dev/accel/accel0 ACILAMADI (SVA yok)\n");
    }

    /*
     * SVA baglanmadiginda surucunun kendi yedek yolu: carveout bellek.
     * amdxdna_drm_open, PASID alinamazsa carveout yapilandirilmissa
     * open()'i BASARILI sayiyor (amdxdna_pci_drv.c). Carveout, stock
     * surucunun debugfs arayuzunden ayarlaniyor:
     *
     *     /sys/kernel/debug/accel/<aygit>/carveout  <-  "<boyut>@<adres>"
     *
     * DRM debugfs kokunu aygitin PCI adresiyle adlandiriyor, o yuzden
     * dizini tarayip buluyoruz.
     *
     * Adres, kernel'in kullanmadigi fiziksel bir bolge olmali; kosumda
     * cekirdek komut satirinda memmap= ile ayriliyor.
     */
    if (mount("debugfs", "/sys/kernel/debug", "debugfs", 0, NULL) != 0) {
        printf("  debugfs baglanamadi\n");
    }
    {
        static const char cfg[] = CARVEOUT_CFG;
        char path[320] = "";
        DIR *d = opendir("/sys/kernel/debug/accel");
        struct dirent *e;
        int cf = -1;

        while (d && (e = readdir(d))) {
            if (e->d_name[0] == '.') {
                continue;
            }
            snprintf(path, sizeof(path),
                     "/sys/kernel/debug/accel/%s/carveout", e->d_name);
            cf = open(path, O_RDWR);
            if (cf >= 0) {
                break;
            }
        }
        if (d) {
            closedir(d);
        }

        if (cf < 0) {
            printf("  carveout debugfs dosyasi YOK\n");
            ls("/sys/kernel/debug/accel");
        } else if (write(cf, cfg, sizeof(cfg) - 1) < 0) {
            printf("  carveout yazilamadi (%s -> %s)\n", cfg, path);
            close(cf);
        } else {
            close(cf);
            printf("  carveout ayarlandi: %s\n", cfg);
            cat(path);

            fd = open("/dev/accel/accel0", O_RDWR);
            if (fd >= 0) {
                printf("  /dev/accel/accel0 ACILDI (carveout ile, fd=%d)\n",
                       fd);
                probe_ioctls(fd);
                close(fd);
            } else {
                printf("  /dev/accel/accel0 carveout ile de ACILAMADI\n");
            }
        }
    }
    printf("########## TEST BITTI ##########\n\n");

    sync();
    reboot(RB_POWER_OFF);
    return 0;
}
