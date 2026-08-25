// SPDX-License-Identifier: GPL-2.0-only
/*
 * MERT -- yonetim firmware'inin davranis modeli.
 *
 * Gercek Hawk Point'te MERT, NPU mikrodenetleyicisinde kosan ayricalikli
 * firmware'dir: sorgulari cevaplar, workload context'lerini (ERT) yaratir ve
 * yok eder, guc/telemetri islerini yurutur. Burada onu instruction seviyesinde
 * degil, MESAJ seviyesinde emule ediyoruz.
 *
 * KRITIK KISIT (amdxdna_mailbox_helper.c xdna_msg_cb):
 *   Cevap payload'inin boyutu, surucudeki `struct <name>_resp` boyutuna
 *   BIREBIR esit olmalidir; aksi halde surucu -EINVAL dondurur. Bu yuzden
 *   asagidaki tum struct'lar packed ve boyutlari static_assert ile kilitli.
 */

#include <assert.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "xdna_internal.h"

#pragma pack(push, 1)

typedef struct {
    uint32_t status;
    uint32_t major;
    uint32_t minor;
} ProtocolVersionResp;

typedef struct {
    uint32_t status;
    uint32_t major;
    uint32_t minor;
    uint32_t sub;
    uint32_t build;
} FirmwareVersionResp;

typedef struct {
    uint32_t status;
    uint16_t major;
    uint16_t minor;
} AieVersionResp;

/* struct aie_tile_info -- surucude packed DEGIL; u32 + 20 x u16 = 44 bayt. */
typedef struct {
    uint32_t size;
    uint16_t major;
    uint16_t minor;
    uint16_t cols;
    uint16_t rows;
    uint16_t core_rows;
    uint16_t mem_rows;
    uint16_t shim_rows;
    uint16_t core_row_start;
    uint16_t mem_row_start;
    uint16_t shim_row_start;
    uint16_t core_dma_channels;
    uint16_t mem_dma_channels;
    uint16_t shim_dma_channels;
    uint16_t core_locks;
    uint16_t mem_locks;
    uint16_t shim_locks;
    uint16_t core_events;
    uint16_t mem_events;
    uint16_t shim_events;
    uint16_t reserved;
} AieTileInfo;

typedef struct {
    uint32_t status;
    AieTileInfo info;
} AieTileInfoResp;

typedef struct {
    uint32_t status;
} StatusOnlyResp;

typedef struct {
    uint32_t type;
    uint64_t value;
} SetRuntimeCfgReq;

typedef struct {
    uint32_t type;
} GetRuntimeCfgReq;

typedef struct {
    uint32_t status;
    uint64_t value;
} GetRuntimeCfgResp;

typedef struct {
    uint16_t pasid;
    uint16_t reserved;
} AssignPasidReq;

typedef struct {
    uint32_t head_addr;
    uint32_t tail_addr;
    uint32_t buf_addr;
    uint32_t buf_size;
} CqInfo;

typedef struct {
    CqInfo x2i_q;
    CqInfo i2x_q;
} CqPair;

typedef struct {
    uint32_t aie_type;
    uint8_t start_col;
    uint8_t num_col;
    uint8_t num_unused_col;
    uint8_t reserved;
    uint8_t num_cq_pairs_requested;
    uint8_t reserved1;
    uint16_t pasid;
    uint32_t pad[2];
    uint32_t sec_comm_target_type;
    uint32_t context_priority;
} CreateCtxReq;

typedef struct {
    uint32_t status;
    uint32_t context_id;
    uint16_t msix_id;
    uint8_t num_cq_pairs_allocated;
    uint8_t reserved;
    CqPair cq_pair[2];
} CreateCtxResp;

typedef struct {
    uint32_t context_id;
} DestroyCtxReq;

typedef struct {
    uint32_t context_id;
    uint64_t buf_addr;
    uint64_t buf_size;
} MapHostBufferReq;

typedef struct {
    uint64_t buf_addr;
    uint32_t buf_size;
} AsyncEventReq;

typedef struct {
    uint64_t dump_buff_addr;
    uint32_t dump_buff_size;
    uint32_t num_cols;
    uint32_t aie_bitmap;
} ColumnInfoReq;

typedef struct {
    uint32_t status;
    uint32_t size;
} ColumnInfoResp;

typedef struct {
    uint32_t type;
    uint64_t buf_addr;
    uint32_t buf_size;
} TelemetryReq;

typedef struct {
    uint32_t major;
    uint32_t minor;
    uint32_t size;
    uint32_t status;
} TelemetryResp;

/* --- Yurutme yolu --- */

#define MAX_NUM_CUS 32

typedef struct {
    uint32_t num_cus;
    uint32_t cfgs[MAX_NUM_CUS];
} ConfigCuReq;

typedef struct {
    uint64_t inst_buf_addr;
    uint32_t inst_size;      /* bayt (amdxdna_ctx.h) */
    uint32_t inst_prop_cnt;
    uint32_t cu_idx;
    uint32_t payload[35];
} ExecDpuReq;

typedef struct {
    uint64_t buf_addr;
    uint32_t buf_size;
    uint32_t count;
} CmdChainReq;

