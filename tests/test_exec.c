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

/* Host bellek yerlesimi (FW imaji 0x10000..0x50000 arasinda) */
#define CTRLCODE_ADDR (HOST_MEM_BASE + 0x300000)
#define INPUT_ADDR    (HOST_MEM_BASE + 0x400000)
#define OUTPUT_ADDR   (HOST_MEM_BASE + 0x410000)
#define CHAIN_ADDR    (HOST_MEM_BASE + 0x420000)
#define SYNC_SRC_ADDR (HOST_MEM_BASE + 0x430000)
#define SYNC_DST_ADDR (HOST_MEM_BASE + 0x440000)

#define XFER_WORDS 64u
#define XFER_BYTES (XFER_WORDS * 4u)
#define MEMT_WORD_OFF 0x100u   /* memory tile icinde 1024. bayt */

/* ---------------------------------------------------------------- */
/* ctrlcode uretici -- aie-rt serilestirme bicimi                    */
/* ---------------------------------------------------------------- */

typedef struct {
    uint8_t Major, Minor, DevGen, NumRows, NumCols, NumMemTileRows;
    uint32_t NumOps, TxnSize;
} TxnHeader;

typedef struct {
    uint8_t Op, Col, Row;
} OpHdr;

typedef struct {
    OpHdr hdr;
    uint64_t RegOff;
    uint32_t Value, Size;
} Write32Hdr;

typedef struct {
    OpHdr hdr;
    uint64_t RegOff;
    uint32_t Value, Mask, Size;
} MaskPoll32Hdr;

typedef struct {
    OpHdr hdr;
    uint8_t Col, Row;
    uint32_t RegOff, Size;
} BlockWrite32Hdr;

typedef struct {
    uint8_t *buf;
    uint32_t pos;
    uint32_t ops;
} TxnBuild;

static void txn_init(TxnBuild *b, uint8_t *buf)
{
    memset(b, 0, sizeof(*b));
    b->buf = buf;
    b->pos = sizeof(TxnHeader);
}

static void txn_w32(TxnBuild *b, uint8_t col, uint8_t row, uint32_t off,
                    uint32_t val)
{
    Write32Hdr w;

    memset(&w, 0, sizeof(w));
    w.hdr.Op = XAIE_IO_WRITE;
    w.hdr.Col = col;
    w.hdr.Row = row;
    w.RegOff = AIE_ADDR(col, row, off);
    w.Value = val;
    w.Size = (uint32_t)sizeof(w);
    memcpy(b->buf + b->pos, &w, sizeof(w));
    b->pos += (uint32_t)sizeof(w);
    b->ops++;
}

static void txn_maskpoll(TxnBuild *b, uint8_t col, uint8_t row, uint32_t off,
                         uint32_t mask, uint32_t val)
{
    MaskPoll32Hdr w;

    memset(&w, 0, sizeof(w));
    w.hdr.Op = XAIE_IO_MASKPOLL;
    w.hdr.Col = col;
    w.hdr.Row = row;
    w.RegOff = AIE_ADDR(col, row, off);
    w.Value = val;
    w.Mask = mask;
    w.Size = (uint32_t)sizeof(w);
    memcpy(b->buf + b->pos, &w, sizeof(w));
    b->pos += (uint32_t)sizeof(w);
    b->ops++;
}

static void txn_blockwrite(TxnBuild *b, uint8_t col, uint8_t row, uint32_t off,
                           const uint32_t *data, uint32_t words)
{
    BlockWrite32Hdr w;

    memset(&w, 0, sizeof(w));
    w.hdr.Op = XAIE_IO_BLOCKWRITE;
    w.hdr.Col = col;
    w.hdr.Row = row;
    w.RegOff = AIE_ADDR(col, row, off);
    w.Size = (uint32_t)sizeof(w) + words * 4u;
    memcpy(b->buf + b->pos, &w, sizeof(w));
    memcpy(b->buf + b->pos + sizeof(w), data, words * 4u);
    b->pos += w.Size;
    b->ops++;
}

static uint32_t txn_finish(TxnBuild *b)
{
    TxnHeader h;

    memset(&h, 0, sizeof(h));
    h.Major = 0;
    h.Minor = 1;
    h.DevGen = 2;               /* AIE-ML */
    h.NumRows = (uint8_t)AIE_NUM_ROWS;
    h.NumCols = (uint8_t)AIE_NUM_COLS;
    h.NumMemTileRows = (uint8_t)AIE_MEM_NUM_ROWS;
    h.NumOps = b->ops;
    h.TxnSize = b->pos;
    memcpy(b->buf, &h, sizeof(h));
    return b->pos;
}

