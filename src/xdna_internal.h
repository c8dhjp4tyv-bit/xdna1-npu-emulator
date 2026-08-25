/* SPDX-License-Identifier: GPL-2.0-only */
/* libxdna dahili durum tanimlari. */

#ifndef XDNA_INTERNAL_H
#define XDNA_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "xdna/xdna_aie.h"
#include "xdna/xdna_emu.h"
#include "xdna/xdna_regs.h"

#define XDNA_MAX_CHANNELS (1u + XDNA_NPU1_HWCTX_LIMIT) /* mgmt + 6 hwctx */

/*
 * Mailbox mesaj basligi -- amdxdna_mailbox.c: struct xdna_msg_header.
 * Ring buffer'da bu basligin ardindan `total_size` bayt payload gelir.
 */
typedef struct XdnaMsgHeader {
    uint32_t total_size;
    uint32_t sz_ver;
    uint32_t id;
    uint32_t opcode;
} XdnaMsgHeader;

/*
 * Firmware'in SRAM'e yazdigi yonetim kanali tanimi.
 * aie2_pci.c: struct mgmt_mbox_chann_info (64 bayt).
 * Adresler CIHAZ adresleridir (aperture tabani dahil); surucu bunlari
 * AIE2_SRAM_OFF() / AIE2_MBOX_OFF() ile BAR offsetine cevirir.
 */
typedef struct XdnaMgmtMboxInfo {
    uint32_t x2i_tail;
    uint32_t x2i_head;
    uint32_t x2i_buf;
    uint32_t x2i_buf_sz;
    uint32_t i2x_tail;
    uint32_t i2x_head;
    uint32_t i2x_buf;
    uint32_t i2x_buf_sz;
    uint32_t magic;
    uint32_t msi_id;
    uint32_t prot_major;
    uint32_t prot_minor;
    uint32_t rsvd[4];
} XdnaMgmtMboxInfo;

/* Bir mailbox kanalinin cihaz tarafindaki durumu. */
typedef struct XdnaChannel {
    bool active;
    unsigned index;      /* MSI-X vektoru ile ayni                  */
    uint32_t x2i_head;   /* cihaz ilerletir, surucu okur            */
    uint32_t x2i_tail;   /* surucu yazar                            */
    uint32_t i2x_head;   /* surucu yazar                            */
    uint32_t i2x_tail;   /* cihaz ilerletir                         */
    uint32_t intr;       /* iohub interrupt status                  */
    uint32_t x2i_buf;    /* SRAM offseti                            */
    uint32_t x2i_size;
    uint32_t i2x_buf;    /* SRAM offseti                            */
    uint32_t i2x_size;
} XdnaChannel;

/* Bir hardware context (ERT partition). */
typedef struct XdnaContext {
    bool valid;
    uint32_t id;
    uint16_t pasid;
    uint8_t start_col;
    uint8_t num_col;
    uint32_t priority;
    unsigned chan;        /* kanal / MSI-X indeksi */
    uint64_t heap_addr;   /* MAP_HOST_BUFFER ile bildirilen instruction heap */
    uint64_t heap_size;

    /* CONFIG_CU ile bildirilen CU -> PDI eslemesi */
    uint32_t num_cus;
    uint32_t cu_cfg[32];  /* aie2_msg_priv.h: MAX_NUM_CUS */
} XdnaContext;

/* ---------------------------------------------------------------- */
/* XDNA array modeli (src/xdna_array.c)                              */
/* ---------------------------------------------------------------- */

typedef enum {
    AIE_TILE_SHIM = 0,
    AIE_TILE_MEM,
    AIE_TILE_CORE,
} AieTileKind;

enum {
    AIE_DMA_S2MM = 0,   /* stream -> bellek */
    AIE_DMA_MM2S = 1,   /* bellek -> stream */
    AIE_DMA_NUM_DIR
};

typedef struct {
    uint32_t off;
    uint32_t val;
} AieRegEntry;