typedef struct {
    uint32_t status;
    uint32_t fail_cmd_idx;
    uint32_t fail_cmd_status;
} CmdChainResp;

#define SYNC_BO_DEV_MEM  0u
#define SYNC_BO_HOST_MEM 2u

typedef struct {
    uint64_t src_addr;
    uint64_t dst_addr;
    uint32_t size;
    uint32_t type;
} SyncBoReq;

#pragma pack(pop)

/*
 * cmd_chain_slot_dpu surucude PACKED DEGIL: u64 hizalamasi geciyor.
 * args[] slot'un hemen ardindan geliyor.
 */
typedef struct {
    uint64_t inst_buf_addr;
    uint32_t inst_size;
    uint32_t inst_prop_cnt;
    uint32_t cu_idx;
    uint32_t arg_cnt;
} CmdChainSlotDpu;

/* Emulator sinirlari: bozuk bir istegin bellegi tuketmesini engeller. */
#define XDNA_MAX_CTRLCODE_SIZE   (64u * 1024u * 1024u)  /* context heap boyutu */
#define XDNA_MAX_CMD_CHAIN_SIZE  (1u * 1024u * 1024u)

/* Surucudeki resp struct boyutlariyla birebir eslesme kilidi. */
static_assert(sizeof(ProtocolVersionResp) == 12, "protocol_version_resp");
static_assert(sizeof(FirmwareVersionResp) == 20, "firmware_version_resp");
static_assert(sizeof(AieVersionResp) == 8, "aie_version_info_resp");
static_assert(sizeof(AieTileInfo) == 44, "aie_tile_info");
static_assert(sizeof(AieTileInfoResp) == 48, "aie_tile_info_resp");
static_assert(sizeof(StatusOnlyResp) == 4, "status-only resp");
static_assert(sizeof(GetRuntimeCfgResp) == 12, "get_runtime_cfg_resp");
static_assert(sizeof(CreateCtxReq) == 28, "create_ctx_req");
static_assert(sizeof(CreateCtxResp) == 76, "create_ctx_resp");
static_assert(sizeof(MapHostBufferReq) == 20, "map_host_buffer_req");
static_assert(sizeof(AsyncEventResp) == 8, "async_event_msg_resp");
static_assert(sizeof(ColumnInfoResp) == 8, "aie_column_info_resp");
static_assert(sizeof(TelemetryResp) == 16, "get_telemetry_resp");
static_assert(sizeof(ConfigCuReq) == 132, "config_cu_req");
static_assert(sizeof(ExecDpuReq) == 160, "exec_dpu_req");
static_assert(sizeof(CmdChainReq) == 16, "cmd_chain_req");
static_assert(sizeof(CmdChainResp) == 12, "cmd_chain_resp");
static_assert(sizeof(SyncBoReq) == 24, "sync_bo_req");
static_assert(sizeof(CmdChainSlotDpu) == 24, "cmd_chain_slot_dpu");

/*
 * TODO(dogrula): QUERY_COL_STATUS icin kolon basina dump boyutu. Surucu bunu
 * metadata.col_size olarak saklar ve XRT'ye aktarir. Gercek Hawk Point
 * degeriyle karsilastirilmali.
 */
#define XDNA_COL_STATUS_SIZE 0x2000u

/* ---------------------------------------------------------------- */

static void mert_reply(XdnaNpu *npu, unsigned chan, const XdnaMsgHeader *hdr,
                       const void *payload, uint32_t len)
{
    xdna_mbox_send(npu, chan, hdr->id, hdr->opcode, payload, len);
}

/*
 * Bir opcode'un cevap boyutu, HATA durumunda bile degismez: surucu
 * xdna_msg_cb() icinde boyutu birebir karsilastirir. Kisa bir "sadece
 * status" cevabi gondermek, surucuye gercek hata kodu yerine -EINVAL
 * dondurur. Bu tablo her opcode icin cevap boyutunu ve status alaninin
 * offsetini verir.
 */
static uint32_t resp_size_for(uint32_t opcode, uint32_t *status_off)
{
    *status_off = 0;
    switch (opcode) {
    case MSG_OP_GET_PROTOCOL_VERSION:
        return sizeof(ProtocolVersionResp);
    case MSG_OP_GET_FIRMWARE_VERSION:
        return sizeof(FirmwareVersionResp);
    case MSG_OP_QUERY_AIE_VERSION:
        return sizeof(AieVersionResp);
    case MSG_OP_QUERY_AIE_TILE_INFO:
        return sizeof(AieTileInfoResp);
    case MSG_OP_QUERY_COL_STATUS:
        return sizeof(ColumnInfoResp);
    case MSG_OP_GET_RUNTIME_CONFIG:
        return sizeof(GetRuntimeCfgResp);
    case MSG_OP_GET_TELEMETRY:
        /* get_telemetry_resp'te status EN SONDA. */
        *status_off = offsetof(TelemetryResp, status);
        return sizeof(TelemetryResp);
    case MSG_OP_CREATE_CONTEXT:
        return sizeof(CreateCtxResp);
    case MSG_OP_REGISTER_ASYNC_EVENT_MSG:
        return sizeof(AsyncEventResp);
    case MSG_OP_GET_APP_HEALTH:
        return 36;  /* get_app_health_resp: status + u32 + u32[7] */
    case MSG_OP_GET_DEV_REVISION:
        return 12;  /* get_dev_revision_resp: status + rev + raw_fuse_data */
    case MSG_OP_CHAIN_EXEC_BUFFER_CF:
    case MSG_OP_CHAIN_EXEC_DPU:
    case MSG_OP_CHAIN_EXEC_NPU:
        return 12;  /* cmd_chain_resp */
    default:
        /*
         * Geri kalan opcode'lar "sadece status" cevabi kullanir. Tanimadigimiz
         * bir opcode gelirse de bu yolu seciyoruz: boyut tutmazsa surucu
         * -EINVAL gorur, ki bilinmeyen bir komut icin dogru sonuc budur.
         */
        return sizeof(StatusOnlyResp);
    }
}

