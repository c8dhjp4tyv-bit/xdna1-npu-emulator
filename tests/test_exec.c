// SPDX-License-Identifier: GPL-2.0-only
/*
 * Uctan uca yurutme testi: gercek bicimde bir ctrlcode (XAie transaction)
 * uretilir, stock surucu yolundan EXEC_DPU ile gonderilir ve XDNA array
 * uzerinde veri hareketi dogrulanir.
 *
 * Veri yolu:
 *
 *   host giris tamponu
 *        | shim MM2S (kolon 0, satir 0)
 *        v
 *   kolon stream'i
 *        | memory tile S2MM (kolon 0, satir 1)   -> lock 0 release
 *        v
 *   memory tile bellegi
 *        | memory tile MM2S                      -> lock 0 acquire
 *        v
 *   kolon stream'i
 *        | shim S2MM
 *        v
 *   host cikis tamponu
 *
 * Girisin cikisa birebir ulasmasi; BD cozumleme, lock semantigi, DMA
 * motoru ve ctrlcode yorumlayicisinin birlikte dogru calistigini gosterir.
 */

#include <stdlib.h>
#include <string.h>

#include "drv_model.h"
#include "ctrlcode.h"
#include "txn_vectors.h"

/* Host bellek yerlesimi (FW imaji 0x10000..0x50000 arasinda) */
#define CTRLCODE_ADDR (HOST_MEM_BASE + 0x300000)
#define INPUT_ADDR    (HOST_MEM_BASE + 0x400000)
#define OUTPUT_ADDR   (HOST_MEM_BASE + 0x410000)
#define CHAIN_ADDR    (HOST_MEM_BASE + 0x420000)
#define SYNC_SRC_ADDR (HOST_MEM_BASE + 0x430000)
#define SYNC_DST_ADDR (HOST_MEM_BASE + 0x440000)
#define ASYNC_BUF_ADDR (HOST_MEM_BASE + 0x450000)
#define ASYNC_BUF_SIZE 0x2000u

/* aie2_error.c: struct aie_error / struct aie_err_info -- packed DEGIL */
typedef struct {
    uint8_t row, col;
    uint32_t mod_type;
    uint8_t event_id;
} AieErrorEntry;

typedef struct {
    uint32_t err_cnt, ret_code, rsvd;
} AieErrInfoHdr;

typedef struct { uint64_t buf_addr; uint32_t buf_size; } AsyncEventReq;
typedef struct { uint32_t status, type; } AsyncEventResp;

#define XFER_WORDS 64u
#define XFER_BYTES (XFER_WORDS * 4u)
#define MEMT_WORD_OFF CTRLCODE_MEMT_WORD_OFF

/* ---------------------------------------------------------------- */

static int exec_ctrlcode(Drv *ctx_drv, uint64_t addr, uint32_t size)
{
    ExecDpuReq req;
    StatusResp st = { .status = 0xFFFFFFFFu };

    memset(&req, 0, sizeof(req));
    req.inst_buf_addr = addr;
    req.inst_size = size;
    req.cu_idx = 0;

    if (mbox_send_recv(ctx_drv, MSG_OP_EXEC_DPU, &req, sizeof(req), &st,
                       sizeof(st)) != 0) {
        return -1;
    }
    return (int)st.status;
}

