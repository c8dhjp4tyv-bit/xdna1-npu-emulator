/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Test kosumlari icin stock amdxdna surucu modeli.
 *
 * Buradaki kod kasten emulatorun ic yapilarina bakmaz: yalnizca MMIO okur
 * ve yazar, tipki gercek surucunun yaptigi gibi. Adimlar ve bekleme
 * kosullari aie2_pci.c / aie_psp.c / aie_smu.c / amdxdna_mailbox.c
 * fonksiyonlarindan turetilmistir.
 */

#ifndef DRV_MODEL_H
#define DRV_MODEL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "xdna/xdna_aie.h"
#include "xdna/xdna_emu.h"
#include "xdna/xdna_regs.h"

/* ---------------------------------------------------------------- */
/* Kucuk test cercevesi                                              */
/* ---------------------------------------------------------------- */

extern int g_failures;
extern int g_checks;

#define CHECK(cond, ...)                                                      \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_failures++;                                                     \
            printf("  [BASARISIZ] %s:%d: ", __FILE__, __LINE__);              \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

#define CHECK_EQ(a, b, name)                                                  \
    do {                                                                      \
        uint64_t _a = (uint64_t)(a), _b = (uint64_t)(b);                      \
        CHECK(_a == _b, "%s: 0x%llx beklendi, 0x%llx bulundu", name,          \
              (unsigned long long)_b, (unsigned long long)_a);                \
    } while (0)

void step(const char *msg);

/* ---------------------------------------------------------------- */
/* Sahte host bellegi                                                */
/* ---------------------------------------------------------------- */

#define HOST_MEM_BASE 0x100000000ULL
#define HOST_MEM_SIZE (8u * 1024u * 1024u)

/* Sahte npu.sbin imaji */
#define FW_PADDR (HOST_MEM_BASE + 0x10000)
#define FW_SIZE  0x40000u

typedef struct {
    uint8_t *mem;
    unsigned irq_count;
    unsigned last_vector;
    bool irq_pending[XDNA_MSIX_VECTORS];
    bool verbose;
} Host;

extern const XdnaHostOps drv_host_ops;

Host *host_new(bool verbose);
void host_free(Host *h);
/* Host bellegine dogrudan erisim (test verisi hazirlamak icin). */
uint8_t *host_ptr(Host *h, uint64_t addr);

/* ---------------------------------------------------------------- */
/* Surucu modeli                                                     */
/* ---------------------------------------------------------------- */

typedef struct {
    uint32_t rb_start;      /* SRAM BAR offseti */
    uint32_t rb_size;
    uint32_t head_reg;      /* MBOX BAR offseti */
    uint32_t tail_reg;
} ChannRes;

typedef struct {
    XdnaNpu *npu;
    Host *host;

    ChannRes x2i;
    ChannRes i2x;
    uint32_t intr_reg;
    uint32_t msix_id;
    uint32_t prot_major, prot_minor;

    uint32_t x2i_tail;
    uint32_t i2x_head;
    uint32_t next_id;
} Drv;

uint32_t rd32(Drv *d, int bar, uint32_t off);
void wr32(Drv *d, int bar, uint32_t off, uint32_t val);

int psp_exec(Drv *d, uint32_t cmd, uint32_t arg0, uint32_t arg1, uint32_t arg2);
int smu_exec(Drv *d, uint32_t cmd, uint32_t arg, uint32_t *out);

int mbox_send_recv(Drv *d, uint32_t opcode, const void *req, uint32_t req_size,
                   void *resp, uint32_t resp_size);

/*
 * Cevap beklemeden gonder; mesaj id'sini dondurur. Firmware'in hemen
 * cevaplamadigi mesajlar icin (REGISTER_ASYNC_EVENT_MSG).
 */
uint32_t mbox_send_only(Drv *d, uint32_t opcode, const void *req,
                        uint32_t req_size);

/* Bekleyen bir cevabi tuket. */
int mbox_recv_pending(Drv *d, uint32_t want_id, void *resp,
                      uint32_t resp_size);

int drv_get_mgmt_chann_info(Drv *d);
/* SMU guc dizisi + PSP firmware yukleme + yonetim kanali kurulumu. */
int drv_hw_start(Drv *d);

/* ---------------------------------------------------------------- */
/* Surucunun kullandigi mesaj yapilari                               */
/* ---------------------------------------------------------------- */

#pragma pack(push, 1)
typedef struct { uint32_t status, major, minor, sub, build; } FwVerResp;
typedef struct { uint32_t status; uint16_t major, minor; } AieVerResp;
typedef struct {
    uint32_t size;
    uint16_t major, minor, cols, rows, core_rows, mem_rows, shim_rows;
    uint16_t core_row_start, mem_row_start, shim_row_start;
    uint16_t core_dma_channels, mem_dma_channels, shim_dma_channels;
    uint16_t core_locks, mem_locks, shim_locks;
    uint16_t core_events, mem_events, shim_events, reserved;
} TileInfo;
typedef struct { uint32_t status; TileInfo info; } TileInfoResp;
typedef struct { uint32_t status; } StatusResp;
typedef struct { uint32_t type; uint64_t value; } SetRtCfgReq;
typedef struct { uint16_t pasid, reserved; } PasidReq;
typedef struct { uint32_t head_addr, tail_addr, buf_addr, buf_size; } CqInfo;
typedef struct { CqInfo x2i_q, i2x_q; } CqPair;
typedef struct {
    uint32_t aie_type;
    uint8_t start_col, num_col, num_unused_col, reserved;
    uint8_t num_cq_pairs_requested, reserved1;
    uint16_t pasid;
    uint32_t pad[2];
    uint32_t sec_comm_target_type;
    uint32_t context_priority;
} CreateCtxReq;
typedef struct {
    uint32_t status, context_id;
    uint16_t msix_id;
    uint8_t num_cq_pairs_allocated, reserved;
    CqPair cq_pair[2];
} CreateCtxResp;
typedef struct { uint32_t context_id; } DestroyCtxReq;
typedef struct { uint32_t context_id; uint64_t buf_addr, buf_size; } MapBufReq;
typedef struct { uint32_t num_cus; uint32_t cfgs[32]; } ConfigCuReq;
typedef struct {
    uint64_t inst_buf_addr;
    uint32_t inst_size, inst_prop_cnt, cu_idx;
    uint32_t payload[35];
} ExecDpuReq;
typedef struct { uint64_t buf_addr; uint32_t buf_size, count; } CmdChainReq;
typedef struct { uint32_t status, fail_cmd_idx, fail_cmd_status; } CmdChainResp;
typedef struct { uint64_t src_addr, dst_addr; uint32_t size, type; } SyncBoReq;
#define SYNC_BO_DEV_MEM_T  0u
#define SYNC_BO_HOST_MEM_T 2u
#pragma pack(pop)

/* cmd_chain_slot_dpu -- surucude packed degil */
typedef struct {
    uint64_t inst_buf_addr;
    uint32_t inst_size, inst_prop_cnt, cu_idx, arg_cnt;
} CmdChainSlotDpu;

/*
 * Bir hwctx kanalini surucunun yaptigi gibi kur: CREATE_CONTEXT cevabindaki
 * cq_pair bilgisinden kanal kaynaklarini cikarir ve `out` icine yazar.
 */
void drv_attach_ctx_channel(const CreateCtxResp *resp, Drv *out, Drv *mgmt);

#endif /* DRV_MODEL_H */
