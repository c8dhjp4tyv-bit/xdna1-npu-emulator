// SPDX-License-Identifier: GPL-2.0-only
/*
 * ctrlcode (XAie transaction) yurutucusu.
 *
 * Bir XDNA workload'u iki parcadan olusur: overlay (array konfigurasyonu ve
 * compute tile ELF'leri) ve ctrlcode (array uzerinde orkestrasyon). ctrlcode,
 * aie-rt'nin serilestirdigi XAie_TxnOpcode komutlarindan olusan bir ikili
 * tampondur; gercek donanimda ERT bunu yorumlar.
 *
 * Bicim (aie-rt: driver/src/global/xaiegbl.h, driver/src/common/xaie_txn.h):
 *
 *   XAie_TxnHeader            (16 bayt)
 *   [ op kaydi ] [ op kaydi ] ...
 *
 * Her op kaydi kendi `Size` alanini tasir (baslik + payload, bayt cinsinden),
 * yani yurutucu opcode'u tanimasa bile kaydi dogru atlayabilir.
 *
 * Yapilar aie-rt'de PACKED DEGIL; dogal hizalama gecerli. Boyutlar
 * static_assert ile kilitlendi.
 */

#include <assert.h>
#include <stddef.h>
#include <string.h>

#include "xdna_internal.h"

typedef struct {
    uint8_t Major;
    uint8_t Minor;
    uint8_t DevGen;
    uint8_t NumRows;
    uint8_t NumCols;
    uint8_t NumMemTileRows;
    uint32_t NumOps;
    uint32_t TxnSize;
} TxnHeader;

typedef struct {
    uint8_t Op;
    uint8_t Col;
    uint8_t Row;
} OpHdr;

typedef struct {
    OpHdr hdr;
    uint64_t RegOff;
    uint32_t Value;
    uint32_t Size;
} Write32Hdr;

typedef struct {
    OpHdr hdr;
    uint64_t RegOff;
    uint32_t Value;
    uint32_t Mask;
    uint32_t Size;
} MaskWrite32Hdr;

typedef struct {
    OpHdr hdr;
    uint8_t Col;
    uint8_t Row;
    uint32_t RegOff;
    uint32_t Size;
} BlockWrite32Hdr;

typedef struct {
    OpHdr hdr;
    uint32_t Size;
} CustomOpHdr;

static_assert(sizeof(TxnHeader) == 16, "XAie_TxnHeader");
static_assert(sizeof(OpHdr) == 3, "XAie_OpHdr");
static_assert(sizeof(Write32Hdr) == 24, "XAie_Write32Hdr");
static_assert(sizeof(MaskWrite32Hdr) == 32, "XAie_MaskWrite32Hdr");
static_assert(sizeof(BlockWrite32Hdr) == 16, "XAie_BlockWrite32Hdr");
static_assert(sizeof(CustomOpHdr) == 8, "XAie_CustomOpHdr");

/*
 * TCT (Task Completion Token) payload -- aiebu aie2p_passes.cpp
 * stringify_tct():
 *   word:   bayt2 = kolon, bayt1 = satir, bayt0 = yon (1 = MM2S, 0 = S2MM)
 *   config: bayt3 = kanal, bayt2 = kolon sayisi, bayt1 = satir sayisi
 */
typedef struct {
    uint32_t word;
    uint32_t config;
} TctOp;

/*
 * DDR_PATCH payload -- aiebu xaie_txn.h patch_op_t.
 * Anlam: regaddr'a, `argidx` numarali kernel argumaninin degeri + argplus
 * yazilir. Yapinin yerlesimi DOGRULANDI; ancak argumanlarin komut
 * icindeki yerlesimi (kelime indeksi mi, 64-bit degerin bolunmesi vs.)
 * dogrulanmadi, bu yuzden op UYGULANMIYOR -- yanlis yamalama sessizce
 * yanlis sonuc uretirdi.
 */
typedef struct {
    uint32_t action;
    uint64_t regaddr;
    uint64_t argidx;
    uint64_t argplus;
} PatchOp;

static_assert(sizeof(TctOp) == 8, "TCT payload");
static_assert(sizeof(PatchOp) == 32, "patch_op_t");

/*
 * MASKPOLL, donanimda "kosul saglanana kadar bekle" demek. Emulatorde
 * DMA'lar senkron tamamlandigindan kosul genellikle ilk okumada saglanir.
 * Yine de sonsuz donguye karsi bir ust sinir koyuyoruz: bu sinira
 * dayanmak, ctrlcode'un bekledigi bir olayin modellenmedigi anlamina gelir
 * ve acikca hata olarak raporlanir.
 */
#define TXN_MASKPOLL_LIMIT 1024u

