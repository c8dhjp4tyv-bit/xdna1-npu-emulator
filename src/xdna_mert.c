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
    uint32_t status;
    uint32_t type;
} AsyncEventResp;

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

#pragma pack(pop)

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
    /* TODO(dogrula): asagidaki sayilar AIE2 mimarisinden turetildi. */
    info->core_dma_channels = 2;
    info->mem_dma_channels = 6;
    info->shim_dma_channels = 2;
    info->core_locks = 16;
    info->mem_locks = 64;
    info->shim_locks = 16;
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
        if (!req.dump_buff_addr || !req.dump_buff_size) {
            resp.status = AIE2_STATUS_INVALID_PARAM;
        } else if (!npu->ops->dma_write) {
            resp.status = AIE2_STATUS_AIE_DMA_ERROR;
        } else {
            static const uint8_t zeros[256] = { 0 };
            uint32_t done = 0;
            while (done < req.dump_buff_size) {
                uint32_t n = req.dump_buff_size - done;
                if (n > sizeof(zeros)) {
                    n = sizeof(zeros);
                }
                if (npu->ops->dma_write(npu->opaque, req.dump_buff_addr + done,
                                        zeros, n) != 0) {
                    resp.status = AIE2_STATUS_AIE_DMA_ERROR;
                    break;
                }
                done += n;
            }
            resp.size = done;
            if (done == req.dump_buff_size) {
                resp.status = AIE2_STATUS_SUCCESS;
            }
        }
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

    /*
     * Asagidakiler XDNA array'inin gercek yurutulmesini gerektirir
     * (yol haritasi asama 6-9). Sessizce "basarili" demek yanlis sonuc
     * uretir; bu yuzden acikca desteklenmedigini bildiriyoruz.
     */
    case MSG_OP_CONFIG_CU:
    case MSG_OP_EXECUTE_BUFFER_CF:
    case MSG_OP_EXEC_DPU:
    case MSG_OP_CHAIN_EXEC_BUFFER_CF:
    case MSG_OP_CHAIN_EXEC_DPU:
    case MSG_OP_CHAIN_EXEC_NPU:
    case MSG_OP_SYNC_BO:
    case MSG_OP_CONFIG_DEBUG_BO:
        xdna_log(npu, XDNA_LOG_WARN,
                 "MERT: opcode 0x%x henuz uygulanmadi (array asamasi)",
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