static void mert_reply_status(XdnaNpu *npu, unsigned chan,
                              const XdnaMsgHeader *hdr, uint32_t status)
{
    uint8_t buf[128] = { 0 };
    uint32_t status_off;
    uint32_t size = resp_size_for(hdr->opcode, &status_off);

    if (size > sizeof(buf)) {
        xdna_log(npu, XDNA_LOG_ERROR, "MERT: cevap tamponu kucuk (%u)", size);
        return;
    }
    memcpy(buf + status_off, &status, sizeof(status));
    mert_reply(npu, chan, hdr, buf, size);
}

static void fill_tile_info(AieTileInfo *info)
{
    memset(info, 0, sizeof(*info));
    info->size = XDNA_COL_STATUS_SIZE;
    info->major = XDNA_AIE_VERSION_MAJOR;
    info->minor = XDNA_AIE_VERSION_MINOR;
    info->cols = XDNA_AIE_COLS;
    info->rows = XDNA_AIE_ROWS;
    info->core_rows = XDNA_AIE_CORE_ROWS;
    info->mem_rows = XDNA_AIE_MEM_ROWS;
    info->shim_rows = XDNA_AIE_SHIM_ROWS;
    info->core_row_start = XDNA_AIE_CORE_ROW_START;
    info->mem_row_start = XDNA_AIE_MEM_ROW_START;
    info->shim_row_start = XDNA_AIE_SHIM_ROW_START;
    /* aie-rt xaie_lite_hwcfg.h (AIE_GEN 2) ile dogrulandi. */
    info->core_dma_channels = AIE_TILE_DMA_NUM_CH;
    info->mem_dma_channels = AIE_MEM_TILE_DMA_NUM_CH;
    info->shim_dma_channels = AIE_SHIM_DMA_NUM_CH;
    info->core_locks = AIE_TILE_NUM_LOCKS;
    info->mem_locks = AIE_MEM_TILE_NUM_LOCKS;
    info->shim_locks = AIE_SHIM_NUM_LOCKS;
    info->core_events = 128;
    info->mem_events = 128;
    info->shim_events = 128;
}

/* ---------------------------------------------------------------- */
/* Context yonetimi                                                  */
/* ---------------------------------------------------------------- */

static XdnaContext *ctx_by_id(XdnaNpu *npu, uint32_t id)
{
    unsigned i;

    for (i = 0; i < XDNA_NPU1_HWCTX_LIMIT; i++) {
        if (npu->ctx[i].valid && npu->ctx[i].id == id) {
            return &npu->ctx[i];
        }
    }
    return NULL;
}

