// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compute tile yurutme cekirdegi testleri.
 *
 * Iki bolum:
 *   1. AIE2 VLIW paket uzunlugu cozucusu -- saf fonksiyon, tam kapsam.
 *   2. Core enable yolu -- program bellegine yazip core'u calistiriyor ve
 *      ERROR_HALT ile durdugunu YALNIZCA MMIO uzerinden dogruluyor
 *      (ctrlcode MASKPOLL ile).
 */

#include <stdlib.h>
#include <string.h>

#include "drv_model.h"

#define CTRLCODE_ADDR (HOST_MEM_BASE + 0x300000)

/* ---------------------------------------------------------------- */
/* 1. Paket uzunlugu cozucusu                                        */
/* ---------------------------------------------------------------- */

/*
 * Beklenen tablo -- Xilinx/llvm-aie AIE2CompositeFormats.td:
 *   bit0 == 0            -> 16 bayt (instr128)
 *   dusuk 3 bit == 0b101 ->  6 bayt (instr48)
 *   dusuk 4 bit:
 *     0b0001 ->  2   0b0011 ->  8   0b0111 -> 12
 *     0b1001 ->  4   0b1011 -> 10   0b1111 -> 14
 */
static uint32_t expected_size(uint32_t w)
{
    if ((w & 1u) == 0u) {
        return 16u;
    }
    if ((w & 7u) == 5u) {
        return 6u;
    }
    switch (w & 0xFu) {
    case 0x1u: return 2u;
    case 0x3u: return 8u;
    case 0x7u: return 12u;
    case 0x9u: return 4u;
    case 0xBu: return 10u;
    case 0xFu: return 14u;
    default:   return 0u;
    }
}

static void test_packet_sizes(void)
{
    uint32_t n;
    unsigned covered = 0;

    step("Paket uzunlugu: dusuk 5 bitin TUM desenleri");
    for (n = 0; n < 32u; n++) {
        uint32_t got = xdna_aie2_packet_size(n);
        uint32_t want = expected_size(n);

        CHECK(got == want,
              "dusuk bitler 0x%02x: %u beklendi, %u bulundu", n, want, got);
        if (got) {
            covered++;
        }
    }
    CHECK_EQ(covered, 32u, "her desen bir boyuta cozulmeli (bosluk yok)");

    step("Paket uzunlugu: ust bitler sonucu etkilememeli");
    for (n = 0; n < 32u; n++) {
        uint32_t hi = 0xDEADBE00u | n;

        CHECK(xdna_aie2_packet_size(hi) == xdna_aie2_packet_size(n),
              "ust bitler 0x%08x sonucu degistirdi", hi);
    }

    step("Paket uzunlugu: bilinen ornekler");
    CHECK_EQ(xdna_aie2_packet_size(0x00000000u), 16u, "instr128");
    CHECK_EQ(xdna_aie2_packet_size(0x00000005u), 6u, "instr48");
    CHECK_EQ(xdna_aie2_packet_size(0x0000000Du), 6u, "instr48 (0b1101)");
    CHECK_EQ(xdna_aie2_packet_size(0x00000001u), 2u, "instr16");
    CHECK_EQ(xdna_aie2_packet_size(0x00000009u), 4u, "instr32");
    CHECK_EQ(xdna_aie2_packet_size(0x00000003u), 8u, "instr64");
    CHECK_EQ(xdna_aie2_packet_size(0x0000000Bu), 10u, "instr80");
    CHECK_EQ(xdna_aie2_packet_size(0x00000007u), 12u, "instr96");
    CHECK_EQ(xdna_aie2_packet_size(0x0000000Fu), 14u, "instr112");
}

/* ---------------------------------------------------------------- */
/* 2. Core enable yolu                                               */
/* ---------------------------------------------------------------- */

typedef struct {
    uint8_t Major, Minor, DevGen, NumRows, NumCols, NumMemTileRows;
    uint32_t NumOps, TxnSize;
} TxnHeader;

typedef struct { uint8_t Op, Col, Row; } OpHdr;

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
    h.Minor = 1;
    h.DevGen = 2;
    h.NumRows = (uint8_t)AIE_NUM_ROWS;
    h.NumCols = (uint8_t)AIE_NUM_COLS;
    h.NumMemTileRows = (uint8_t)AIE_MEM_NUM_ROWS;
    h.NumOps = b->ops;
    h.TxnSize = b->pos;
    memcpy(b->buf, &h, sizeof(h));
    return b->pos;
}

static int exec_ctrlcode(Drv *d, uint64_t addr, uint32_t size)
{
    ExecDpuReq req;
    StatusResp st = { .status = 0xFFFFFFFFu };

    memset(&req, 0, sizeof(req));
    req.inst_buf_addr = addr;
    req.inst_size = size;
    if (mbox_send_recv(d, MSG_OP_EXEC_DPU, &req, sizeof(req), &st,
                       sizeof(st)) != 0) {
        return -1;
    }
    return (int)st.status;
}

