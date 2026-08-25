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
 * DDR_PATCH payload.
 *
 * Yerlesim DERLEYICININ KENDI ENCODER'INDAN dogrulandi -- mlir-aie
 * include/aie/Runtime/TxnEncoding.h, txn_append_address_patch():
 *
 *   w0  = opcode (DDR_PATCH)          } CustomOpHdr
 *   w1  = op boyutu (48 bayt)         }
 *   w2..w4 = rezerve (0)
 *   w5  = action (0 = patch)
 *   w6  = yamanacak register adresi
 *   w7  = rezerve (0)
 *   w8  = buffer argumaninin indeksi
 *   w9  = rezerve (0)
 *   w10 = argplus -- buffer icindeki BAYT offseti
 *   w11 = rezerve (0)
 *
 * Anlam (AIETargetNPU.cpp + AIEDmaToNpu.cpp): yamanacak adres bir shim
 * BD'sinin adres kelimesidir (getDmaBdAddress + getDmaBdAddressOffset) ve
 * oraya `argidx` numarali host buffer argumaninin adresi + `argplus`
 * yazilir. Derleyici BD'yi once blok-yazip adres alanini sifir biraktigi
 * icin "yaz" ile "ekle" pratikte ayni sonucu verir.
 *
 * OP HALA UYGULANMIYOR. Kodlama artik tam biliniyor ama iki nokta
 * dogrulanmadi ve ikisi de yanlis olursa DMA yanlis adrese gider:
 *
 *   1. Argumanlarin komut icindeki paketlenmesi. exec_dpu_req.payload
 *      "properties and regular kernel arguments" tutuyor ve
 *      inst_prop_cnt kadar property onde geliyor; buffer argumanlari
 *      64-bit mi (2 kelime) yoksa 32-bit mi indeksleniyor dogrulanmadi.
 *   2. 64-bit adresin shim BD'sinin ADDRLO/ADDRHI kelimelerine nasil
 *      bolundugu ve ADDRHI'nin diger alanlarinin nasil korundugu.
 *
 * Ayrica firmware, "instruction buffer" calisma kipinde ILK BES host
 * argumanina 0x80000000 ekliyor (AIETargetNPU.cpp: kDDRAIEAddrOffset,
 * kNumFirmwareTranslatedArgs); sonrakiler icin derleyici bu offseti
 * argplus'a kendisi katiyor. Hangi kipte oldugumuzu komuttan ayirt
 * edemiyoruz.
 *
 * Uydurup uygulamak, sessizce yanlis sonuc ureten tek yer olurdu.
 */
typedef struct {
    uint32_t rsvd[3];
    uint32_t action;
    uint64_t regaddr;
    uint64_t argidx;
    uint64_t argplus;
} PatchOp;

static_assert(sizeof(TctOp) == 8, "TCT payload");
static_assert(sizeof(PatchOp) == 40, "DDR_PATCH payload (TxnEncoding.h)");
static_assert(sizeof(CustomOpHdr) + sizeof(PatchOp) == 48,
              "DDR_PATCH op boyutu 12 kelime");

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

/*
 * Tile'i ADRESTEN coz.
 *
 * Op basligindaki Col/Row alanlarina GUVENILMEZ: mlir-aie derleyicisi
 * (include/aie/Runtime/TxnEncoding.h, "the single in-tree source of truth
 * for the instruction format") write32 icin o kelimeyi rezerve birakip
 * SIFIR yaziyor ve tile'i mutlak adrese katiyor. aie-rt'nin kendi
 * playback'i (XAie_TxnPlay) da adresi dogrudan kullaniyor; OpHdr'daki
 * Col/Row yalnizca relocation icin bilgi amacli.
 *
 * Adresten cozmezsek derleyici ciktisindaki HER yazma tile (0,0)'a
 * giderdi -- yani gercek hicbir workload calismazdi.
 *
 * Baslik alanlari sifir disi ise (bizim ureticimiz gibi) adresle
 * tutarli olmalari beklenir; tutarsizlik acikca raporlanir.
 */