static void handle_create_context(XdnaNpu *npu, unsigned chan,
                                  const XdnaMsgHeader *hdr,
                                  const uint8_t *payload)
{
    CreateCtxReq req;
    CreateCtxResp resp;
    XdnaContext *ctx = NULL;
    XdnaChannel *cch;
    unsigned slot, chan_idx, rb_base, chan_base;

    if (hdr->total_size < sizeof(req)) {
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
        return;
    }
    memcpy(&req, payload, sizeof(req));

    for (slot = 0; slot < XDNA_NPU1_HWCTX_LIMIT; slot++) {
        if (!npu->ctx[slot].valid) {
            ctx = &npu->ctx[slot];
            break;
        }
    }
    if (!ctx) {
        xdna_log(npu, XDNA_LOG_ERROR, "MERT: context limiti (%u) doldu",
                 XDNA_NPU1_HWCTX_LIMIT);
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_MGMT_ERT_NOAVAIL);
        return;
    }
    if (req.num_col == 0 || req.num_col > XDNA_AIE_COLS) {
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_MGMT_ERT_INVALID_PARAM);
        return;
    }

    /* Kanal indeksi 1..6; 0 yonetim kanalina ayrilmistir. */
    chan_idx = slot + 1;
    rb_base = XDNA_SRAM_CTX_RB_BASE + slot * XDNA_SRAM_CTX_RB_STRIDE;
    chan_base = XDNA_MBOX_CHAN_BASE(chan_idx);

    cch = &npu->chan[chan_idx];
    xdna_mbox_channel_reset(npu, chan_idx);
    cch->active = true;
    cch->index = chan_idx;
    cch->x2i_buf = rb_base;
    cch->x2i_size = XDNA_SRAM_CTX_RB_SIZE;
    cch->i2x_buf = rb_base + XDNA_SRAM_CTX_RB_SIZE;
    cch->i2x_size = XDNA_SRAM_CTX_RB_SIZE;
    memset(npu->sram + rb_base, 0, XDNA_SRAM_CTX_RB_STRIDE);

    ctx->valid = true;
    ctx->id = npu->next_ctx_id++;
    ctx->pasid = req.pasid;
    ctx->start_col = req.start_col;
    ctx->num_col = req.num_col;
    ctx->priority = req.context_priority;
    ctx->chan = chan_idx;
    ctx->heap_addr = 0;
    ctx->heap_size = 0;
    npu->stats.active_contexts++;

    memset(&resp, 0, sizeof(resp));
    resp.status = AIE2_STATUS_SUCCESS;
    resp.context_id = ctx->id;
    resp.msix_id = (uint16_t)chan_idx;
    resp.num_cq_pairs_allocated = 1;
    resp.cq_pair[0].x2i_q.head_addr =
        xdna_mbox_dev_addr(chan_base + XDNA_MBOX_X2I_HEAD_OFF);
    resp.cq_pair[0].x2i_q.tail_addr =
        xdna_mbox_dev_addr(chan_base + XDNA_MBOX_X2I_TAIL_OFF);
    resp.cq_pair[0].x2i_q.buf_addr = xdna_sram_dev_addr(cch->x2i_buf);
    resp.cq_pair[0].x2i_q.buf_size = cch->x2i_size;
    resp.cq_pair[0].i2x_q.head_addr =
        xdna_mbox_dev_addr(chan_base + XDNA_MBOX_I2X_HEAD_OFF);
    resp.cq_pair[0].i2x_q.tail_addr =
        xdna_mbox_dev_addr(chan_base + XDNA_MBOX_I2X_TAIL_OFF);
    resp.cq_pair[0].i2x_q.buf_addr = xdna_sram_dev_addr(cch->i2x_buf);
    resp.cq_pair[0].i2x_q.buf_size = cch->i2x_size;

    xdna_log(npu, XDNA_LOG_INFO,
             "MERT: context %u olusturuldu (col %u+%u, pasid %u, kanal %u)",
             ctx->id, req.start_col, req.num_col, req.pasid, chan_idx);

    mert_reply(npu, chan, hdr, &resp, sizeof(resp));
}

static void handle_destroy_context(XdnaNpu *npu, unsigned chan,
                                   const XdnaMsgHeader *hdr,
                                   const uint8_t *payload)
{
    DestroyCtxReq req;
    XdnaContext *ctx;

    if (hdr->total_size < sizeof(req)) {
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
        return;
    }
    memcpy(&req, payload, sizeof(req));

    ctx = ctx_by_id(npu, req.context_id);
    if (!ctx) {
        xdna_log(npu, XDNA_LOG_ERROR, "MERT: bilinmeyen context %u",
                 req.context_id);
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_MGMT_ERT_INVALID_PARAM);
        return;
    }

    xdna_mbox_channel_reset(npu, ctx->chan);
    ctx->valid = false;
    if (npu->stats.active_contexts) {
        npu->stats.active_contexts--;
    }
    xdna_log(npu, XDNA_LOG_INFO, "MERT: context %u yok edildi", req.context_id);
    mert_reply_status(npu, chan, hdr, AIE2_STATUS_SUCCESS);
}

/* ---------------------------------------------------------------- */
/* Yurutme yolu: CONFIG_CU, EXEC_DPU, CHAIN_EXEC_DPU, SYNC_BO        */
/* ---------------------------------------------------------------- */

static XdnaContext *ctx_by_chan(XdnaNpu *npu, unsigned chan)
{
    unsigned i;

    for (i = 0; i < XDNA_NPU1_HWCTX_LIMIT; i++) {
        if (npu->ctx[i].valid && npu->ctx[i].chan == chan) {
            return &npu->ctx[i];
        }
    }
    return NULL;
}

static void handle_config_cu(XdnaNpu *npu, unsigned chan,
                             const XdnaMsgHeader *hdr, const uint8_t *payload)
{
    ConfigCuReq req;
    XdnaContext *ctx = ctx_by_chan(npu, chan);
    uint32_t i;

    if (!ctx) {
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_MGMT_ERT_INVALID_PARAM);
        return;
    }
    if (hdr->total_size < sizeof(uint32_t)) {
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
        return;
    }
    memset(&req, 0, sizeof(req));
    memcpy(&req, payload,
           hdr->total_size < sizeof(req) ? hdr->total_size : sizeof(req));

    if (req.num_cus > MAX_NUM_CUS) {
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_PARAM);
        return;
    }
    ctx->num_cus = req.num_cus;
    for (i = 0; i < req.num_cus; i++) {
        ctx->cu_cfg[i] = req.cfgs[i];
    }

    /*
     * Gercek donanimda bu, CU'larin overlay/PDI imajlarini array'e
     * yuklemesini tetikler. Overlay yukleme henuz uygulanmadi; ctrlcode
     * yolu (EXEC_DPU) array'i dogrudan konfigure ettigi icin veri hareketi
     * yine de dogru calisiyor, ama compute tile programlari yuklenmiyor.
     */
    xdna_log(npu, XDNA_LOG_WARN,
             "MERT: context %u icin %u CU kaydedildi -- overlay/PDI yuklemesi "
             "uygulanmadi", ctx->id, req.num_cus);
    mert_reply_status(npu, chan, hdr, AIE2_STATUS_SUCCESS);
}