static void test_core_enable(Host *host, XdnaNpu *npu)
{
    Drv mgmt = { .npu = npu, .host = host, .next_id = 1 };
    Drv ctxd;
    CreateCtxResp cctx;
    uint8_t *cc = host_ptr(host, CTRLCODE_ADDR);
    TxnBuild b;
    uint32_t size;

    step("Boot ve context");
    CHECK(drv_hw_start(&mgmt) == 0, "hw_start");
    if (g_failures) {
        return;
    }
    {
        CreateCtxReq req = {
            .aie_type = 1, .start_col = 0, .num_col = 1,
            .num_cq_pairs_requested = 1, .context_priority = 3,
        };
        CHECK(mbox_send_recv(&mgmt, MSG_OP_CREATE_CONTEXT, &req, sizeof(req),
                             &cctx, sizeof(cctx)) == 0, "create context");
        CHECK_EQ(cctx.status, 0, "create context durumu");
        if (cctx.status) {
            return;
        }
    }
    drv_attach_ctx_channel(&cctx, &ctxd, &mgmt);

    step("Program yaz, core'u calistir, ERROR_HALT'i MMIO ile dogrula");
    {
        /*
         * Bilerek instr32 (0b1001 = 4 bayt) desenli bir paket koyuyoruz;
         * emulator paketi 4 bayt olarak sinirlamali ve slot'lari
         * calistiramadigi icin ERROR_HALT vermeli.
         */
        uint32_t prog[4] = { 0x12345609u, 0u, 0u, 0u };

        txn_init(&b, cc);
        txn_blockwrite(&b, 0, 2, AIEML_CORE_PROG_MEM, prog, 4);
        /* Once reset: durum ve PC temizlensin. */
        txn_w32(&b, 0, 2, AIEML_CORE_CONTROL, AIE_CORE_CTRL_RESET_MASK);
        txn_maskpoll(&b, 0, 2, AIEML_CORE_STATUS, AIE_CORE_STAT_ERROR_HALT, 0);
        /* Calistir. */
        txn_w32(&b, 0, 2, AIEML_CORE_CONTROL, AIE_CORE_CTRL_ENABLE_MASK);
        /* ERROR_HALT set olmali; olmazsa MASKPOLL zaman asimina ugrar. */
        txn_maskpoll(&b, 0, 2, AIEML_CORE_STATUS, AIE_CORE_STAT_ERROR_HALT,
                     AIE_CORE_STAT_ERROR_HALT);
        /* PC hala 0 olmali: hicbir instruction yurutulmedi. */
        txn_maskpoll(&b, 0, 2, AIEML_CORE_PC, 0xFFFFFFFFu, 0u);
        size = txn_finish(&b);

        CHECK_EQ(exec_ctrlcode(&ctxd, CTRLCODE_ADDR, size), 0,
                 "core enable ctrlcode durumu");
    }

    step("Paket getirme sayaci ilerlemeli, core halt kaydedilmeli");
    {
        XdnaStats stats;

        xdna_npu_get_stats(npu, &stats);
        CHECK(stats.core_fetches >= 1, "core paket getirme sayaci (%llu)",
              (unsigned long long)stats.core_fetches);
        CHECK(stats.core_halts >= 1, "core halt sayaci (%llu)",
              (unsigned long long)stats.core_halts);
    }

    step("Program bellegi disina PC: hata halt");
    {
        uint8_t *cc2 = host_ptr(host, CTRLCODE_ADDR + 0x10000);
        uint32_t size2;

        txn_init(&b, cc2);
        txn_w32(&b, 0, 3, AIEML_CORE_CONTROL, AIE_CORE_CTRL_RESET_MASK);
        /* PC'yi program bellegi sinirinin disina koy. */
        txn_w32(&b, 0, 3, AIEML_CORE_PC, AIE_CORE_PROG_MEM_SIZE);
        txn_w32(&b, 0, 3, AIEML_CORE_CONTROL, AIE_CORE_CTRL_ENABLE_MASK);
        txn_maskpoll(&b, 0, 3, AIEML_CORE_STATUS, AIE_CORE_STAT_ERROR_HALT,
                     AIE_CORE_STAT_ERROR_HALT);
        size2 = txn_finish(&b);

        CHECK_EQ(exec_ctrlcode(&ctxd, CTRLCODE_ADDR + 0x10000, size2), 0,
                 "sinir disi PC ctrlcode durumu");
    }
}

int main(int argc, char **argv)
{
    Host *host;
    XdnaNpu *npu;

    printf("=== XDNA1 emulatoru: compute tile yurutme cekirdegi ===\n");

    test_packet_sizes();

    host = host_new(argc > 1 && strcmp(argv[1], "-v") == 0);
    if (!host) {
        return 1;
    }
    npu = xdna_npu_new(&drv_host_ops, host);
    if (!npu) {
        host_free(host);
        return 1;
    }

    test_core_enable(host, npu);

    xdna_npu_free(npu);
    host_free(host);

    printf("\n%d kontrol, %d basarisiz\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