/* Modellenmeyen registerlar icin seyrek saklama. */
typedef struct {
    AieRegEntry *tab;
    uint32_t cap;
    uint32_t used;
} AieRegMap;

/* Stream verisi icin basit FIFO. */
typedef struct {
    uint8_t *buf;
    size_t cap;
    size_t len;
    size_t rd;
} AieStream;

/* Stream switch port sinifi */
typedef enum {
    AIE_PC_NONE = 0,
    AIE_PC_CORE,
    AIE_PC_DMA,
    AIE_PC_CTRL,
    AIE_PC_FIFO,
    AIE_PC_SOUTH,
    AIE_PC_WEST,
    AIE_PC_NORTH,
    AIE_PC_EAST,
    AIE_PC_TRACE,
} AiePortClass;

typedef struct {
    uint8_t cls;   /* AiePortClass */
    uint8_t idx;   /* sinif icindeki alt indeks */
} AiePortDesc;

typedef struct {
    AieTileKind kind;
    uint8_t col, row;

    uint8_t *data;          /* mem tile / compute tile veri bellegi */
    uint32_t data_size;
    uint8_t *prog;          /* compute tile program bellegi */
    uint32_t prog_size;

    int32_t lock[AIE_MAX_LOCKS];
    uint32_t bd[AIE_MAX_BDS][AIE_BD_WORDS];
    uint32_t ch_ctrl[AIE_DMA_NUM_DIR][AIE_MAX_DMA_CH];
    uint32_t ch_queue[AIE_DMA_NUM_DIR][AIE_MAX_DMA_CH];

    uint32_t core_ctrl;
    uint32_t core_status;

    /* Stream switch */
    uint32_t ss_master[AIE_SS_MAX_PORTS];
    uint32_t ss_slave[AIE_SS_MAX_PORTS];
    uint32_t shim_mux;      /* yalnizca shim */
    uint32_t shim_demux;    /* yalnizca shim */

    /* S2MM kanal basina giris FIFO'su (stream switch'ten gelen veri) */
    AieStream in_fifo[AIE_MAX_DMA_CH];

    AieRegMap regs;
} AieTile;

typedef struct {
    uint64_t dma_tasks;
    uint64_t dma_bytes;
    uint64_t lock_ops;
    uint64_t core_starts;
    uint64_t txn_ops;
    uint64_t stream_hops;
    uint64_t stream_drops;
} AieStats;

typedef struct XdnaArray {
    XdnaNpu *npu;
    AieTile tile[AIE_NUM_COLS][AIE_NUM_ROWS];
    AieStats stats;
} XdnaArray;

XdnaArray *xdna_array_new(XdnaNpu *npu);
void xdna_array_free(XdnaArray *arr);
void xdna_array_reset(XdnaArray *arr);
AieTile *xdna_array_tile(XdnaArray *arr, uint8_t col, uint8_t row);
uint32_t xdna_array_read32(XdnaArray *arr, uint8_t col, uint8_t row,
                           uint32_t off);
void xdna_array_write32(XdnaArray *arr, uint8_t col, uint8_t row, uint32_t off,
                        uint32_t val);
void xdna_array_block_write(XdnaArray *arr, uint8_t col, uint8_t row,
                            uint32_t off, const uint32_t *data, uint32_t words);

/* ctrlcode yurutucu (src/xdna_txn.c) */
int xdna_txn_execute(XdnaNpu *npu, uint32_t ctx_id, const uint8_t *buf,
                     uint32_t size);

typedef enum {
    XDNA_PSP_COLD = 0,   /* guc yok / firmware yuklenmedi */
    XDNA_PSP_VALIDATED,  /* PSP_VALIDATE gecti            */
    XDNA_PSP_RUNNING,    /* PSP_START gecti, MERT ayakta  */
} XdnaPspState;

struct XdnaNpu {
    const XdnaHostOps *ops;
    void *opaque;