static bool devm_to_host(XdnaNpu *npu, const XdnaContext *ctx,
                         uint64_t dev_addr, uint32_t size, uint64_t *host_addr);

/*
 * Firmware'e gelen tampon adresleri gercek akista CIHAZ adresleridir:
 * hem instruction buffer hem komut listesi tamponu context heap'inden
 * ayrilmis DEV BO'lardir (aie2_message.c: init_chain_req'e
 * `amdxdna_gem_dev_addr(cmdbuf_abo)` veriliyor).
 *
 * Cihaz bellegi penceresi SINIRLI: [AIE2_DEVM_BASE, +AIE2_DEVM_SIZE).
 * Disindaki adresler host adresi (IOVA) sayiliyor.
 */
static bool resolve_dev_addr(XdnaNpu *npu, const XdnaContext *ctx,
                             uint64_t addr, uint32_t size, uint64_t *out)
{
    if (addr < AIE2_DEVM_BASE ||
        addr >= (uint64_t)AIE2_DEVM_BASE + AIE2_DEVM_SIZE) {
        *out = addr;
        return true;
    }
    return devm_to_host(npu, ctx, addr, size, out);
}

/* Bir instruction buffer'i host bellegindin okuyup ctrlcode olarak yurut. */
static uint32_t run_instruction_buffer(XdnaNpu *npu, XdnaContext *ctx,
                                       uint64_t inst_addr, uint32_t inst_size)
{
    uint8_t *buf;
    uint32_t status;
    uint64_t host_addr;

    if (!inst_size || inst_size > XDNA_MAX_CTRLCODE_SIZE) {
        xdna_log(npu, XDNA_LOG_ERROR,
                 "MERT: gecersiz instruction buffer boyutu %u", inst_size);
        return AIE2_STATUS_INVALID_INPUT_BUFFER;
    }
    if (!resolve_dev_addr(npu, ctx, inst_addr, inst_size, &host_addr)) {
        return AIE2_STATUS_INVALID_INPUT_BUFFER;
    }
    buf = malloc(inst_size);
    if (!buf) {
        return AIE2_STATUS_MGMT_ERT_NOAVAIL;
    }
    if (!npu->ops->dma_read ||
        npu->ops->dma_read(npu->opaque, host_addr, buf, inst_size) != 0) {
        xdna_log(npu, XDNA_LOG_ERROR,
                 "MERT: instruction buffer okunamadi 0x%llx (%u bayt)",
                 (unsigned long long)inst_addr, inst_size);
        free(buf);
        return AIE2_STATUS_INVALID_INPUT_BUFFER;
    }

    status = xdna_txn_execute(npu, ctx->id, buf, inst_size) == 0
                 ? AIE2_STATUS_SUCCESS
                 : AIE2_STATUS_APP_INVALID_INSTR;
    free(buf);
    return status;
}

static void handle_exec_dpu(XdnaNpu *npu, unsigned chan,
                            const XdnaMsgHeader *hdr, const uint8_t *payload)
{
    ExecDpuReq req;
    XdnaContext *ctx = ctx_by_chan(npu, chan);
    uint32_t status;

    if (!ctx) {
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_MGMT_ERT_INVALID_PARAM);
        return;
    }
    if (hdr->total_size < offsetof(ExecDpuReq, payload)) {
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
        return;
    }
    memset(&req, 0, sizeof(req));
    memcpy(&req, payload,
           hdr->total_size < sizeof(req) ? hdr->total_size : sizeof(req));

    xdna_log(npu, XDNA_LOG_DEBUG,
             "MERT: EXEC_DPU addr 0x%llx, %u bayt, CU %u",
             (unsigned long long)req.inst_buf_addr, req.inst_size,
             req.cu_idx);
    status = run_instruction_buffer(npu, ctx, req.inst_buf_addr, req.inst_size);
    npu->stats.exec_cmds++;
    mert_reply_status(npu, chan, hdr, status);
}

