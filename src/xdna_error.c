// SPDX-License-Identifier: GPL-2.0-only
/*
 * Asenkron hata bildirimi.
 *
 * Protokol (amdxdna: aie2_error.c):
 *   1. Surucu MSG_OP_REGISTER_ASYNC_EVENT_MSG ile bir tampon kaydeder.
 *      Firmware bu mesaji HEMEN CEVAPLAMAZ; bekletir.
 *   2. Array'de bir hata olustugunda firmware tampona `struct aie_err_info`
 *      ve ardindan `struct aie_error` dizisini yazar, sonra bekleyen
 *      mesaja cevap doner.
 *   3. Surucu hatayi siniflandirir (aie_get_error_category) ve olayi
 *      YENIDEN kaydeder.
 *
 * Olay kimlikleri surucudeki LUT'lardan alindi; boylece surucu hatayi
 * dogru kategoriye ayiriyor. Uydurma bir event_id "unknown" olarak
 * siniflanirdi.
 */

#include <string.h>

#include "xdna_internal.h"

/* aie2_error.c: enum aie_module_type */
#define AIE_MEM_MOD     0u
#define AIE_CORE_MOD    1u
#define AIE_PL_MOD      2u

/* aie2_msg_priv.h: enum async_event_type */
#define ASYNC_EVENT_TYPE_AIE_ERROR 0u

/*
 * struct aie_error / struct aie_err_info -- surucude PACKED DEGIL.
 * aie_error: u8 row, u8 col, (2 bayt dolgu), u32 mod_type, u8 event_id
 *            + dolgu = 12 bayt.
 */
typedef struct {
    uint8_t row;
    uint8_t col;
    uint32_t mod_type;
    uint8_t event_id;
} AieErrorEntry;

typedef struct {
    uint32_t err_cnt;
    uint32_t ret_code;
    uint32_t rsvd;
} AieErrInfoHdr;

_Static_assert(sizeof(AieErrorEntry) == 12, "struct aie_error");
_Static_assert(sizeof(AieErrInfoHdr) == 12, "struct aie_err_info basligi");

/*
 * Olay kimlikleri -- aie2_error.c LUT'lari:
 *   aie_ml_shim_tile_event_cat : DMA 72, LOCK 74, STREAM 65
 *   aie_ml_mem_tile_event_cat  : DMA 133, LOCK 139, STREAM 135  (satir 1)
 *   aie_ml_mem_event_cat       : DMA 97, LOCK 101               (compute tile)
 *   aie_ml_core_event_cat      : STREAM 56, INSTRUCTION 59
 *
 * Surucu AIE_MEM_MOD icin LUT'u satira gore seciyor (row == 1 -> mem tile),
 * bu da bizim topolojimizle birebir ortusuyor.
 */
static bool pick_event(AieTileKind kind, XdnaErrCat cat, uint32_t *mod,
                       uint8_t *evt)
{
    switch (kind) {
    case AIE_TILE_SHIM:
        *mod = AIE_PL_MOD;
        switch (cat) {
        case XDNA_ERR_DMA:    *evt = 72; return true;
        case XDNA_ERR_LOCK:   *evt = 74; return true;
        case XDNA_ERR_STREAM: *evt = 65; return true;
        default: return false;   /* shim'de instruction hatasi yok */
        }

    case AIE_TILE_MEM:
        *mod = AIE_MEM_MOD;
        switch (cat) {
        case XDNA_ERR_DMA:    *evt = 133; return true;
        case XDNA_ERR_LOCK:   *evt = 139; return true;
        case XDNA_ERR_STREAM: *evt = 135; return true;
        default: return false;   /* memory tile'da instruction hatasi yok */
        }

    default:
        /*
         * Compute tile: DMA ve lock bellek modulunde, stream ve
         * instruction core modulunde.
         */
        switch (cat) {
        case XDNA_ERR_DMA:    *mod = AIE_MEM_MOD;  *evt = 97;  return true;
        case XDNA_ERR_LOCK:   *mod = AIE_MEM_MOD;  *evt = 101; return true;
        case XDNA_ERR_STREAM: *mod = AIE_CORE_MOD; *evt = 56;  return true;
        /* aie_ml_core_event_cat: 59 = AIE_ERROR_INSTRUCTION */
        case XDNA_ERR_INSTRUCTION:
            *mod = AIE_CORE_MOD; *evt = 59; return true;
        }
        return false;
    }
}

void xdna_async_error(XdnaNpu *npu, uint8_t col, uint8_t row, AieTileKind kind,
                      XdnaErrCat cat)
{
    uint8_t buf[sizeof(AieErrInfoHdr) + sizeof(AieErrorEntry)];
    AieErrInfoHdr info;
    AieErrorEntry err;
    AsyncEventResp resp;
    uint32_t mod;
    uint8_t evt;

    npu->stats.array_errors++;

    if (!pick_event(kind, cat, &mod, &evt)) {
        return;
    }

    if (!npu->async_registered) {
        /*
         * Surucu henuz olay kaydetmemis. Hata gercek ama bildirilemiyor;
         * sessizce yutmuyoruz.
         */
        xdna_log(npu, XDNA_LOG_WARN,
                 "async: tile(%u,%u) hatasi bildirilemedi (kayitli olay yok)",
                 col, row);
        return;
    }

    memset(&info, 0, sizeof(info));
    info.err_cnt = 1;
    info.ret_code = 0;

    memset(&err, 0, sizeof(err));
    err.row = row;
    err.col = col;
    err.mod_type = mod;
    err.event_id = evt;

    if (npu->async_buf_size < sizeof(buf)) {
        xdna_log(npu, XDNA_LOG_ERROR, "async: kayitli tampon cok kucuk (%u)",
                 npu->async_buf_size);
        return;
    }

    memcpy(buf, &info, sizeof(info));
    memcpy(buf + sizeof(info), &err, sizeof(err));

    if (!npu->ops->dma_write ||
        npu->ops->dma_write(npu->opaque, npu->async_buf_addr, buf,
                            sizeof(buf)) != 0) {
        xdna_log(npu, XDNA_LOG_ERROR, "async: hata tamponu yazilamadi 0x%llx",
                 (unsigned long long)npu->async_buf_addr);
        return;
    }

    memset(&resp, 0, sizeof(resp));
    resp.status = AIE2_STATUS_SUCCESS;
    resp.type = ASYNC_EVENT_TYPE_AIE_ERROR;

    /*
     * Bekleyen REGISTER_ASYNC_EVENT_MSG mesajina cevap dondur. Surucu
     * olayi isledikten sonra yeniden kaydediyor, bu yuzden kaydi
     * dusuruyoruz.
     */
    npu->async_registered = false;
    xdna_mbox_send(npu, npu->async_chan, npu->async_msg_id,
                   MSG_OP_REGISTER_ASYNC_EVENT_MSG, &resp, sizeof(resp));

    xdna_log(npu, XDNA_LOG_INFO,
             "async: tile(%u,%u) hatasi bildirildi (mod %u, event %u)", col,
             row, mod, evt);
}
