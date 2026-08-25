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

/* ctrlcode uretici -- testlerle ayni kod (tests/ctrlcode.h). */
#include "ctrlcode.h"

/*
 * Carveout bolgesi: run.sh cekirdek komut satirinda
 * "memmap=128M$0x60000000" ile ayiriyor. Boyut@adres bicimi surucunun
 * debugfs arayuzunun bekledigi bicim.
 *
 * 128 MB, cunku cihaz heap'i tam 64 MB olmak ZORUNDA (surucu heap
 * boyutunun dev_heap_max_size'in kati olmasini istiyor) ve CMD/SHARE
 * BO'lari da ayni blogtan ayriliyor.
 */
#ifndef CARVEOUT_CFG
#define CARVEOUT_CFG "0x8000000@0x60000000"
#endif

#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <time.h>

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

/* ---------------------------------------------------------------- */
/* Uctan uca workload                                                */
/* ---------------------------------------------------------------- */

/* amdxdna_ctx.h: exec buffer komut basligi alanlari */
#define CMD_STATE_SHIFT      0
#define CMD_EXTRA_CU_SHIFT  10
#define CMD_COUNT_SHIFT     12
#define CMD_OPCODE_SHIFT    23
#define ERT_CMD_STATE_NEW    1
#define ERT_START_NPU       20

/* amdxdna_ctx.h: struct amdxdna_cmd_start_npu */
typedef struct {
    uint64_t buffer;
    uint32_t buffer_size;
    uint32_t prop_count;
} CmdStartNpu;

typedef struct {
    unsigned handle;
    uint64_t dev_addr;      /* cihaz (XDNA) adresi */
    void *map;              /* userspace esleme */
    uint64_t size;
} Bo;

static int bo_new(int fd, unsigned type, uint64_t size, Bo *out)
{
    struct amdxdna_drm_create_bo bo;
    struct amdxdna_drm_get_bo_info info;

    memset(out, 0, sizeof(*out));
    memset(&bo, 0, sizeof(bo));
    bo.type = type;
    bo.size = size;
    if (ioctl(fd, DRM_IOCTL_AMDXDNA_CREATE_BO, &bo) != 0) {
        return -1;
    }

    memset(&info, 0, sizeof(info));
    info.handle = bo.handle;
    if (ioctl(fd, DRM_IOCTL_AMDXDNA_GET_BO_INFO, &info) != 0) {
        return -1;
    }

    out->handle = bo.handle;
    out->dev_addr = info.xdna_addr;
    out->size = size;

    /*
     * Cihaz bellegi BO'lari heap'in icinden ayrildigi icin zaten bir user
     * VA'ya sahip; surucu onu `vaddr` ile donduruyor. Digerleri icin
     * mmap() gerekiyor. Uapi "0 in case user needs mmap()" diyor ama
     * surucu pratikte AMDXDNA_INVALID_ADDR (~0) donduruyor -- ikisini de
     * "mmap gerekiyor" say.
     */
    if (info.vaddr && info.vaddr != AMDXDNA_INVALID_ADDR) {
        out->map = (void *)(unsigned long)info.vaddr;
        return 0;
    }

    out->map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                    (off_t)info.map_offset);
    if (out->map == MAP_FAILED) {
        out->map = NULL;
        return -2;
    }
    return 0;
}

/*
 * Gercek bir workload'i uctan uca kostur:
 *
 *   guest ctrlcode uretir  ->  DRM EXEC_CMD  ->  surucu  ->  mailbox
 *   ->  emulator MERT  ->  ctrlcode yorumlayicisi  ->  XDNA array
 *   ->  shim DMA  ->  cikis BO'su
 *
 * Veri yolu, tests/test_exec.c'nin dogruladigi yolun aynisi: giris BO'su
 * -> shim MM2S -> memory tile -> shim S2MM -> cikis BO'su.
 */