static void handle_chain_exec_dpu(XdnaNpu *npu, unsigned chan,
                                  const XdnaMsgHeader *hdr,
                                  const uint8_t *payload)
{
    CmdChainReq req;
    CmdChainResp resp = { 0 };
    XdnaContext *ctx = ctx_by_chan(npu, chan);
    uint8_t *chain;
    uint64_t chain_addr;
    uint32_t pos = 0;
    uint32_t i;

    if (!ctx) {
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_MGMT_ERT_INVALID_PARAM);
        return;
    }
    if (hdr->total_size < sizeof(req)) {
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
        return;
    }
    memcpy(&req, payload, sizeof(req));

    if (!req.buf_size || req.buf_size > XDNA_MAX_CMD_CHAIN_SIZE) {
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
        return;
    }
    if (!resolve_dev_addr(npu, ctx, req.buf_addr, req.buf_size, &chain_addr)) {
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
        return;
    }
    chain = malloc(req.buf_size);
    if (!chain) {
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_MGMT_ERT_NOAVAIL);
        return;
    }
    if (!npu->ops->dma_read ||
        npu->ops->dma_read(npu->opaque, chain_addr, chain, req.buf_size) != 0) {
        free(chain);
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
        return;
    }

    xdna_log(npu, XDNA_LOG_DEBUG,
             "MERT: komut zinciri, tampon 0x%llx -> 0x%llx, %u bayt, %u komut",
             (unsigned long long)req.buf_addr, (unsigned long long)chain_addr,
             req.buf_size, req.count);

    resp.status = AIE2_STATUS_SUCCESS;
    for (i = 0; i < req.count; i++) {
        CmdChainSlotDpu slot;
        uint32_t slot_size, st;

        if (pos + sizeof(slot) > req.buf_size) {
            resp.status = AIE2_STATUS_INVALID_INPUT_BUFFER;
            resp.fail_cmd_idx = i;
            resp.fail_cmd_status = AIE2_STATUS_INVALID_INPUT_BUFFER;
            break;
        }
        memcpy(&slot, chain + pos, sizeof(slot));
        slot_size = (uint32_t)sizeof(slot) + slot.arg_cnt * 4u;
        if (pos + slot_size > req.buf_size) {
            resp.status = AIE2_STATUS_INVALID_INPUT_BUFFER;
            resp.fail_cmd_idx = i;
            resp.fail_cmd_status = AIE2_STATUS_INVALID_INPUT_BUFFER;
            break;
        }

        st = run_instruction_buffer(npu, ctx, slot.inst_buf_addr,
                                    slot.inst_size);
        npu->stats.exec_cmds++;
        if (st != AIE2_STATUS_SUCCESS) {
            resp.status = st;
            resp.fail_cmd_idx = i;
            resp.fail_cmd_status = st;
            break;
        }
        pos += slot_size;
    }

    free(chain);
    mert_reply(npu, chan, hdr, &resp, sizeof(resp));
}

/*
 * Cihaz adresi -> host adresi.
 *
 * amdxdna_gem.c: heap BO'nun cihaz adresi dev_mem_base'den basliyor
 * (abo->dev_addr = dev_info->dev_mem_base + total_heap_size) ve devm
 * penceresindeki offset `dev_addr - dev_mem_base` olarak hesaplaniyor.
 * Heap'in host adresini firmware MAP_HOST_BUFFER ile ogreniyor.
 *
 * Yani cihaz bellegi ayri bir fiziksel bellek degil; context'in host
 * heap tamponuna bakan bir adres penceresi.
 */
static bool devm_to_host(XdnaNpu *npu, const XdnaContext *ctx,
                         uint64_t dev_addr, uint32_t size, uint64_t *host_addr)
{
    uint64_t off;

    if (!ctx->heap_addr || !ctx->heap_size) {
        xdna_log(npu, XDNA_LOG_ERROR,
                 "MERT: context %u icin heap bildirilmemis", ctx->id);
        return false;
    }
    if (dev_addr < AIE2_DEVM_BASE) {
        xdna_log(npu, XDNA_LOG_ERROR,
                 "MERT: 0x%llx cihaz bellegi penceresinin altinda",
                 (unsigned long long)dev_addr);
        return false;
    }
    off = dev_addr - AIE2_DEVM_BASE;
    if (off + size > ctx->heap_size) {
        xdna_log(npu, XDNA_LOG_ERROR,
                 "MERT: cihaz adresi heap disinda (offset 0x%llx + %u > 0x%llx)",
                 (unsigned long long)off, size,
                 (unsigned long long)ctx->heap_size);
        return false;
    }
    *host_addr = ctx->heap_addr + off;
    return true;
}

static void handle_sync_bo(XdnaNpu *npu, unsigned chan,
                           const XdnaMsgHeader *hdr, const uint8_t *payload)
{
    SyncBoReq req;
    XdnaContext *ctx = ctx_by_chan(npu, chan);
    uint8_t chunk[4096];
    uint32_t done = 0;
    uint32_t src_type, dst_type;
    uint64_t src = 0, dst = 0;

    if (hdr->total_size < sizeof(req)) {
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
        return;
    }
    memcpy(&req, payload, sizeof(req));

    /* aie2_msg_priv.h: type alaninin alt/ust nibble'lari src/dst turu */
    src_type = req.type & 0xFu;
    dst_type = (req.type >> 4) & 0xFu;

    src = req.src_addr;
    dst = req.dst_addr;

    if (src_type == SYNC_BO_DEV_MEM || dst_type == SYNC_BO_DEV_MEM) {
        if (!ctx) {
            mert_reply_status(npu, chan, hdr,
                              AIE2_STATUS_MGMT_ERT_INVALID_PARAM);
            return;
        }
        if (src_type == SYNC_BO_DEV_MEM &&
            !devm_to_host(npu, ctx, req.src_addr, req.size, &src)) {
            mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
            return;
        }
        if (dst_type == SYNC_BO_DEV_MEM &&
            !devm_to_host(npu, ctx, req.dst_addr, req.size, &dst)) {
            mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
            return;
        }
    }

    if ((src_type != SYNC_BO_HOST_MEM && src_type != SYNC_BO_DEV_MEM) ||
        (dst_type != SYNC_BO_HOST_MEM && dst_type != SYNC_BO_DEV_MEM)) {
        xdna_log(npu, XDNA_LOG_WARN, "MERT: SYNC_BO bilinmeyen tur 0x%x",
                 req.type);
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_PARAM);
        return;
    }

    while (done < req.size) {
        uint32_t n = req.size - done;

        if (n > sizeof(chunk)) {
            n = sizeof(chunk);
        }
        if (!npu->ops->dma_read || !npu->ops->dma_write ||
            npu->ops->dma_read(npu->opaque, src + done, chunk, n) != 0 ||
            npu->ops->dma_write(npu->opaque, dst + done, chunk, n) != 0) {
            mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
            return;
        }
        done += n;
    }
    mert_reply_status(npu, chan, hdr, AIE2_STATUS_SUCCESS);
}