typedef struct {
    XdnaNpu *npu;
    XdnaArray *arr;
    uint8_t col_base;     /* context'in partition baslangic kolonu */
    uint8_t col_count;    /* partition genisligi */
    uint32_t ops_done;
    uint32_t skipped;
} TxnCtx;

/*
 * ctrlcode kolon numaralari partition'a GORECELIDIR: derleyici workload'u
 * 0'dan baslayan kolonlar icin uretir, ERT bunu context'in start_col'una
 * kaydirir.
 */
static bool txn_tile(TxnCtx *tc, uint8_t col, uint8_t row, uint8_t *out_col)
{
    uint32_t abs = (uint32_t)tc->col_base + col;

    /*
     * Partition siniri: bir context yalnizca kendisine ayrilan kolonlara
     * erisebilir. Gercek donanimda bunu ERT/donanim zorlar; emulatorde de
     * zorlamazsak context'ler arasi izolasyon modellenmemis olur.
     */
    if (col >= tc->col_count || abs >= AIE_NUM_COLS || row >= AIE_NUM_ROWS) {
        xdna_log(tc->npu, XDNA_LOG_ERROR,
                 "ctrlcode: partition disi tile (kolon %u+%u/%u, satir %u)",
                 tc->col_base, col, tc->col_count, row);
        return false;
    }
    *out_col = (uint8_t)abs;
    return true;
}