static bool txn_tile_from_addr(TxnCtx *tc, uint64_t reg_off, uint8_t hdr_col,
                               uint8_t hdr_row, uint8_t *out_col,
                               uint8_t *out_row, uint32_t *out_off)
{
    uint8_t col = (uint8_t)AIE_ADDR_COL(reg_off);
    uint8_t row = (uint8_t)AIE_ADDR_ROW(reg_off);

    if ((hdr_col || hdr_row) && (hdr_col != col || hdr_row != row)) {
        xdna_log(tc->npu, XDNA_LOG_WARN,
                 "ctrlcode: op basligi tile (%u,%u) diyor ama adres (%u,%u) "
                 "-- adres esas alindi",
                 hdr_col, hdr_row, col, row);
    }

    *out_row = row;
    *out_off = (uint32_t)AIE_ADDR_OFF(reg_off);
    return txn_tile(tc, col, row, out_col);
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
        uint32_t off = 0;
        uint8_t col, row;

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
            if (!txn_tile_from_addr(&tc, w.RegOff, op.Col, op.Row, &col, &row,
                                    &off)) {
                return -1;
            }
            xdna_array_write32(tc.arr, col, row, off, w.Value);
            break;
        }

        case XAIE_IO_MASKWRITE: {
            MaskWrite32Hdr w;
            uint32_t cur;

            if (pos + sizeof(w) > size) {
                goto truncated;
            }
            memcpy(&w, buf + pos, sizeof(w));
            op_size = w.Size ? w.Size : (uint32_t)sizeof(w);
            if (!txn_tile_from_addr(&tc, w.RegOff, op.Col, op.Row, &col, &row,
                                    &off)) {
                return -1;
            }
            cur = xdna_array_read32(tc.arr, col, row, off);
            xdna_array_write32(tc.arr, col, row, off,
                               (cur & ~w.Mask) | (w.Value & w.Mask));
            break;
        }

        case XAIE_IO_MASKPOLL: {
            MaskWrite32Hdr w;   /* MaskPoll32Hdr ile ayni yerlesim */
            uint32_t tries = 0;
            bool ok = false;

            if (pos + sizeof(w) > size) {
                goto truncated;
            }
            memcpy(&w, buf + pos, sizeof(w));
            op_size = w.Size ? w.Size : (uint32_t)sizeof(w);
            if (!txn_tile_from_addr(&tc, w.RegOff, op.Col, op.Row, &col, &row,
                                    &off)) {
                return -1;
            }
            while (tries++ < TXN_MASKPOLL_LIMIT) {
                if ((xdna_array_read32(tc.arr, col, row, off) & w.Mask) ==
                    (w.Value & w.Mask)) {
                    ok = true;
                    break;
                }
            }
            if (!ok) {
                xdna_log(npu, XDNA_LOG_ERROR,
                         "ctrlcode: op %u MASKPOLL zaman asimi "
                         "(tile %u,%u offset 0x%x mask 0x%x deger 0x%x)",
                         op_index, col, row, off, w.Mask, w.Value);
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
            /*
             * BLOCKWRITE'ta derleyici col/row'u ayri bir kelimeye de
             * koyuyor ama adres yine mutlak; tile'i adresten cozuyoruz.
             */
            if (!txn_tile_from_addr(&tc, w.RegOff, op.Col, op.Row, &col, &row,
                                    &off)) {
                return -1;
            }
            {
                const uint8_t *payload = buf + pos + sizeof(w);
                uint32_t k;

                for (k = 0; k < words; k++) {
                    uint32_t v;
                    memcpy(&v, payload + k * 4u, 4);
                    xdna_array_write32(tc.arr, col, row, off + k * 4u, v);
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
            uint32_t tries = 0;
            bool ok = false;

            if (pos + sizeof(w) > size) {
                goto truncated;
            }
            memcpy(&w, buf + pos, sizeof(w));
            op_size = w.Size ? w.Size : (uint32_t)sizeof(w);
            if (!txn_tile_from_addr(&tc, w.RegOff, op.Col, op.Row, &col, &row,
                                    &off)) {
                return -1;
            }
            while (tries++ < TXN_MASKPOLL_LIMIT) {
                if ((xdna_array_read32(tc.arr, col, row, off) & w.Mask) ==
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
        case XAIE_CONFIG_SHIMDMA_BD:
        case XAIE_CONFIG_SHIMDMA_DMABUF_BD: {
            /*
             * Bu op'lar BD ADRESLERINI belirliyor. Atlanirlarsa DMA
             * yanlis adrese gider ve sonuc SESSIZCE yanlis olur -- yani
             * atlamak, hata dondurmekten daha kotu. Durup hata donuyoruz.
             * DDR_PATCH icin kodlama biliniyor, eksik olan ne oldugu
             * yukarida PatchOp yorumunda yaziyor.
             */
            xdna_log(npu, XDNA_LOG_ERROR,
                     "ctrlcode: op %u (opcode %u) BD adresi yamiyor ama "
                     "uygulanmadi -- sessizce yanlis sonuc uretmemek icin "
                     "duruldu",
                     op_index, op.Op);
            return -1;
        }

        case XAIE_IO_CUSTOM_OP_READ_REGS:
        case XAIE_IO_CUSTOM_OP_RECORD_TIMER:
        case XAIE_IO_CUSTOM_OP_MERGE_SYNC: {
            /*
             * Taniyoruz ama uygulamiyoruz. Bunlar teshis/olcum op'lari;
             * atlanmalari hesabin sonucunu degistirmiyor. Kayit kendi
             * boyutunu tasidigi icin dogru atlayabiliyoruz; sessizce
             * gecmiyoruz.
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
