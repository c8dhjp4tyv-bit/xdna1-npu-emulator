/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * libxdna -- AMD XDNA1 (Phoenix / Hawk Point) NPU fonksiyonel emulatoru.
 *
 * Bu kutuphane QEMU'ya bagimli DEGILDIR. Disariya sadece bir PCI cihazinin
 * gozlemlenebilir davranisini verir:
 *
 *   - BAR MMIO okuma/yazma
 *   - host bellegine DMA (callback ile)
 *   - MSI-X interrupt tetikleme (callback ile)
 *
 * QEMU tarafi (qemu/hw/misc/xdna_npu.c) bu API'nin ince bir sarmalayicisidir.
 * Ayni cekirdek, QEMU olmadan da testlerden surulebilir; tests/test_boot.c
 * stock amdxdna surucusunun boot dizisini birebir taklit eder.
 */

#ifndef XDNA_EMU_H
#define XDNA_EMU_H

#include <stddef.h>
#include <stdint.h>

typedef struct XdnaNpu XdnaNpu;

enum {
    XDNA_LOG_ERROR = 0,
    XDNA_LOG_WARN,
    XDNA_LOG_INFO,
    XDNA_LOG_DEBUG,
};

/*
 * Host tarafinin sagladigi islemler. `opaque`, xdna_npu_new()'e verilen
 * degerdir (QEMU'da PCIDevice*, testlerde sahte bellek modeli).
 */
typedef struct XdnaHostOps {
    /* Guest fiziksel bellegine erisim. 0 = basarili, <0 = hata. */
    int (*dma_read)(void *opaque, uint64_t addr, void *buf, size_t len);
    /*
     * PSP firmware buffers are passed by amdxdna as guest physical addresses
     * (virt_to_phys()), even when force_iova selects a translated private
     * domain.  This optional callback reads that address without PCI-IOMMU
     * translation.  It is used only for PSP image validation; all regular
     * device DMA continues through dma_read/dma_write.
     */
    int (*dma_read_phys)(void *opaque, uint64_t addr, void *buf, size_t len);
    int (*dma_write)(void *opaque, uint64_t addr, const void *buf, size_t len);
    /* MSI-X vektorunu tetikle. */
    void (*raise_irq)(void *opaque, unsigned vector);
    /* Istege bagli; NULL olabilir. */
    void (*log)(void *opaque, int level, const char *msg);
} XdnaHostOps;

/* Emulatorun sundugu MSI-X vektor sayisi: 0 = mgmt, 1..6 = hwctx. */
#define XDNA_MSIX_VECTORS 8
#define XDNA_MGMT_MSIX_VECTOR 0

XdnaNpu *xdna_npu_new(const XdnaHostOps *ops, void *opaque);
void xdna_npu_free(XdnaNpu *npu);

/* Cihazi kapali/soguk sifirlama durumuna dondurur (FLR ve makine reset). */
void xdna_npu_reset(XdnaNpu *npu);

/* BAR boyutunu dondurur; bilinmeyen BAR icin 0. */
uint32_t xdna_npu_bar_size(int bar);

/*
 * MMIO erisimi. `bar` XDNA_BAR_REG / XDNA_BAR_SRAM / XDNA_BAR_MBOX olabilir.
 * len 1/2/4/8 olabilir; register semantigi 4 bayt hizali erisimde isler,
 * SRAM BAR'i duz bellek gibi davranir (memcpy_toio/fromio icin gerekli).
 */
uint64_t xdna_npu_mmio_read(XdnaNpu *npu, int bar, uint32_t off, unsigned len);
void xdna_npu_mmio_write(XdnaNpu *npu, int bar, uint32_t off, uint64_t val,
                         unsigned len);

/* Blok erisimi (test kosumlari ve hata ayiklama icin). */
void xdna_npu_mmio_read_buf(XdnaNpu *npu, int bar, uint32_t off, void *buf,
                            uint32_t len);
void xdna_npu_mmio_write_buf(XdnaNpu *npu, int bar, uint32_t off,
                             const void *buf, uint32_t len);

/*
 * Gozlem / test yardimcilari. Emulator ic durumunu disariya acar; QEMU
 * tarafinda "info xdna" benzeri bir HMP komutu bunlarin uzerine kurulur.
 */
typedef struct XdnaStats {
    uint64_t psp_cmds;
    uint64_t smu_cmds;
    uint64_t mbox_msgs_in;
    uint64_t mbox_msgs_out;
    uint64_t irqs_raised;
    uint32_t active_contexts;
    int fw_alive;
    int power_on;
} XdnaStats;

void xdna_npu_get_stats(const XdnaNpu *npu, XdnaStats *out);

#endif /* XDNA_EMU_H */