static int txn_run(XdnaNpu *npu, uint32_t ctx_id, const uint8_t *buf,
                   uint32_t size)
{
    TxnCtx tc = { 0 };
    TxnHeader hdr;
    uint32_t pos;
    uint32_t op_index = 0;
    XdnaContext *ctx = NULL;
    unsigned i;

    for (i = 0; i < XDNA_NPU1_HWCTX_LIMIT; i++) {
        if (npu->ctx[i].valid && npu->ctx[i].id == ctx_id) {
            ctx = &npu->ctx[i];
            break;
        }
    }
    if (!ctx) {
        xdna_log(npu, XDNA_LOG_ERROR, "ctrlcode: bilinmeyen context %u", ctx_id);
        return -1;
    }

    if (size < sizeof(hdr)) {
        xdna_log(npu, XDNA_LOG_ERROR, "ctrlcode: tampon cok kucuk (%u)", size);
        return -1;
    }
    memcpy(&hdr, buf, sizeof(hdr));

    if (hdr.TxnSize > size) {
        xdna_log(npu, XDNA_LOG_ERROR,
                 "ctrlcode: baslikta bildirilen boyut (%u) tampondan (%u) buyuk",
                 hdr.TxnSize, size);
        return -1;
    }
    if (hdr.TxnSize) {
        size = hdr.TxnSize;
    }

    tc.npu = npu;
    tc.arr = npu->array;
    tc.col_base = ctx->start_col;
    tc.col_count = ctx->num_col;

    /*
     * Shim DMA adres cevirisi icin context'in cihaz bellegi penceresi.
     * Yurutme suresince gecerli; sonunda temizleniyor ki context disinda
     * kalmis bir pencere yanlislikla kullanilmasin.
     */
    npu->array->devm_heap_addr = ctx->heap_addr;
    npu->array->devm_heap_size = ctx->heap_size;

    xdna_log(npu, XDNA_LOG_INFO,
             "ctrlcode: context %u, %u op, %u bayt, devgen %u, %ux%u",
             ctx_id, hdr.NumOps, hdr.TxnSize, hdr.DevGen, hdr.NumCols,
             hdr.NumRows);

    pos = sizeof(hdr);
    while (pos < size) {
        OpHdr op;
        uint32_t op_size = 0;
        uint8_t col;

        if (pos + sizeof(op) > size) {
            xdna_log(npu, XDNA_LOG_ERROR,
                     "ctrlcode: op %u basligi tampona sigmiyor", op_index);
            return -1;
        }
        memcpy(&op, buf + pos, sizeof(op));

        switch (op.Op) {
        case XAIE_IO_WRITE: {
            Write32Hdr w;

            if (pos + sizeof(w) > size) {
                goto truncated;
            }
            memcpy(&w, buf + pos, sizeof(w));
            op_size = w.Size ? w.Size : (uint32_t)sizeof(w);
            if (!txn_tile(&tc, op.Col, op.Row, &col)) {
                return -1;
            }
            xdna_array_write32(tc.arr, col, op.Row,
                               (uint32_t)w.RegOff & AIE_TILE_OFF_MASK, w.Value);
            break;
        }

        case XAIE_IO_MASKWRITE: {
            MaskWrite32Hdr w;
            uint32_t off, cur;

            if (pos + sizeof(w) > size) {
                goto truncated;
            }
            memcpy(&w, buf + pos, sizeof(w));
            op_size = w.Size ? w.Size : (uint32_t)sizeof(w);
            if (!txn_tile(&tc, op.Col, op.Row, &col)) {
                return -1;
            }
            off = (uint32_t)w.RegOff & AIE_TILE_OFF_MASK;
            cur = xdna_array_read32(tc.arr, col, op.Row, off);
            xdna_array_write32(tc.arr, col, op.Row, off,
                               (cur & ~w.Mask) | (w.Value & w.Mask));
            break;
        }

        case XAIE_IO_MASKPOLL: {
            MaskWrite32Hdr w;   /* MaskPoll32Hdr ile ayni yerlesim */
            uint32_t off, tries = 0;
            bool ok = false;

            if (pos + sizeof(w) > size) {
                goto truncated;
            }
            memcpy(&w, buf + pos, sizeof(w));
            op_size = w.Size ? w.Size : (uint32_t)sizeof(w);
            if (!txn_tile(&tc, op.Col, op.Row, &col)) {
                return -1;
            }
            off = (uint32_t)w.RegOff & AIE_TILE_OFF_MASK;
            while (tries++ < TXN_MASKPOLL_LIMIT) {
                if ((xdna_array_read32(tc.arr, col, op.Row, off) & w.Mask) ==
                    (w.Value & w.Mask)) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                xdna_log(npu, XDNA_LOG_ERROR,
                         "ctrlcode: op %u MASKPOLL zaman asimi "
                         "(tile %u,%u offset 0x%x mask 0x%x deger 0x%x)",
                         op_index, col, op.Row, off, w.Mask, w.Value);
                return -1;
            }
            break;
        }

        case XAIE_IO_BLOCKWRITE:
        case XAIE_IO_BLOCKSET: {
            BlockWrite32Hdr w;
            uint32_t words;

            if (pos + sizeof(w) > size) {
                goto truncated;
            }
            memcpy(&w, buf + pos, sizeof(w));
            op_size = w.Size;
            if (op_size < sizeof(w) || pos + op_size > size) {
                goto truncated;
            }
            /*
             * BLOCKSET de serilestirilirken BLOCKWRITE'a donusuyor
             * (aie-rt _XAie_AppendBlockSet32), yani payload zaten
             * genisletilmis haldedir.
             */
            words = (op_size - (uint32_t)sizeof(w)) / 4u;
            if (!txn_tile(&tc, op.Col, op.Row, &col)) {
                return -1;
            }
            {
                const uint8_t *payload = buf + pos + sizeof(w);
                uint32_t k;

                for (k = 0; k < words; k++) {
                    uint32_t v;
                    memcpy(&v, payload + k * 4u, 4);
                    xdna_array_write32(tc.arr, col, op.Row,
                                       (w.RegOff & AIE_TILE_OFF_MASK) + k * 4u,
                                       v);
                }
            }
            break;
        }

        case XAIE_IO_MASKPOLL_BUSY: {
            /*
             * MaskPoll ile ayni baslik. "Busy" varyantinin tam semantigi
             * dogrulanmadi; kosul saglanana kadar bekleme olarak
             * isliyoruz (en yakin okuma).
             */
            MaskWrite32Hdr w;
            uint32_t off, tries = 0;
            bool ok = false;

            if (pos + sizeof(w) > size) {
                goto truncated;
            }
            memcpy(&w, buf + pos, sizeof(w));
            op_size = w.Size ? w.Size : (uint32_t)sizeof(w);
            if (!txn_tile(&tc, op.Col, op.Row, &col)) {
                return -1;
            }
            off = (uint32_t)w.RegOff & AIE_TILE_OFF_MASK;
            while (tries++ < TXN_MASKPOLL_LIMIT) {
                if ((xdna_array_read32(tc.arr, col, op.Row, off) & w.Mask) ==
                    (w.Value & w.Mask)) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                xdna_log(npu, XDNA_LOG_ERROR,
                         "ctrlcode: op %u MASKPOLL_BUSY zaman asimi", op_index);
                return -1;
            }
            break;
        }

        case XAIE_IO_NOOP:
            op_size = AIE_TXN_NOOP_SIZE;
            break;

        case XAIE_IO_CUSTOM_OP_TCT: {
            /*
             * Task Completion Token: bir DMA kanalinin gorevini
             * tamamlamasini bekler. Emulatorde DMA'lar SENKRON tamamlandigi
             * icin token zaten hazir; yapilacak is kanalin gecerliligini
             * dogrulamak. Bu bir tahmin degil, modelimizin dogrudan sonucu.
             */
            CustomOpHdr c;
            TctOp tct;
            uint32_t tcol, trow, tch, dir;

            if (pos + sizeof(c) > size) {
                goto truncated;
            }
            memcpy(&c, buf + pos, sizeof(c));
            op_size = c.Size;
            if (op_size < sizeof(c) + sizeof(tct) || pos + op_size > size) {
                goto truncated;
            }
            memcpy(&tct, buf + pos + sizeof(c), sizeof(tct));

            tcol = (tct.word >> 16) & 0xFFu;
            trow = (tct.word >> 8) & 0xFFu;
            dir = tct.word & 0xFFu;
            tch = (tct.config >> 24) & 0xFFu;

            if (!txn_tile(&tc, (uint8_t)tcol, (uint8_t)trow, &col)) {
                return -1;
            }
            xdna_log(npu, XDNA_LOG_DEBUG,
                     "ctrlcode: TCT tile(%u,%u) %s kanal %u -- senkron model, "
                     "token hazir",
                     col, trow, dir ? "MM2S" : "S2MM", tch);
            break;
        }

        case XAIE_IO_PREEMPT:
            op_size = AIE_TXN_PREEMPT_SIZE;
            tc.skipped++;
            xdna_log(npu, XDNA_LOG_WARN,
                     "ctrlcode: op %u PREEMPT uygulanmadi", op_index);
            break;

        case XAIE_IO_LOADPDI:
            op_size = AIE_TXN_LOADPDI_SIZE;
            tc.skipped++;
            xdna_log(npu, XDNA_LOG_WARN,
                     "ctrlcode: op %u LOADPDI uygulanmadi (overlay yuklemesi yok)",
                     op_index);
            break;

        case XAIE_IO_LOAD_PM_START:
            op_size = AIE_TXN_PMLOAD_SIZE;
            tc.skipped++;
            xdna_log(npu, XDNA_LOG_WARN,
                     "ctrlcode: op %u LOAD_PM_START uygulanmadi", op_index);
            break;

        case XAIE_IO_CUSTOM_OP_DDR_PATCH:
        case XAIE_IO_CUSTOM_OP_READ_REGS:
        case XAIE_IO_CUSTOM_OP_RECORD_TIMER:
        case XAIE_IO_CUSTOM_OP_MERGE_SYNC:
        case XAIE_CONFIG_SHIMDMA_BD:
        case XAIE_CONFIG_SHIMDMA_DMABUF_BD: {
            /*
             * Taniyoruz ama uygulamiyoruz. Kayit kendi boyutunu tasidigi
             * icin dogru atlayabiliyoruz; sessizce gecmiyoruz.
             */
            CustomOpHdr c;

            if (pos + sizeof(c) > size) {
                goto truncated;
            }
            memcpy(&c, buf + pos, sizeof(c));
            op_size = c.Size;
            tc.skipped++;
            xdna_log(npu, XDNA_LOG_WARN,
                     "ctrlcode: op %u opcode %u uygulanmadi, %u bayt atlandi",
                     op_index, op.Op, op_size);
            break;
        }

        default:
            /*
             * Tanimadigimiz opcode. Boyutunu BILEMEYIZ: bazi op'lar sabit
             * boyutlu baslik kullaniyor (NOOP, PREEMPT, LOADPDI,
             * LOAD_PM_START), bazilari Size alani tasiyor. Custom op
             * varsayip Size okumak coplu bir deger verir ve ctrlcode'un
             * geri kalanini yanlis cozer. Bu yuzden hata donuyoruz.
             */
            xdna_log(npu, XDNA_LOG_ERROR,
                     "ctrlcode: op %u bilinmeyen opcode %u -- boyutu "
                     "bilinemedigi icin duruldu",
                     op_index, op.Op);
            return -1;
        }

        if (op_size == 0 || (op_size & 3u) != 0) {
            xdna_log(npu, XDNA_LOG_ERROR,
                     "ctrlcode: op %u gecersiz boyut %u", op_index, op_size);
            return -1;
        }
        pos += op_size;
        op_index++;
        tc.arr->stats.txn_ops++;
    }

    if (hdr.NumOps && op_index != hdr.NumOps) {
        xdna_log(npu, XDNA_LOG_WARN,
                 "ctrlcode: baslik %u op bildirdi, %u op islendi", hdr.NumOps,
                 op_index);
    }
    if (tc.skipped) {
        xdna_log(npu, XDNA_LOG_WARN,
                 "ctrlcode: %u op atlandi -- sonuc eksik olabilir",
                 tc.skipped);
    }
    return 0;

truncated:
    xdna_log(npu, XDNA_LOG_ERROR, "ctrlcode: op %u tampona sigmiyor (pos %u)",
             op_index, pos);
    return -1;
}

int xdna_txn_execute(XdnaNpu *npu, uint32_t ctx_id, const uint8_t *buf,
                     uint32_t size)
{
    int ret = txn_run(npu, ctx_id, buf, size);

    /*
     * Cihaz bellegi penceresi yalnizca bu context'in yurutmesi boyunca
     * gecerli; hata yollarinda da temizlenmeli.
     */
    npu->array->devm_heap_addr = 0;
    npu->array->devm_heap_size = 0;
    return ret;
}