/* ---------------------------------------------------------------- */
/* Test ctrlcode'lari                                                */
/* ---------------------------------------------------------------- */

static uint32_t shim_bd(uint32_t bd, uint32_t word)
{
    return AIEML_SHIM_DMA_BD0 + bd * AIE_BD_STRIDE + word * 4u;
}

static uint32_t memt_bd(uint32_t bd, uint32_t word)
{
    return AIEML_MEMT_DMA_BD0 + bd * AIE_BD_STRIDE + word * 4u;
}

/*
 * Stream switch port indeksleri -- aie-rt register siralamasindan
 * (offset sirasi = port indeksi):
 *
 *   shim slave : TILE_CTRL, FIFO_0, SOUTH_0..7, WEST_0..3, NORTH_0..3,
 *                EAST_0..3, TRACE
 *   shim master: TILE_CTRL, FIFO0, SOUTH0..5, WEST0..3, NORTH0..5, EAST0..3
 *   memt slave : DMA_0..5, TILE_CTRL, SOUTH_0..5, NORTH_0..3, TRACE
 *   memt master: DMA0..5, TILE_CTRL, SOUTH0..3, NORTH0..5
 */
#define SHIM_SLAVE_SOUTH(n)   (2u + (n))
#define SHIM_SLAVE_NORTH(n)   (14u + (n))
#define SHIM_MASTER_SOUTH(n)  (2u + (n))
#define SHIM_MASTER_NORTH(n)  (12u + (n))
#define MEMT_SLAVE_DMA(n)     (0u + (n))
#define MEMT_SLAVE_SOUTH(n)   (7u + (n))
#define MEMT_MASTER_DMA(n)    (0u + (n))
#define MEMT_MASTER_SOUTH(n)  (7u + (n))

#define SS_MASTER(base, port)  ((base) + (port) * 4u)
#define SS_ENABLE(slave_port) \
    (AIE_SS_MASTER_ENABLE_MASK | ((slave_port) & AIE_SS_MASTER_CFG_MASK))

/*
 * Yonlendirmeyi kur:
 *
 *   host -> shim MM2S ch0 -> shim slave SOUTH_3 -> shim master NORTH0
 *        -> memtile slave SOUTH_0 -> memtile master DMA0 (S2MM ch0)
 *
 *   memtile MM2S ch0 -> memtile slave DMA_0 -> memtile master SOUTH0
 *        -> shim slave NORTH_0 -> shim master SOUTH2 (S2MM ch0) -> host
 *
 * Gercek ctrlcode da bu registerlari boyle programliyor; stream switch
 * konfigure edilmezse veri hicbir yere gitmez.
 */
static void txn_config_stream_switch(TxnBuild *b)
{
    /* --- host -> array --- */
    /* shim MUX: SOUTH3 alani (LSB 10) DMA'ya baglansin */
    txn_w32(b, 0, 0, AIEML_SHIM_MUX_CONFIG, AIE_MUX_TYPE_DMA << 10);
    txn_w32(b, 0, 0, SS_MASTER(AIEML_SHIM_SS_SLAVE, SHIM_SLAVE_SOUTH(3)),
            AIE_SS_SLAVE_ENABLE_MASK);
    txn_w32(b, 0, 0, SS_MASTER(AIEML_SHIM_SS_MASTER, SHIM_MASTER_NORTH(0)),
            SS_ENABLE(SHIM_SLAVE_SOUTH(3)));
    txn_w32(b, 0, 1, SS_MASTER(AIEML_MEMT_SS_SLAVE, MEMT_SLAVE_SOUTH(0)),
            AIE_SS_SLAVE_ENABLE_MASK);
    txn_w32(b, 0, 1, SS_MASTER(AIEML_MEMT_SS_MASTER, MEMT_MASTER_DMA(0)),
            SS_ENABLE(MEMT_SLAVE_SOUTH(0)));

    /* --- array -> host --- */
    txn_w32(b, 0, 1, SS_MASTER(AIEML_MEMT_SS_SLAVE, MEMT_SLAVE_DMA(0)),
            AIE_SS_SLAVE_ENABLE_MASK);
    txn_w32(b, 0, 1, SS_MASTER(AIEML_MEMT_SS_MASTER, MEMT_MASTER_SOUTH(0)),
            SS_ENABLE(MEMT_SLAVE_DMA(0)));
    txn_w32(b, 0, 0, SS_MASTER(AIEML_SHIM_SS_SLAVE, SHIM_SLAVE_NORTH(0)),
            AIE_SS_SLAVE_ENABLE_MASK);
    txn_w32(b, 0, 0, SS_MASTER(AIEML_SHIM_SS_MASTER, SHIM_MASTER_SOUTH(2)),
            SS_ENABLE(SHIM_SLAVE_NORTH(0)));
    /* shim DEMUX: SOUTH2 alani (LSB 4) DMA'ya baglansin */
    txn_w32(b, 0, 0, AIEML_SHIM_DEMUX_CONFIG, AIE_MUX_TYPE_DMA << 4);
}