static int run_workload(int fd, unsigned hwctx, unsigned syncobj)
{
    enum { WORDS = 64, BYTES = WORDS * 4 };
    Bo in, out, inst, cmd;
    uint32_t *cw;
    CmdStartNpu *sn;
    uint32_t cc_size, count;
    unsigned i;
    struct amdxdna_drm_config_hwctx cfg;
    struct {
        uint16_t num_cus;
        uint16_t pad[3];
        struct amdxdna_cu_config cu[1];
    } cu_cfg;
    struct amdxdna_drm_exec_cmd exec;
    struct amdxdna_drm_wait_cmd wait;

    printf("  --- uctan uca workload ---\n");

    {
        static const struct { unsigned type; uint64_t size; const char *name; }
        want[] = {
            { AMDXDNA_BO_DEV, 0x10000, "inst" },
            { AMDXDNA_BO_DEV, 0x1000,  "in"   },
            { AMDXDNA_BO_DEV, 0x1000,  "out"  },
            { AMDXDNA_BO_CMD, 0x1000,  "cmd"  },
        };
        Bo *slot[4] = { &inst, &in, &out, &cmd };
        unsigned k;

        for (k = 0; k < 4; k++) {
            int r = bo_new(fd, want[k].type, want[k].size, slot[k]);

            if (r != 0) {
                printf("  BO '%s'%*s: %s hatasi %d\n", want[k].name,
                       (int)(16 - strlen(want[k].name)), "",
                       r == -2 ? "mmap" : "ioctl", errno);
                return -1;
            }
        }
    }
    printf("  BO'lar                 = inst 0x%llx, in 0x%llx, out 0x%llx\n",
           (unsigned long long)inst.dev_addr,
           (unsigned long long)in.dev_addr,
           (unsigned long long)out.dev_addr);

    /*
     * CU yapilandirmasi: surucu CONFIG_CU'yu firmware'e gonderiyor.
     * CU tampon BO'su olarak instruction BO'sunu veriyoruz.
     */
    memset(&cu_cfg, 0, sizeof(cu_cfg));
    cu_cfg.num_cus = 1;
    cu_cfg.cu[0].cu_bo = inst.handle;
    cu_cfg.cu[0].cu_func = 0;

    memset(&cfg, 0, sizeof(cfg));
    cfg.handle = hwctx;
    cfg.param_type = DRM_AMDXDNA_HWCTX_CONFIG_CU;
    cfg.param_val = (unsigned long long)(unsigned long)&cu_cfg;
    cfg.param_val_size = sizeof(cu_cfg);
    if (ioctl(fd, DRM_IOCTL_AMDXDNA_CONFIG_HWCTX, &cfg) != 0) {
        printf("  CONFIG_HWCTX(CU)       : hata %d\n", errno);
        return -1;
    }
    printf("  CONFIG_HWCTX(CU)       = tamam\n");

    /* Giris desenini yaz, cikisi temizle. */
    for (i = 0; i < BYTES; i++) {
        ((uint8_t *)in.map)[i] = (uint8_t)(i * 7u + 3u);
    }
    memset(out.map, 0, BYTES);

    /*
     * ctrlcode: shim BD adresleri argüman BO'larinin CIHAZ adresleri.
     * Gercek akista bunu XRT DDR_PATCH ile yapiyor; burada dogrudan
     * yaziyoruz.
     */
    cc_size = build_loopback_ctrlcode(inst.map, in.dev_addr, out.dev_addr,
                                      WORDS);
    printf("  ctrlcode               = %u bayt\n", cc_size);

    /* ERT komut paketi. */
    cw = cmd.map;
    cw[1] = 0x1;                             /* cu_mask: CU 0 */
    sn = (CmdStartNpu *)&cw[2];
    sn->buffer = inst.dev_addr;
    sn->buffer_size = cc_size;
    sn->prop_count = 0;
    count = 1 + (uint32_t)(sizeof(*sn) / 4);  /* cu_mask + payload */
    cw[0] = ((uint32_t)ERT_CMD_STATE_NEW << CMD_STATE_SHIFT) |
            (count << CMD_COUNT_SHIFT) |
            ((uint32_t)ERT_START_NPU << CMD_OPCODE_SHIFT);

    memset(&exec, 0, sizeof(exec));
    exec.hwctx = hwctx;
    exec.type = 0;                            /* AMDXDNA_CMD_SUBMIT_EXEC_BUF */
    /*
     * uapi: "Array of command handles or the command handle itself in case
     * of just one." Tek komutta isaretci degil, handle'in KENDISI.
     */
    exec.cmd_handles = cmd.handle;
    exec.cmd_count = 1;
    exec.args = 0;
    exec.arg_count = 0;
    if (ioctl(fd, DRM_IOCTL_AMDXDNA_EXEC_CMD, &exec) != 0) {
        printf("  EXEC_CMD               : hata %d\n", errno);
        return -1;
    }
    printf("  EXEC_CMD               = seq %llu\n",
           (unsigned long long)exec.seq);

    /*
     * Tamamlanmayi bekle. Bu surucude WAIT_CMD ioctl'i yok (cmd_wait
     * isleyicisi tanimli degil, -EOPNOTSUPP donuyor); CREATE_HWCTX'in
     * dondurdugu DRM timeline syncobj kullaniliyor: her komutun seq'i
     * bir timeline noktasi.
     */
    memset(&wait, 0, sizeof(wait));
    wait.hwctx = hwctx;
    wait.seq = exec.seq;
    wait.timeout = 5000;
    if (ioctl(fd, DRM_IOCTL_AMDXDNA_WAIT_CMD, &wait) == 0) {
        printf("  WAIT_CMD               = tamam\n");
    } else if (errno == EOPNOTSUPP) {
        struct drm_syncobj_timeline_wait tw;
        struct timespec ts;
        uint32_t h = syncobj;
        uint64_t pt = exec.seq;

        clock_gettime(CLOCK_MONOTONIC, &ts);
        memset(&tw, 0, sizeof(tw));
        tw.handles = (unsigned long long)(unsigned long)&h;
        tw.points = (unsigned long long)(unsigned long)&pt;
        tw.count_handles = 1;
        tw.timeout_nsec = (int64_t)(ts.tv_sec + 5) * 1000000000 + ts.tv_nsec;
        tw.flags = DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL |
                   DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT;
        if (ioctl(fd, DRM_IOCTL_SYNCOBJ_TIMELINE_WAIT, &tw) != 0) {
            printf("  SYNCOBJ_TIMELINE_WAIT  : hata %d\n", errno);
            return -1;
        }
        printf("  SYNCOBJ_TIMELINE_WAIT  = tamam (nokta %llu)\n",
               (unsigned long long)pt);
    } else {
        printf("  WAIT_CMD               : hata %d\n", errno);
        return -1;
    }

    /*
     * Surucu komutun ERT durumunu CMD BO basligina geri yaziyor.
     * 4 = COMPLETED, 5 = ERROR.
     */
    for (i = 0; i < 1000u && (cw[0] & 0xFu) < 4u; i++) {
        usleep(1000);
    }
    printf("  komut durumu           = %u (%s)\n", cw[0] & 0xFu,
           (cw[0] & 0xFu) == 4u ? "COMPLETED" :
           (cw[0] & 0xFu) == 5u ? "ERROR" : "?");

    if (memcmp(in.map, out.map, BYTES) == 0) {
        printf("  SONUC                  = cikis girisle BIREBIR AYNI "
               "(%u bayt)\n", (unsigned)BYTES);
        return 0;
    }
    for (i = 0; i < BYTES; i++) {
        if (((uint8_t *)in.map)[i] != ((uint8_t *)out.map)[i]) {
            printf("  SONUC                  = FARKLI, ilk fark bayt %u "
                   "(0x%02x != 0x%02x)\n", i, ((uint8_t *)in.map)[i],
                   ((uint8_t *)out.map)[i]);
            break;
        }
    }
    return -1;
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
        /* Surucu heap boyutunun 64 MB'in kati olmasini istiyor. */
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
            int ok = 0;

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

                ok = run_workload(fd, ctx.handle, ctx.syncobj_handle);
                (void)ok;

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
