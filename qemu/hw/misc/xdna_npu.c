/*
 * AMD XDNA1 (Phoenix / Hawk Point) NPU -- QEMU PCI aygiti.
 *
 * Bu dosya ince bir kabuktur: tum davranis libxdna icindedir. Buradaki is
 * QEMU'nun PCI/MMIO/MSI-X/DMA arayuzlerini libxdna'nin XdnaHostOps
 * callback'lerine baglamaktan ibarettir.
 *
 * SPDX-License-Identifier: GPL-2.0-only
 *
 * DIKKAT: Bu dosya bu depoda DERLENMEMISTIR -- konteynerde QEMU agaci yok.
 * Derlemek icin qemu/README.md'deki adimlari izleyin.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/msix.h"
#include "hw/qdev-properties.h"
#include "migration/vmstate.h"
#include "qom/object.h"

#include "xdna/xdna_emu.h"
#include "xdna/xdna_regs.h"

#define TYPE_XDNA_NPU "xdna-npu"
OBJECT_DECLARE_SIMPLE_TYPE(XdnaNpuState, XDNA_NPU)

/*
 * MSI-X tablosu icin ayri bir BAR ayiramiyoruz: surucu BAR 0/2/4'u
 * bekliyor ve hepsi 64-bit oldugundan 0..5 doluyor. Tabloyu REG BAR'inin
 * kullanilmayan ust bolgesine koyuyoruz (registerlar 0x11000'in altinda).
 */
#define XDNA_MSIX_TABLE_OFFSET 0x40000
#define XDNA_MSIX_PBA_OFFSET   0x50000

struct XdnaNpuState {
    PCIDevice parent_obj;

    MemoryRegion bar_reg;
    MemoryRegion bar_sram;
    MemoryRegion bar_mbox;

    XdnaNpu *emu;
};

/* ------------------------------------------------------------------ */
/* Host ops                                                            */
/* ------------------------------------------------------------------ */

static int xdna_host_dma_read(void *opaque, uint64_t addr, void *buf,
                              size_t len)
{
    XdnaNpuState *s = opaque;

    return pci_dma_read(PCI_DEVICE(s), addr, buf, len) == MEMTX_OK ? 0 : -1;
}

static int xdna_host_dma_write(void *opaque, uint64_t addr, const void *buf,
                               size_t len)
{
    XdnaNpuState *s = opaque;

    return pci_dma_write(PCI_DEVICE(s), addr, buf, len) == MEMTX_OK ? 0 : -1;
}

static void xdna_host_raise_irq(void *opaque, unsigned vector)
{
    XdnaNpuState *s = opaque;
    PCIDevice *pdev = PCI_DEVICE(s);

    if (vector >= XDNA_MSIX_VECTORS) {
        return;
    }
    if (msix_enabled(pdev)) {
        msix_notify(pdev, vector);
    } else {
        pci_set_irq(pdev, 1);
    }
}

static void xdna_host_log(void *opaque, int level, const char *msg)
{
    if (level <= XDNA_LOG_WARN) {
        qemu_log_mask(LOG_GUEST_ERROR, "xdna-npu: %s\n", msg);
    } else {
        qemu_log_mask(LOG_TRACE, "xdna-npu: %s\n", msg);
    }
}

static const XdnaHostOps xdna_host_ops = {
    .dma_read = xdna_host_dma_read,
    .dma_write = xdna_host_dma_write,
    .raise_irq = xdna_host_raise_irq,
    .log = xdna_host_log,
};

/* ------------------------------------------------------------------ */
/* MMIO                                                                */
/* ------------------------------------------------------------------ */

typedef struct {
    XdnaNpuState *s;
    int bar;
} XdnaBarCtx;

static uint64_t xdna_bar_read(void *opaque, hwaddr addr, unsigned size)
{
    XdnaBarCtx *ctx = opaque;

    return xdna_npu_mmio_read(ctx->s->emu, ctx->bar, (uint32_t)addr, size);
}

static void xdna_bar_write(void *opaque, hwaddr addr, uint64_t val,
                           unsigned size)
{
    XdnaBarCtx *ctx = opaque;

    xdna_npu_mmio_write(ctx->s->emu, ctx->bar, (uint32_t)addr, val, size);
}