    /* --- REG BAR (BAR0) --- */
    uint32_t scratch[10];      /* SCRATCH0..SCRATCH9 */
    uint32_t pwaitmode;
    uint32_t sec_intr;         /* PSP kick        */
    uint32_t pwrmgmt_intr;     /* SMU kick        */

    /* --- SRAM BAR (BAR2) --- */
    uint8_t *sram;

    /* --- MBOX BAR (BAR4) --- */
    XdnaChannel chan[XDNA_MAX_CHANNELS];

    /* --- Firmware / guc durumu --- */
    XdnaPspState psp_state;
    bool power_on;
    bool fw_alive;
    bool fw_suspended;
    uint64_t fw_paddr;         /* PSP_VALIDATE ile bildirilen host adresi */
    uint32_t fw_size;
    uint32_t npuclk;
    uint32_t hclk;
    uint32_t dpm_level;
    uint16_t mgmt_pasid;

    /* Runtime config (MSG_OP_SET/GET_RUNTIME_CONFIG) */
    uint64_t rt_cfg[64];

    /* Kaynak durumu */
    XdnaContext ctx[XDNA_NPU1_HWCTX_LIMIT];
    uint32_t next_ctx_id;
    uint32_t col_used;         /* kolon bitmap'i */

    /* XDNA array */
    XdnaArray *array;

    /* Async event kaydi (MSG_OP_REGISTER_ASYNC_EVENT_MSG) */
    uint64_t async_buf_addr;
    uint32_t async_buf_size;
    bool async_registered;

    XdnaStats stats;
};

/* --- ic API --- */

void xdna_log(XdnaNpu *npu, int level, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));

/* PSP / SMU durum makineleri */
void xdna_psp_kick(XdnaNpu *npu);
void xdna_smu_kick(XdnaNpu *npu);

/* Firmware boot: SRAM'e mgmt kanal bilgisini yazip FW_ALIVE'i kaldirir. */
void xdna_fw_boot(XdnaNpu *npu);
void xdna_fw_shutdown(XdnaNpu *npu);

/* Mailbox */
void xdna_mbox_reset(XdnaNpu *npu);
void xdna_mbox_channel_reset(XdnaNpu *npu, unsigned idx);
uint32_t xdna_mbox_reg_read(XdnaNpu *npu, uint32_t off);
void xdna_mbox_reg_write(XdnaNpu *npu, uint32_t off, uint32_t val);
/* Kanaldaki bekleyen x2i mesajlarini isler. */
void xdna_mbox_process(XdnaNpu *npu, unsigned chan_idx);
/* Cihazdan surucuye mesaj gonderir (cevap veya async event). */
int xdna_mbox_send(XdnaNpu *npu, unsigned chan_idx, uint32_t id,
                   uint32_t opcode, const void *payload, uint32_t len);

/* MERT: yonetim firmware'inin mesaj isleyicisi. */
void xdna_mert_handle(XdnaNpu *npu, unsigned chan_idx,
                      const XdnaMsgHeader *hdr, const uint8_t *payload);

/* SRAM yardimcilari */
static inline uint32_t xdna_sram_read32(XdnaNpu *npu, uint32_t off)
{
    uint32_t v;
    if (off + 4 > XDNA_BAR_SRAM_SIZE) {
        return 0;
    }
    memcpy(&v, npu->sram + off, 4);
    return v;
}

static inline void xdna_sram_write32(XdnaNpu *npu, uint32_t off, uint32_t v)
{
    if (off + 4 > XDNA_BAR_SRAM_SIZE) {
        return;
    }
    memcpy(npu->sram + off, &v, 4);
}

/* Cihaz adresi <-> BAR offseti */
static inline uint32_t xdna_sram_dev_addr(uint32_t off)
{
    return MPNPU_APERTURE1_BASE + off;
}

static inline uint32_t xdna_mbox_dev_addr(uint32_t off)
{
    return MPNPU_APERTURE2_BASE + off;
}

#endif /* XDNA_INTERNAL_H */