/* ---------------------------------------------------------------- */
/* Mesaj dagitici                                                    */
/* ---------------------------------------------------------------- */

void xdna_mert_handle(XdnaNpu *npu, unsigned chan, const XdnaMsgHeader *hdr,
                      const uint8_t *payload)
{
    if (!npu->fw_alive) {
        xdna_log(npu, XDNA_LOG_ERROR,
                 "MERT: firmware ayakta degilken opcode 0x%x", hdr->opcode);
        return;
    }

    switch (hdr->opcode) {
    case MSG_OP_GET_PROTOCOL_VERSION: {
        ProtocolVersionResp resp = {
            .status = AIE2_STATUS_SUCCESS,
            .major = XDNA_MGMT_PROT_MAJOR,
            .minor = XDNA_MGMT_PROT_MINOR,
        };
        mert_reply(npu, chan, hdr, &resp, sizeof(resp));
        return;
    }

    case MSG_OP_GET_FIRMWARE_VERSION: {
        FirmwareVersionResp resp = {
            .status = AIE2_STATUS_SUCCESS,
            .major = XDNA_FW_VER_MAJOR,
            .minor = XDNA_FW_VER_MINOR,
            .sub = XDNA_FW_VER_SUB,
            .build = XDNA_FW_VER_BUILD,
        };
        mert_reply(npu, chan, hdr, &resp, sizeof(resp));
        return;
    }

    case MSG_OP_QUERY_AIE_VERSION: {
        AieVersionResp resp = {
            .status = AIE2_STATUS_SUCCESS,
            .major = XDNA_AIE_VERSION_MAJOR,
            .minor = XDNA_AIE_VERSION_MINOR,
        };
        mert_reply(npu, chan, hdr, &resp, sizeof(resp));
        return;
    }

    case MSG_OP_QUERY_AIE_TILE_INFO: {
        AieTileInfoResp resp;
        resp.status = AIE2_STATUS_SUCCESS;
        fill_tile_info(&resp.info);
        mert_reply(npu, chan, hdr, &resp, sizeof(resp));
        return;
    }

    case MSG_OP_SET_RUNTIME_CONFIG: {
        SetRuntimeCfgReq req;
        if (hdr->total_size < sizeof(req)) {
            mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
            return;
        }
        memcpy(&req, payload, sizeof(req));
        if (req.type >= sizeof(npu->rt_cfg) / sizeof(npu->rt_cfg[0])) {
            xdna_log(npu, XDNA_LOG_WARN,
                     "MERT: destegi olmayan runtime config tipi %u", req.type);
            mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_PARAM);
            return;
        }
        npu->rt_cfg[req.type] = req.value;
        xdna_log(npu, XDNA_LOG_DEBUG, "MERT: rt_cfg[%u] = %llu", req.type,
                 (unsigned long long)req.value);
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_SUCCESS);
        return;
    }

    case MSG_OP_GET_RUNTIME_CONFIG: {
        GetRuntimeCfgReq req;
        GetRuntimeCfgResp resp = { 0 };
        if (hdr->total_size < sizeof(req)) {
            mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
            return;
        }
        memcpy(&req, payload, sizeof(req));
        if (req.type >= sizeof(npu->rt_cfg) / sizeof(npu->rt_cfg[0])) {
            resp.status = AIE2_STATUS_INVALID_PARAM;
        } else {
            resp.status = AIE2_STATUS_SUCCESS;
            resp.value = npu->rt_cfg[req.type];
        }
        mert_reply(npu, chan, hdr, &resp, sizeof(resp));
        return;
    }

    case MSG_OP_ASSIGN_MGMT_PASID: {
        AssignPasidReq req;
        if (hdr->total_size < sizeof(req)) {
            mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
            return;
        }
        memcpy(&req, payload, sizeof(req));
        npu->mgmt_pasid = req.pasid;
        xdna_log(npu, XDNA_LOG_INFO, "MERT: yonetim PASID'i %u", req.pasid);
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_SUCCESS);
        return;
    }

    case MSG_OP_SUSPEND:
        npu->fw_suspended = true;
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_SUCCESS);
        return;

    case MSG_OP_RESUME:
        npu->fw_suspended = false;
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_SUCCESS);
        return;

    case MSG_OP_INVOKE_SELF_TEST:
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_SUCCESS);
        return;

    case MSG_OP_CREATE_CONTEXT:
        handle_create_context(npu, chan, hdr, payload);
        return;

    case MSG_OP_DESTROY_CONTEXT:
        handle_destroy_context(npu, chan, hdr, payload);
        return;

    case MSG_OP_MAP_HOST_BUFFER:
    case MSG_OP_ADD_HOST_BUFFER: {
        MapHostBufferReq req;
        XdnaContext *ctx;
        if (hdr->total_size < sizeof(req)) {
            mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
            return;
        }
        memcpy(&req, payload, sizeof(req));
        ctx = ctx_by_id(npu, req.context_id);
        if (!ctx) {
            mert_reply_status(npu, chan, hdr,
                              AIE2_STATUS_MGMT_ERT_INVALID_PARAM);
            return;
        }
        ctx->heap_addr = req.buf_addr;
        ctx->heap_size = req.buf_size;
        xdna_log(npu, XDNA_LOG_INFO,
                 "MERT: context %u heap 0x%llx (%llu bayt)", req.context_id,
                 (unsigned long long)req.buf_addr,
                 (unsigned long long)req.buf_size);
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_SUCCESS);
        return;
    }

    case MSG_OP_REGISTER_ASYNC_EVENT_MSG: {
        AsyncEventReq req;
        AsyncEventResp resp = { 0 };
        if (hdr->total_size < sizeof(req)) {
            mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
            return;
        }
        memcpy(&req, payload, sizeof(req));
        npu->async_buf_addr = req.buf_addr;
        npu->async_buf_size = req.buf_size;
        npu->async_registered = true;
        npu->async_chan = chan;
        npu->async_msg_id = hdr->id;
        /*
         * Gercek firmware bu mesaji HEMEN cevaplamaz: asenkron bir olay
         * olustugunda cevap olarak dondurur. Surucu de (aie2_error.c) bunu
         * bekleyen bir mesaj olarak tutar. Bu yuzden burada cevap URETMIYORUZ.
         */
        xdna_log(npu, XDNA_LOG_DEBUG,
                 "MERT: async event buffer kaydedildi 0x%llx",
                 (unsigned long long)req.buf_addr);
        (void)resp;
        return;
    }

    case MSG_OP_QUERY_COL_STATUS: {
        ColumnInfoReq req;
        ColumnInfoResp resp = { 0 };
        if (hdr->total_size < sizeof(req)) {
            mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_INPUT_BUFFER);
            return;
        }
        memcpy(&req, payload, sizeof(req));
        /*
         * TODO(asama 6): kolon durumu dokumu icin gercek array durumu
         * gerekiyor. Su an sifirlanmis bir dump yaziyoruz -- surucu ve
         * xrt-smi bunu gecerli kabul eder, icerigi bos gorunur.
         */
        if (npu->ops->dma_write && req.dump_buff_addr && req.dump_buff_size) {
            static const uint8_t zeros[256] = { 0 };
            uint32_t done = 0;
            while (done < req.dump_buff_size) {
                uint32_t n = req.dump_buff_size - done;
                if (n > sizeof(zeros)) {
                    n = sizeof(zeros);
                }
                if (npu->ops->dma_write(npu->opaque, req.dump_buff_addr + done,
                                        zeros, n) != 0) {
                    break;
                }
                done += n;
            }
            resp.size = done;
        }
        resp.status = AIE2_STATUS_SUCCESS;
        mert_reply(npu, chan, hdr, &resp, sizeof(resp));
        return;
    }

    case MSG_OP_GET_TELEMETRY: {
        TelemetryResp resp = {
            .major = 1,
            .minor = 0,
            .size = 0,
            .status = AIE2_STATUS_SUCCESS,
        };
        mert_reply(npu, chan, hdr, &resp, sizeof(resp));
        return;
    }

    case MSG_OP_CONFIG_CU:
        handle_config_cu(npu, chan, hdr, payload);
        return;

    case MSG_OP_EXEC_DPU:
        handle_exec_dpu(npu, chan, hdr, payload);
        return;

    case MSG_OP_CHAIN_EXEC_DPU:
        handle_chain_exec_dpu(npu, chan, hdr, payload);
        return;

    case MSG_OP_SYNC_BO:
        handle_sync_bo(npu, chan, hdr, payload);
        return;

    case MSG_OP_CONFIG_DEBUG_BO:
        /* Debug BO kaydi: array yurutmesini etkilemiyor. */
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_SUCCESS);
        return;

    /*
     * Bunlar compute tile'larda gercek program yurutmesi gerektiriyor
     * (yol haritasi asama 7: AIE instruction interpreter). Sessizce
     * "basarili" demek yanlis sonuc uretir.
     */
    case MSG_OP_EXECUTE_BUFFER_CF:
    case MSG_OP_CHAIN_EXEC_BUFFER_CF:
    case MSG_OP_CHAIN_EXEC_NPU:
        xdna_log(npu, XDNA_LOG_WARN,
                 "MERT: opcode 0x%x henuz uygulanmadi (AIE interpreter yok)",
                 hdr->opcode);
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_OPERATION);
        return;

    default:
        xdna_log(npu, XDNA_LOG_WARN, "MERT: bilinmeyen opcode 0x%x",
                 hdr->opcode);
        mert_reply_status(npu, chan, hdr, AIE2_STATUS_INVALID_COMMAND);
        return;
    }
}