static const MemoryRegionOps xdna_bar_ops = {
    .read = xdna_bar_read,
    .write = xdna_bar_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/* Her BAR icin kucuk bir baglam; aygit omru boyunca yasar. */
static XdnaBarCtx *xdna_bar_ctx_new(XdnaNpuState *s, int bar)
{
    XdnaBarCtx *ctx = g_new0(XdnaBarCtx, 1);

    ctx->s = s;
    ctx->bar = bar;
    return ctx;
}

/* ------------------------------------------------------------------ */
/* Aygit yasam dongusu                                                 */
/* ------------------------------------------------------------------ */

static void xdna_npu_reset_hold(Object *obj, ResetType type)
{
    XdnaNpuState *s = XDNA_NPU(obj);

    xdna_npu_reset(s->emu);
    msix_reset(PCI_DEVICE(s));
}

static void xdna_npu_realize(PCIDevice *pdev, Error **errp)
{
    XdnaNpuState *s = XDNA_NPU(pdev);
    int ret;

    s->emu = xdna_npu_new(&xdna_host_ops, s);
    if (!s->emu) {
        error_setg(errp, "xdna-npu: emulator cekirdegi olusturulamadi");
        return;
    }

    /* Surucu 64-bit DMA maskesi kuruyor; BAR'lar da 64-bit. */
    memory_region_init_io(&s->bar_reg, OBJECT(s), &xdna_bar_ops,
                          xdna_bar_ctx_new(s, XDNA_BAR_REG),
                          "xdna-npu-reg", XDNA_BAR_REG_SIZE);
    memory_region_init_io(&s->bar_sram, OBJECT(s), &xdna_bar_ops,
                          xdna_bar_ctx_new(s, XDNA_BAR_SRAM),
                          "xdna-npu-sram", XDNA_BAR_SRAM_SIZE);
    memory_region_init_io(&s->bar_mbox, OBJECT(s), &xdna_bar_ops,
                          xdna_bar_ctx_new(s, XDNA_BAR_MBOX),
                          "xdna-npu-mbox", XDNA_BAR_MBOX_SIZE);

    pci_register_bar(pdev, XDNA_BAR_REG,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &s->bar_reg);
    pci_register_bar(pdev, XDNA_BAR_SRAM,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &s->bar_sram);
    pci_register_bar(pdev, XDNA_BAR_MBOX,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64, &s->bar_mbox);

    ret = msix_init(pdev, XDNA_MSIX_VECTORS,
                    &s->bar_reg, XDNA_BAR_REG, XDNA_MSIX_TABLE_OFFSET,
                    &s->bar_reg, XDNA_BAR_REG, XDNA_MSIX_PBA_OFFSET,
                    0, errp);
    if (ret < 0) {
        xdna_npu_free(s->emu);
        s->emu = NULL;
        return;
    }
    for (int i = 0; i < XDNA_MSIX_VECTORS; i++) {
        msix_vector_use(pdev, i);
    }

    /*
     * Sinif kodu (0x1200, "Processing accelerator") class_init icinde
     * ayarlaniyor; surucu buna bakmiyor ama lspci ciktisi anlamli oluyor.
     */
    pcie_endpoint_cap_init(pdev, 0);
}

static void xdna_npu_exit(PCIDevice *pdev)
{
    XdnaNpuState *s = XDNA_NPU(pdev);

    msix_uninit(pdev, &s->bar_reg, &s->bar_reg);
    xdna_npu_free(s->emu);
    s->emu = NULL;
}

static void xdna_npu_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    k->realize = xdna_npu_realize;
    k->exit = xdna_npu_exit;
    k->vendor_id = XDNA_PCI_VENDOR_ID;
    k->device_id = XDNA_PCI_DEVICE_ID_NPU1;
    k->revision = XDNA_PCI_REVISION_NPU1;
    k->class_id = 0x1200;  /* Processing accelerator */

    rc->phases.hold = xdna_npu_reset_hold;

    dc->desc = "AMD XDNA1 NPU (Phoenix / Hawk Point) fonksiyonel emulator";
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
}

static const TypeInfo xdna_npu_info = {
    .name          = TYPE_XDNA_NPU,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(XdnaNpuState),
    .class_init    = xdna_npu_class_init,
    .interfaces = (InterfaceInfo[]) {
        { INTERFACE_PCIE_DEVICE },
        { },
    },
};

static void xdna_npu_register_types(void)
{
    type_register_static(&xdna_npu_info);
}

type_init(xdna_npu_register_types)
