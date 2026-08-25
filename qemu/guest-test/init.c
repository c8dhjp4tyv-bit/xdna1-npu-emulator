/* Minik init: surucunun probe sonucunu gozlemle ve kapan. */
#include <fcntl.h>
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
        printf("  /dev/accel/accel0 ACILAMADI\n");
    }
    printf("########## TEST BITTI ##########\n\n");

    sync();
    reboot(RB_POWER_OFF);
    return 0;
}
