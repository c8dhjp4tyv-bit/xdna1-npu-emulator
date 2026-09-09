/* SPDX-License-Identifier: GPL-2.0-only */
/* libxdna dahili durum tanimlari. */

#ifndef XDNA_INTERNAL_H
#define XDNA_INTERNAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "xdna/xdna_emu.h"
#include "xdna/xdna_regs.h"

#if defined(__GNUC__) || defined(__clang__)
#define XDNA_PRINTF_ATTR(fmt_idx, arg_idx) \
    __attribute__((format(printf, fmt_idx, arg_idx)))
#else
#define XDNA_PRINTF_ATTR(fmt_idx, arg_idx)
#endif

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
} XdnaContext;

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

    /* Async event kaydi (MSG_OP_REGISTER_ASYNC_EVENT_MSG) */
    uint64_t async_buf_addr;
    uint32_t async_buf_size;
    bool async_registered;

    XdnaStats stats;
};

/* --- ic API --- */

void xdna_log(XdnaNpu *npu, int level, const char *fmt, ...)
    XDNA_PRINTF_ATTR(3, 4);

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