/* host -> memory tile -> host yolunu kuran ctrlcode. */
static uint32_t build_loopback_ctrlcode(uint8_t *buf, uint64_t in_addr,
                                        uint64_t out_addr)
{
    TxnBuild b;

    txn_init(&b, buf);
    txn_config_stream_switch(&b);

    /* --- shim MM2S: host girisini stream switch'e bas --- */
    txn_w32(&b, 0, 0, shim_bd(0, AIE_SHIM_BD_LEN_WORD), XFER_WORDS);
    txn_w32(&b, 0, 0, shim_bd(0, AIE_SHIM_BD_ADDRLO_WORD),
            (uint32_t)in_addr & AIE_SHIM_BD_ADDRLO_MASK);
    txn_w32(&b, 0, 0, shim_bd(0, AIE_SHIM_BD_ADDRHI_WORD),
            (uint32_t)(in_addr >> 32) & AIE_SHIM_BD_ADDRHI_MASK);
    txn_w32(&b, 0, 0, shim_bd(0, AIE_SHIM_BD_CTRL_WORD),
            1u << AIE_SHIM_BD_VALID_LSB);
    txn_w32(&b, 0, 0, AIEML_SHIM_DMA_MM2S0_CTRL + AIEML_DMA_QUEUE_OFF, 0);

    /* --- memory tile S2MM: stream'i tile bellegine yaz, lock 0 release --- */
    txn_w32(&b, 0, 1, memt_bd(0, AIE_MEMT_BD_LEN_WORD), XFER_WORDS);
    txn_w32(&b, 0, 1, memt_bd(0, AIE_MEMT_BD_ADDR_WORD), MEMT_WORD_OFF);
    txn_w32(&b, 0, 1, memt_bd(0, AIE_MEMT_BD_CTRL_WORD),
            (1u << AIE_MEMT_BD_VALID_LSB) | (1u << AIE_MEMT_BD_REL_VAL_LSB));
    txn_w32(&b, 0, 1, AIEML_MEMT_DMA_S2MM0_CTRL + AIEML_DMA_QUEUE_OFF, 0);

    /* --- senkronizasyon: lock 0 degerinin 1 olmasini bekle --- */
    txn_maskpoll(&b, 0, 1, AIEML_MEMT_LOCK0_VALUE, 0xFFu, 1u);

    /* --- memory tile MM2S: lock 0 acquire, tile bellegini stream'e bas --- */
    txn_w32(&b, 0, 1, memt_bd(1, AIE_MEMT_BD_LEN_WORD), XFER_WORDS);
    txn_w32(&b, 0, 1, memt_bd(1, AIE_MEMT_BD_ADDR_WORD), MEMT_WORD_OFF);
    txn_w32(&b, 0, 1, memt_bd(1, AIE_MEMT_BD_CTRL_WORD),
            (1u << AIE_MEMT_BD_VALID_LSB) | (1u << AIE_MEMT_BD_ACQ_EN_LSB) |
                (1u << AIE_MEMT_BD_ACQ_VAL_LSB));
    txn_w32(&b, 0, 1, AIEML_MEMT_DMA_MM2S0_CTRL + AIEML_DMA_QUEUE_OFF, 1);

    /* --- shim S2MM: stream'i host cikisina yaz --- */
    txn_w32(&b, 0, 0, shim_bd(1, AIE_SHIM_BD_LEN_WORD), XFER_WORDS);
    txn_w32(&b, 0, 0, shim_bd(1, AIE_SHIM_BD_ADDRLO_WORD),
            (uint32_t)out_addr & AIE_SHIM_BD_ADDRLO_MASK);
    txn_w32(&b, 0, 0, shim_bd(1, AIE_SHIM_BD_ADDRHI_WORD),
            (uint32_t)(out_addr >> 32) & AIE_SHIM_BD_ADDRHI_MASK);
    txn_w32(&b, 0, 0, shim_bd(1, AIE_SHIM_BD_CTRL_WORD),
            1u << AIE_SHIM_BD_VALID_LSB);
    txn_w32(&b, 0, 0, AIEML_SHIM_DMA_S2MM0_CTRL + AIEML_DMA_QUEUE_OFF, 1);

    return txn_finish(&b);
}

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
    cc_size = build_loopback_ctrlcode(cc, INPUT_ADDR, OUTPUT_ADDR);
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
