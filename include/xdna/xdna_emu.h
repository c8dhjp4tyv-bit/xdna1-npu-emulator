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
    /* Cihazin DMA yolu (IOMMU'dan gecer). 0 = basarili, <0 = hata. */
    int (*dma_read)(void *opaque, uint64_t addr, void *buf, size_t len);
    int (*dma_write)(void *opaque, uint64_t addr, const void *buf, size_t len);
    /*
     * Platform fiziksel bellek okumasi -- IOMMU'dan GECMEZ.
     * PSP firmware yukleme yolu icin: surucu firmware tamponunun adresini
     * virt_to_phys() ile veriyor, yani DMA API'sini atliyor. Bunu cihazin
     * cevrilmis DMA yolundan okumak, IOMMU ceviri modundayken sayfa
     * hatasina yol acar. Gercek donanimda PSP fiziksel bellege dogrudan
     * erisir. NULL ise dma_read'e dusulur.
     */
    int (*phys_read)(void *opaque, uint64_t addr, void *buf, size_t len);
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
    uint64_t exec_cmds;
    uint64_t dma_bytes;
    uint64_t txn_ops;
    uint64_t array_errors;
    uint64_t core_fetches;
    uint64_t core_halts;
    uint32_t active_contexts;
    int fw_alive;
    int power_on;
} XdnaStats;

void xdna_npu_get_stats(const XdnaNpu *npu, XdnaStats *out);

/*
 * AIE2 VLIW paket uzunlugu cozucusu. Ilk 32-bit kelimeden paketin bayt
 * cinsinden boyutunu dondurur (2,4,6,8,10,12,14,16); tanimsiz desende 0.
 * Kodlama Xilinx/llvm-aie AIE2CompositeFormats.td'den alinmistir.
 * Saf fonksiyon; ayri test edilebilsin diye disariya aciliyor.
 */
uint32_t xdna_aie2_packet_size(uint32_t first_word);

/*
 * AIE2 VLIW bundle (composite paket) slot cozucusu.
 *
 * AIE2 bir VLIW: her paket, birkac islev biriminin ("slot") alanlarini yan
 * yana tasiyor. Hangi slotlarin bulundugunu ve hangi bit araliklarinda
 * durduklarini "composite format" belirliyor; formati de paketin icindeki
 * sabit bitler ayirt ediyor.
 *
 * Slot kimlikleri Xilinx/llvm-aie AIE2CompositeFormats.td'deki adlarla
 * birebir ayni:
 *   lda / ldb  yukleme birimleri      st   saklama birimi
 *   alu        skaler ALU             mv   tasima birimi
 *   vec        vektor birimi          lng  alu+mv yerine gecen uzun alan
 *   nop        tek bitlik dolgu
 */
typedef enum {
    AIE2_SLOT_LDA = 0,
    AIE2_SLOT_LDB,
    AIE2_SLOT_ST,
    AIE2_SLOT_ALU,
    AIE2_SLOT_MV,
    AIE2_SLOT_LNG,
    AIE2_SLOT_VEC,
    AIE2_SLOT_NOP,
    AIE2_SLOT_KINDS
} Aie2SlotKind;

#define AIE2_MAX_SLOTS 6u

typedef struct {
    Aie2SlotKind kind;
    uint8_t width;      /* bit cinsinden slot genisligi */
    uint64_t value;     /* slottan cikarilmis ham alan */
} Aie2Slot;

typedef struct {
    uint32_t size;              /* paket boyutu, bayt */
    const char *format;         /* eslesen composite format adi */
    unsigned nslots;
    Aie2Slot slot[AIE2_MAX_SLOTS];  /* en anlamli bitten en az anlamliya */
} Aie2Bundle;

/*
 * `bytes` ile baslayan `avail` baytlik bellekten bir bundle coz.
 *
 * Basarili olursa 0 doner ve *out doldurulur. Paket uzunlugu cozulemezse,
 * `avail` yetmezse veya hicbir composite format eslesmezse -1 doner --
 * eslesmeyen desen gecersiz bir kodlamadir, "sessizce atlanacak" bir sey
 * degil.
 *
 * Slot adi -> okunabilir metin icin xdna_aie2_slot_name().
 */
int xdna_aie2_decode(const uint8_t *bytes, size_t avail, Aie2Bundle *out);

const char *xdna_aie2_slot_name(Aie2SlotKind kind);

#endif /* XDNA_EMU_H */