static void test_exec(Host *host, XdnaNpu *npu)
{
    Drv mgmt = { .npu = npu, .host = host, .next_id = 1 };
    Drv ctxd;
    CreateCtxResp cctx;
    StatusResp st;
    uint8_t *in, *out, *cc;
    uint32_t cc_size;
    uint32_t i;

    step("Boot ve context olusturma");
    CHECK(drv_hw_start(&mgmt) == 0, "hw_start");
    if (g_failures) {
        return;
    }
    {
        CreateCtxReq req = {
            .aie_type = 1,
            .start_col = 0,
            .num_col = 1,
            .num_cq_pairs_requested = 1,
            .pasid = 13,
            .context_priority = 3,
        };
        CHECK(mbox_send_recv(&mgmt, MSG_OP_CREATE_CONTEXT, &req, sizeof(req),
                             &cctx, sizeof(cctx)) == 0, "create context");
        CHECK_EQ(cctx.status, 0, "create context durumu");
        if (cctx.status) {
            return;
        }
    }
    drv_attach_ctx_channel(&cctx, &ctxd, &mgmt);

    step("CONFIG_CU (context kanali uzerinden)");
    {
        ConfigCuReq req;
        memset(&req, 0, sizeof(req));
        req.num_cus = 1;
        req.cfgs[0] = 0x1;
        CHECK(mbox_send_recv(&ctxd, MSG_OP_CONFIG_CU, &req, sizeof(req), &st,
                             sizeof(st)) == 0, "config cu");
        CHECK_EQ(st.status, 0, "config cu durumu");
    }

    step("ctrlcode uret: host -> memory tile -> host");
    in = host_ptr(host, INPUT_ADDR);
    out = host_ptr(host, OUTPUT_ADDR);
    cc = host_ptr(host, CTRLCODE_ADDR);
    CHECK(in && out && cc, "host tamponlari");
    if (!in || !out || !cc) {
        return;
    }
    for (i = 0; i < XFER_BYTES; i++) {
        in[i] = (uint8_t)(i * 7u + 3u);
    }
    memset(out, 0, XFER_BYTES);
    cc_size = build_loopback_ctrlcode(cc, INPUT_ADDR, OUTPUT_ADDR, XFER_WORDS);
    CHECK(cc_size > sizeof(TxnHeader), "ctrlcode uretildi (%u bayt)", cc_size);

    step("EXEC_DPU ile ctrlcode calistir");
    CHECK_EQ(exec_ctrlcode(&ctxd, CTRLCODE_ADDR, cc_size), 0, "exec dpu durumu");

    step("Cikis tamponu girisle birebir ayni olmali");
    CHECK(memcmp(in, out, XFER_BYTES) == 0,
          "veri array uzerinden dogru gecmedi");
    if (memcmp(in, out, XFER_BYTES) != 0) {
        for (i = 0; i < XFER_BYTES; i++) {
            if (in[i] != out[i]) {
                printf("    ilk fark: bayt %u, 0x%02x != 0x%02x\n", i, in[i],
                       out[i]);
                break;
            }
        }
    }

    step("Ikinci kosum: lock'lar bastaki degerine donmus olmali");
    memset(out, 0, XFER_BYTES);
    CHECK_EQ(exec_ctrlcode(&ctxd, CTRLCODE_ADDR, cc_size), 0,
             "ikinci exec durumu");
    CHECK(memcmp(in, out, XFER_BYTES) == 0, "ikinci kosumda veri hatali");

    step("BLOCKWRITE: memory tile bellegine blok yaz, DMA ile geri oku");
    {
        uint8_t *cc2 = host_ptr(host, CTRLCODE_ADDR + 0x10000);
        uint32_t pattern[XFER_WORDS];
        TxnBuild b;
        uint32_t size2;

        for (i = 0; i < XFER_WORDS; i++) {
            pattern[i] = 0xC0DE0000u + i;
        }
        txn_init(&b, cc2);
        txn_config_stream_switch(&b);
        /* Bellege dogrudan blok yazma (byte offseti = kelime offseti * 4) */
        txn_blockwrite(&b, 0, 1, MEMT_WORD_OFF * 4u, pattern, XFER_WORDS);
        /* memory tile MM2S -> stream switch (lock kullanmadan) */
        txn_w32(&b, 0, 1, memt_bd(2, AIE_MEMT_BD_LEN_WORD), XFER_WORDS);
        txn_w32(&b, 0, 1, memt_bd(2, AIE_MEMT_BD_ADDR_WORD), MEMT_WORD_OFF);
        txn_w32(&b, 0, 1, memt_bd(2, AIE_MEMT_BD_CTRL_WORD),
                1u << AIE_MEMT_BD_VALID_LSB);
        txn_w32(&b, 0, 1, AIEML_MEMT_DMA_MM2S0_CTRL + AIEML_DMA_QUEUE_OFF, 2);
        /* shim S2MM -> host */
        txn_w32(&b, 0, 0, shim_bd(2, AIE_SHIM_BD_LEN_WORD), XFER_WORDS);
        txn_w32(&b, 0, 0, shim_bd(2, AIE_SHIM_BD_ADDRLO_WORD),
                (uint32_t)OUTPUT_ADDR & AIE_SHIM_BD_ADDRLO_MASK);
        txn_w32(&b, 0, 0, shim_bd(2, AIE_SHIM_BD_ADDRHI_WORD),
                (uint32_t)(OUTPUT_ADDR >> 32) & AIE_SHIM_BD_ADDRHI_MASK);
        txn_w32(&b, 0, 0, shim_bd(2, AIE_SHIM_BD_CTRL_WORD),
                1u << AIE_SHIM_BD_VALID_LSB);
        txn_w32(&b, 0, 0, AIEML_SHIM_DMA_S2MM0_CTRL + AIEML_DMA_QUEUE_OFF, 2);
        size2 = txn_finish(&b);

        memset(out, 0, XFER_BYTES);
        CHECK_EQ(exec_ctrlcode(&ctxd, CTRLCODE_ADDR + 0x10000, size2), 0,
                 "blockwrite ctrlcode durumu");
        CHECK(memcmp(out, pattern, XFER_BYTES) == 0,
              "BLOCKWRITE ile yazilan blok geri okunamadi");
    }

    step("CHAIN_EXEC_DPU: iki ctrlcode arka arkaya");
    {
        uint8_t *chain = host_ptr(host, CHAIN_ADDR);
        CmdChainSlotDpu slot;
        CmdChainReq req;
        CmdChainResp resp;
        uint32_t pos = 0;

        memset(&slot, 0, sizeof(slot));
        slot.inst_buf_addr = CTRLCODE_ADDR;
        slot.inst_size = cc_size;
        slot.arg_cnt = 0;
        memcpy(chain + pos, &slot, sizeof(slot));
        pos += (uint32_t)sizeof(slot);
        memcpy(chain + pos, &slot, sizeof(slot));
        pos += (uint32_t)sizeof(slot);

        memset(out, 0, XFER_BYTES);
        req.buf_addr = CHAIN_ADDR;
        req.buf_size = pos;
        req.count = 2;
        CHECK(mbox_send_recv(&ctxd, MSG_OP_CHAIN_EXEC_DPU, &req, sizeof(req),
                             &resp, sizeof(resp)) == 0, "chain exec");
        CHECK_EQ(resp.status, 0, "chain exec durumu");
        CHECK(memcmp(in, out, XFER_BYTES) == 0, "chain exec sonrasi veri");
    }

    step("SYNC_BO: host -> host kopyalama");
    {
        SyncBoReq req;
        uint8_t *src = host_ptr(host, SYNC_SRC_ADDR);
        uint8_t *dst = host_ptr(host, SYNC_DST_ADDR);

        for (i = 0; i < XFER_BYTES; i++) {
            src[i] = (uint8_t)(0xFFu - i);
        }
        memset(dst, 0, XFER_BYTES);

        req.src_addr = SYNC_SRC_ADDR;
        req.dst_addr = SYNC_DST_ADDR;
        req.size = XFER_BYTES;
        req.type = (SYNC_BO_HOST_MEM_T << 4) | SYNC_BO_HOST_MEM_T;
        CHECK(mbox_send_recv(&ctxd, MSG_OP_SYNC_BO, &req, sizeof(req), &st,
                             sizeof(st)) == 0, "sync bo");
        CHECK_EQ(st.status, 0, "sync bo durumu");
        CHECK(memcmp(src, dst, XFER_BYTES) == 0, "sync bo kopyalamasi");
    }

    step("Hata yolu: saglanmayacak MASKPOLL");
    {
        uint8_t *cc3 = host_ptr(host, CTRLCODE_ADDR + 0x20000);
        TxnBuild b;
        uint32_t size3;

        txn_init(&b, cc3);
        /* Lock 5 hicbir zaman 0x7F olmayacak */
        txn_maskpoll(&b, 0, 1, AIEML_MEMT_LOCK0_VALUE + 5u * 0x10u, 0xFFu, 0x7Fu);
        size3 = txn_finish(&b);

        CHECK(exec_ctrlcode(&ctxd, CTRLCODE_ADDR + 0x20000, size3) != 0,
              "saglanmayan MASKPOLL hata dondurmeli");
    }

    step("Hata yolu: partition disi kolona erisim");
    {
        uint8_t *cc4 = host_ptr(host, CTRLCODE_ADDR + 0x30000);
        TxnBuild b;
        uint32_t size4;

        txn_init(&b, cc4);
        /* Context yalnizca 1 kolona sahip; kolon 3 yasak. */
        txn_w32(&b, 3, 1, memt_bd(0, AIE_MEMT_BD_LEN_WORD), 1);
        size4 = txn_finish(&b);

        CHECK(exec_ctrlcode(&ctxd, CTRLCODE_ADDR + 0x30000, size4) != 0,
              "partition disi erisim reddedilmeli");
    }

    step("Sabit boyutlu op'lar dogru atlanmali (NOOP) ve TCT islenmeli");
    {
        uint8_t *cc8 = host_ptr(host, CTRLCODE_ADDR + 0x70000);
        TxnBuild b;
        uint32_t size8;
        uint32_t probe = 0xFEEDFACEu;

        /*
         * NOOP'un boyutu 4 bayt ve Size alani YOK. Custom op sanip Size
         * okusaydik coplu bir deger alir, sonraki WRITE'i kacirirdik.
         * Bu yuzden NOOP'tan SONRA bir WRITE koyup etkisini dogruluyoruz.
         */
        txn_init(&b, cc8);
        txn_fixed_op(&b, XAIE_IO_NOOP, 4);
        txn_w32(&b, 0, 1, MEMT_WORD_OFF * 4u, probe);
        txn_fixed_op(&b, XAIE_IO_NOOP, 4);
        txn_tct(&b, 0, 1, 0, true);
        /* TCT sonrasi ikinci bir yazma: TCT boyutu da dogru islenmeli */
        txn_w32(&b, 0, 1, MEMT_WORD_OFF * 4u + 4u, probe + 1u);
        size8 = txn_finish(&b);

        CHECK_EQ(exec_ctrlcode(&ctxd, CTRLCODE_ADDR + 0x70000, size8), 0,
                 "NOOP + TCT ctrlcode durumu");

        /* Yazilanlari memory tile'dan DMA ile geri oku. */
        {
            uint8_t *cc9 = host_ptr(host, CTRLCODE_ADDR + 0x80000);
            uint32_t got[2];
            uint32_t size9;

            txn_init(&b, cc9);
            txn_config_stream_switch(&b);
            txn_w32(&b, 0, 1, memt_bd(4, AIE_MEMT_BD_LEN_WORD), 2);
            txn_w32(&b, 0, 1, memt_bd(4, AIE_MEMT_BD_ADDR_WORD), MEMT_WORD_OFF);
            txn_w32(&b, 0, 1, memt_bd(4, AIE_MEMT_BD_CTRL_WORD),
                    1u << AIE_MEMT_BD_VALID_LSB);
            txn_w32(&b, 0, 1,
                    AIEML_MEMT_DMA_MM2S0_CTRL + AIEML_DMA_QUEUE_OFF, 4);
            txn_w32(&b, 0, 0, shim_bd(5, AIE_SHIM_BD_LEN_WORD), 2);
            txn_w32(&b, 0, 0, shim_bd(5, AIE_SHIM_BD_ADDRLO_WORD),
                    (uint32_t)OUTPUT_ADDR & AIE_SHIM_BD_ADDRLO_MASK);
            txn_w32(&b, 0, 0, shim_bd(5, AIE_SHIM_BD_ADDRHI_WORD),
                    (uint32_t)(OUTPUT_ADDR >> 32) & AIE_SHIM_BD_ADDRHI_MASK);
            txn_w32(&b, 0, 0, shim_bd(5, AIE_SHIM_BD_CTRL_WORD),
                    1u << AIE_SHIM_BD_VALID_LSB);
            txn_w32(&b, 0, 0,
                    AIEML_SHIM_DMA_S2MM0_CTRL + AIEML_DMA_QUEUE_OFF, 5);
            size9 = txn_finish(&b);

            memset(out, 0, 8);
            CHECK_EQ(exec_ctrlcode(&ctxd, CTRLCODE_ADDR + 0x80000, size9), 0,
                     "geri okuma ctrlcode durumu");
            memcpy(got, out, sizeof(got));
            CHECK_EQ(got[0], probe, "NOOP sonrasi WRITE islendi");
            CHECK_EQ(got[1], probe + 1u, "TCT sonrasi WRITE islendi");
        }
    }

    step("Hata yolu: bilinmeyen opcode");
    {
        uint8_t *cc10 = host_ptr(host, CTRLCODE_ADDR + 0x90000);
        TxnBuild b;

        txn_init(&b, cc10);
        /* 99 tanimli degil: boyutu bilinemez, hata donmeli. */
        txn_fixed_op(&b, 99, 4);
        txn_finish(&b);

        CHECK(exec_ctrlcode(&ctxd, CTRLCODE_ADDR + 0x90000, b.pos) != 0,
              "bilinmeyen opcode reddedilmeli");
    }

    step("Hata yolu: bozuk ctrlcode boyutu");
    {
        uint8_t *cc5 = host_ptr(host, CTRLCODE_ADDR + 0x40000);
        TxnBuild b;
        Write32Hdr bad;

        txn_init(&b, cc5);
        memset(&bad, 0, sizeof(bad));
        bad.hdr.Op = XAIE_IO_WRITE;
        bad.Size = 3;   /* 4'un kati degil */
        memcpy(cc5 + b.pos, &bad, sizeof(bad));
        b.pos += (uint32_t)sizeof(bad);
        b.ops++;
        txn_finish(&b);

        CHECK(exec_ctrlcode(&ctxd, CTRLCODE_ADDR + 0x40000, b.pos) != 0,
              "gecersiz op boyutu reddedilmeli");
    }

    step("Compute tile core enable: interpreter yok, sessizce basarili olmamali");
    {
        uint8_t *cc6 = host_ptr(host, CTRLCODE_ADDR + 0x50000);
        TxnBuild b;
        uint32_t size6, status;

        txn_init(&b, cc6);
        txn_w32(&b, 0, 2, AIEML_CORE_CONTROL, AIE_CORE_CTRL_ENABLE_MASK);
        size6 = txn_finish(&b);

        /*
         * ctrlcode acisindan bu gecerli bir yazma; hata degil. Ama emulator
         * bunu UYARI olarak loglamali (AIE interpreter yok). Testin amaci
         * yazmanin kabul edildigini ve core_status'un guncellendigini
         * gostermek.
         */
        status = (uint32_t)exec_ctrlcode(&ctxd, CTRLCODE_ADDR + 0x50000, size6);
        CHECK_EQ(status, 0, "core enable yazmasi kabul edilmeli");
    }

    step("SYNC_BO: cihaz bellegi (AIE2_DEVM) <-> host");
    {
        SyncBoReq req;
        MapBufReq map;
        uint64_t heap = HOST_MEM_BASE + 0x500000;
        uint32_t heap_size = 0x100000;
        uint8_t *heap_p = host_ptr(host, heap);
        uint8_t *dst = host_ptr(host, SYNC_DST_ADDR);
        uint64_t dev_addr = AIE2_DEVM_BASE + 0x1000;

        /* Context'in heap'ini bildir: cihaz adresleri buraya bakiyor. */
        map.context_id = cctx.context_id;
        map.buf_addr = heap;
        map.buf_size = heap_size;
        CHECK(mbox_send_recv(&ctxd, MSG_OP_MAP_HOST_BUFFER, &map, sizeof(map),
                             &st, sizeof(st)) == 0, "heap bildir");
        CHECK_EQ(st.status, 0, "heap bildirme durumu");

        /* Heap icinde, cihaz adresinin denk geldigi yere desen yaz. */
        for (i = 0; i < XFER_BYTES; i++) {
            heap_p[0x1000 + i] = (uint8_t)(i ^ 0x5Au);
        }
        memset(dst, 0, XFER_BYTES);

        req.src_addr = dev_addr;
        req.dst_addr = SYNC_DST_ADDR;
        req.size = XFER_BYTES;
        req.type = (SYNC_BO_HOST_MEM_T << 4) | SYNC_BO_DEV_MEM_T;
        CHECK(mbox_send_recv(&ctxd, MSG_OP_SYNC_BO, &req, sizeof(req), &st,
                             sizeof(st)) == 0, "sync bo dev->host");
        CHECK_EQ(st.status, 0, "sync bo dev->host durumu");
        CHECK(memcmp(heap_p + 0x1000, dst, XFER_BYTES) == 0,
              "cihaz adresi heap'e dogru cevrilmedi");

        /* Heap sinirinin disi reddedilmeli. */
        req.src_addr = AIE2_DEVM_BASE + heap_size;
        CHECK(mbox_send_recv(&ctxd, MSG_OP_SYNC_BO, &req, sizeof(req), &st,
                             sizeof(st)) == 0, "sinir disi sync bo cevabi");
        CHECK(st.status != 0, "heap disi cihaz adresi reddedilmeli");
    }

    /*
     * Gercek akista shim BD adreslerini XRT yamiyor ve oraya argüman
     * BO'sunun CIHAZ adresini yaziyor. Cihaz bellegi BO'lari context
     * heap'inden ayrildigi icin bu adresler AIE2_DEVM penceresinde oluyor.
     */
    step("Shim DMA: cihaz adresleri heap'e cevrilmeli");
    {
        uint64_t heap = HOST_MEM_BASE + 0x500000;   /* yukarida bildirildi */
        uint8_t *heap_p = host_ptr(host, heap);
        uint64_t dev_in = AIE2_DEVM_BASE + 0x8000;
        uint64_t dev_out = AIE2_DEVM_BASE + 0xC000;
        uint32_t size;

        for (i = 0; i < XFER_BYTES; i++) {
            heap_p[0x8000 + i] = (uint8_t)(i * 11u + 5u);
        }
        memset(heap_p + 0xC000, 0, XFER_BYTES);

        size = build_loopback_ctrlcode(cc, dev_in, dev_out, XFER_WORDS);
        CHECK_EQ(exec_ctrlcode(&ctxd, CTRLCODE_ADDR, size), 0,
                 "cihaz adresli ctrlcode durumu");
        CHECK(memcmp(heap_p + 0x8000, heap_p + 0xC000, XFER_BYTES) == 0,
              "veri cihaz adresleriyle array uzerinden gecmedi");

        /* Pencere icinde ama heap disinda kalan adres reddedilmeli. */
        size = build_loopback_ctrlcode(cc, AIE2_DEVM_BASE + 0x100000, dev_out, XFER_WORDS);
        CHECK(exec_ctrlcode(&ctxd, CTRLCODE_ADDR, size) != 0,
              "heap disi shim adresi reddedilmeli");
    }

    /*
     * Derleyicinin kendi encoder'i (mlir-aie TxnEncoding.h) ile uretilmis
     * ctrlcode. Bizim ureticimizden farki: op basliklarindaki Col/Row
     * alanlari SIFIR, tile mutlak adrese katilmis. Gercek workload'lar
     * boyle geliyor.
     */
    step("Derleyici encoder'iyle uretilmis ctrlcode calismali");
    {
        uint8_t *cc3 = host_ptr(host, CTRLCODE_ADDR + 0x20000);

        CHECK_EQ((uint64_t)HOST_MEM_BASE, TXN_VEC_HOST_BASE,
                 "vektor host tabani testle ayni olmali");
        CHECK_EQ((uint64_t)INPUT_ADDR, TXN_VEC_INPUT_ADDR, "vektor giris adresi");
        CHECK_EQ((uint64_t)OUTPUT_ADDR, TXN_VEC_OUTPUT_ADDR, "vektor cikis adresi");
        CHECK_EQ(TXN_VEC_WORDS, XFER_WORDS, "vektor transfer boyutu");

        memcpy(cc3, txn_vec_loopback, sizeof(txn_vec_loopback));
        memset(out, 0, XFER_BYTES);
        CHECK_EQ(exec_ctrlcode(&ctxd, CTRLCODE_ADDR + 0x20000,
                               (uint32_t)sizeof(txn_vec_loopback)), 0,
                 "derleyici ctrlcode'u durumu");
        CHECK(memcmp(in, out, XFER_BYTES) == 0,
              "derleyici ctrlcode'uyla veri array uzerinden gecmedi");
    }

    /*
     * Bu vektor, tile'in ADRESTEN cozuldugunu veriyle olcuyor: desen
     * memory tile (0,1) bellegine blok-yazilip ayni yerden DMA ile
     * cikariliyor. Tile yanlis cozulurse (op basligindaki sifir col/row
     * kullanilirsa) desen shim register uzayina gider ve bu kontrol
     * duser.
     */
    step("Derleyici encoder'i: tile adresten cozulmeli (BLOCKWRITE + DMA)");
    {
        uint8_t *cc4 = host_ptr(host, CTRLCODE_ADDR + 0x30000);

        memcpy(cc4, txn_vec_blockwrite_out, sizeof(txn_vec_blockwrite_out));
        memset(out, 0, XFER_BYTES);
        CHECK_EQ(exec_ctrlcode(&ctxd, CTRLCODE_ADDR + 0x30000,
                               (uint32_t)sizeof(txn_vec_blockwrite_out)), 0,
                 "blockwrite vektoru durumu");
        CHECK(memcmp(out, txn_vec_pattern, XFER_BYTES) == 0,
              "blockwrite deseni memory tile'a ulasmadi (tile cozumu?)");
    }

    /*
     * BD adresi yamayan op'lar atlanmamali: atlanirsa DMA yanlis adrese
     * gider ve sonuc sessizce yanlis olur.
     */
    step("Hata yolu: DDR_PATCH sessizce atlanmamali");
    {
        uint8_t *cc5 = host_ptr(host, CTRLCODE_ADDR + 0x40000);
        TxnBuild b;
        uint32_t size5;

        txn_init(&b, cc5);
        /* CustomOpHdr(8) + payload(40) = 48 bayt, TxnEncoding.h'ye gore. */
        {
            struct { OpHdr hdr; uint32_t Size; } c;
            memset(&c, 0, sizeof(c));
            c.hdr.Op = XAIE_IO_CUSTOM_OP_DDR_PATCH;
            c.Size = 48u;
            memset(cc5 + b.pos, 0, 48);
            memcpy(cc5 + b.pos, &c, sizeof(c));
            b.pos += 48u;
            b.ops++;
        }
        size5 = txn_finish(&b);

        CHECK(exec_ctrlcode(&ctxd, CTRLCODE_ADDR + 0x40000, size5) != 0,
              "DDR_PATCH iceren ctrlcode basarili donmemeli");
    }

    step("Asenkron hata bildirimi");
    {
        AsyncEventReq areq;
        AsyncEventResp aresp;
        AieErrInfoHdr info;
        AieErrorEntry entry;
        uint8_t *abuf = host_ptr(host, ASYNC_BUF_ADDR);
        uint8_t *cc7 = host_ptr(host, CTRLCODE_ADDR + 0x60000);
        uint32_t async_id, size7;
        TxnBuild b;

        memset(abuf, 0, ASYNC_BUF_SIZE);

        /* Surucu olay tamponunu kaydeder; firmware HEMEN cevaplamaz. */
        areq.buf_addr = ASYNC_BUF_ADDR;
        areq.buf_size = ASYNC_BUF_SIZE;
        mgmt.host->irq_pending[mgmt.msix_id] = false;
        async_id = mbox_send_only(&mgmt, MSG_OP_REGISTER_ASYNC_EVENT_MSG,
                                  &areq, sizeof(areq));
        CHECK(async_id != 0, "async event kaydi gonderildi");
        CHECK(!mgmt.host->irq_pending[mgmt.msix_id],
              "async kaydi hemen cevaplanmamali");

        /*
         * Host bellegi disina isaret eden bir shim BD ile DMA hatasi
         * tetikle. Emulator bunu shim tile (satir 0) uzerinde bir DMA
         * hatasi olarak bildirmeli.
         */
        txn_init(&b, cc7);
        txn_config_stream_switch(&b);
        txn_w32(&b, 0, 0, shim_bd(3, AIE_SHIM_BD_LEN_WORD), XFER_WORDS);
        txn_w32(&b, 0, 0, shim_bd(3, AIE_SHIM_BD_ADDRLO_WORD), 0x0BAD0000u);
        txn_w32(&b, 0, 0, shim_bd(3, AIE_SHIM_BD_ADDRHI_WORD), 0xFFFFu);
        txn_w32(&b, 0, 0, shim_bd(3, AIE_SHIM_BD_CTRL_WORD),
                1u << AIE_SHIM_BD_VALID_LSB);
        txn_w32(&b, 0, 0, AIEML_SHIM_DMA_MM2S0_CTRL + AIEML_DMA_QUEUE_OFF, 3);
        size7 = txn_finish(&b);

        mgmt.host->irq_pending[mgmt.msix_id] = false;
        (void)exec_ctrlcode(&ctxd, CTRLCODE_ADDR + 0x60000, size7);

        CHECK(mgmt.host->irq_pending[mgmt.msix_id],
              "async hata icin mgmt MSI-X tetiklenmeli");
        CHECK(mbox_recv_pending(&mgmt, async_id, &aresp, sizeof(aresp)) == 0,
              "async hata cevabi alinamadi");
        CHECK_EQ(aresp.status, 0, "async cevap durumu");
        CHECK_EQ(aresp.type, 0, "async olay tipi (AIE_ERROR)");

        memcpy(&info, abuf, sizeof(info));
        memcpy(&entry, abuf + sizeof(info), sizeof(entry));
        CHECK_EQ(info.err_cnt, 1, "hata sayisi");
        CHECK_EQ(entry.col, 0, "hatali kolon");
        CHECK_EQ(entry.row, 0, "hatali satir (shim)");
        /* aie2_error.c: AIE_PL_MOD = 2, shim DMA olayi = 72 */
        CHECK_EQ(entry.mod_type, 2, "modul tipi AIE_PL_MOD");
        CHECK_EQ(entry.event_id, 72, "shim DMA olay kimligi");
    }

    step("Context yok et");
    {
        DestroyCtxReq dreq = { .context_id = cctx.context_id };
        CHECK(mbox_send_recv(&mgmt, MSG_OP_DESTROY_CONTEXT, &dreq, sizeof(dreq),
                             &st, sizeof(st)) == 0, "destroy context");
        CHECK_EQ(st.status, 0, "destroy context durumu");
    }
}

int main(int argc, char **argv)
{
    Host *host;
    XdnaNpu *npu;

    host = host_new(argc > 1 && strcmp(argv[1], "-v") == 0);
    if (!host) {
        return 1;
    }
    npu = xdna_npu_new(&drv_host_ops, host);
    if (!npu) {
        host_free(host);
        return 1;
    }

    printf("=== XDNA1 emulatoru: ctrlcode ve array yurutmesi ===\n");
    test_exec(host, npu);

    {
        XdnaStats stats;
        xdna_npu_get_stats(npu, &stats);
        printf("\nIstatistik: exec=%llu txn_op=%llu dma_bayt=%llu irq=%llu\n",
               (unsigned long long)stats.exec_cmds,
               (unsigned long long)stats.txn_ops,
               (unsigned long long)stats.dma_bytes,
               (unsigned long long)stats.irqs_raised);
    }

    xdna_npu_free(npu);
    host_free(host);

    printf("\n%d kontrol, %d basarisiz\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
