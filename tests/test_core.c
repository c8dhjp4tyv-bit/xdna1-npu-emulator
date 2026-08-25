// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compute tile yurutme cekirdegi testleri.
 *
 * Uc bolum:
 *   1. AIE2 VLIW paket uzunlugu cozucusu -- saf fonksiyon, tam kapsam.
 *   2. AIE2 bundle slot cozucusu -- llvm-aie ile uretilmis differential
 *      vektorlere karsi.
 *   3. Core enable yolu -- program bellegine yazip core'u calistiriyor ve
 *      ERROR_HALT ile durdugunu YALNIZCA MMIO uzerinden dogruluyor
 *      (ctrlcode MASKPOLL ile).
 */

#include <stdlib.h>
#include <string.h>

#include "drv_model.h"
#include "aie2_vectors.h"
#include "aie2_slot_vectors.h"
#include "ctrlcode.h"

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

    step("Paket uzunlugu: llvm-aie disassembler'i ile uretilmis altin vektorler");
    {
        size_t i;

        CHECK_EQ(AIE2_VECTOR_COUNT, 16u, "altin vektor sayisi");
        for (i = 0; i < AIE2_VECTOR_COUNT; i++) {
            const Aie2Vector *v = &aie2_vectors[i];
            uint32_t word = 0;
            uint32_t n = v->size < 4u ? v->size : 4u;
            uint32_t got;

            memcpy(&word, v->bytes, n);
            got = xdna_aie2_packet_size(word);
            CHECK(got == v->size,
                  "vektor %u (%s): llvm %u bayt dedi, biz %u bulduk",
                  (unsigned)i, v->disasm, v->size, got);
        }
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
/* 2. Bundle slot cozucusu                                           */
/* ---------------------------------------------------------------- */

/*
 * Vektorler tools/gen-aie2-formats.py --vectors ile uretildi. Testin
 * BAGIMSIZ oldugu iki nokta:
 *
 *   llvm_slots : llvm-aie disassembler'inin ";" ile ayirarak yazdigi slot
 *                sayisi. Bizim tablomuz farkli sayida slot bulursa
 *                yerlesim yanlis demektir.
 *   flips      : llvm ciktisinda yalnizca N. slotu degistiren bir bit.
 *                Bizim cozumumuzde de yalnizca N. slot degismeli.
 */
static void test_slot_decode(void)
{
    size_t i;

    step("Bundle cozucusu: her composite format cozulmeli");
    CHECK_EQ(AIE2_SLOT_VECTOR_COUNT, 78u, "differential vektor sayisi");
    for (i = 0; i < AIE2_SLOT_VECTOR_COUNT; i++) {
        const Aie2SlotVector *v = &aie2_slot_vectors[i];
        Aie2Bundle b;

        CHECK(xdna_aie2_decode(v->bytes, v->size, &b) == 0,
              "vektor %u (%s) cozulemedi", (unsigned)i, v->format);
        if (b.format == NULL) {
            continue;
        }
        CHECK(strcmp(b.format, v->format) == 0,
              "vektor %u: format %s beklendi, %s bulundu", (unsigned)i,
              v->format, b.format);
        CHECK(b.size == v->size, "vektor %u (%s): boyut %u beklendi, %u bulundu",
              (unsigned)i, v->format, v->size, b.size);
        /* Asil bagimsiz kontrol: slot sayisi llvm ile ayni mi? */
        CHECK(b.nslots == v->llvm_slots,
              "vektor %u (%s): llvm %u slot yazdi, biz %u bulduk (%s)",
              (unsigned)i, v->format, v->llvm_slots, b.nslots, v->disasm);
    }

    step("Bundle cozucusu: bit-flip esleme llvm ile ayni slotu gostermeli");
    {
        unsigned flips = 0;

        for (i = 0; i < AIE2_SLOT_VECTOR_COUNT; i++) {
            const Aie2SlotVector *v = &aie2_slot_vectors[i];
            Aie2Bundle base;
            unsigned f;

            if (xdna_aie2_decode(v->bytes, v->size, &base) != 0) {
                continue;
            }
            for (f = 0; f < v->nflips; f++) {
                uint8_t buf[16];
                Aie2Bundle mod;
                unsigned bit = v->flips[f].bit;
                unsigned want = v->flips[f].slot;
                unsigned s, diff = 0, which = 0;

                memcpy(buf, v->bytes, v->size);
                buf[bit >> 3] ^= (uint8_t)(1u << (bit & 7u));

                CHECK(xdna_aie2_decode(buf, v->size, &mod) == 0,
                      "vektor %u (%s): bit %u cevrilince cozulemedi",
                      (unsigned)i, v->format, bit);
                if (mod.format == NULL || mod.nslots != base.nslots) {
                    continue;
                }
                for (s = 0; s < base.nslots; s++) {
                    if (mod.slot[s].value != base.slot[s].value) {
                        diff++;
                        which = s;
                    }
                }
                CHECK(diff == 1,
                      "vektor %u (%s): bit %u tam bir slotu degistirmeli, "
                      "%u slot degisti", (unsigned)i, v->format, bit, diff);
                if (diff == 1) {
                    CHECK(which == want,
                          "vektor %u (%s): bit %u llvm'e gore slot %u, "
                          "bize gore slot %u", (unsigned)i, v->format, bit,
                          want, which);
                }
                flips++;
            }
        }
        CHECK(flips >= 220, "yeterli bit-flip kontrolu (%u)", flips);
    }

    step("Bundle cozucusu: gecersiz kodlama reddedilmeli");
    {
        /*
         * 0x20000019: instr32 etiketi (0b1001) tasiyor ama hicbir instr32
         * formatinin sabit bitlerine uymuyor. llvm-aie de bu deseni
         * "invalid instruction encoding" diye reddediyor.
         */
        static const uint8_t bad[4] = { 0x19, 0x00, 0x00, 0x20 };
        static const uint8_t good[4] = { 0x19, 0x00, 0x00, 0x00 };
        Aie2Bundle b;

        CHECK(xdna_aie2_decode(bad, sizeof(bad), &b) != 0,
              "eslesmeyen sabit bitler kabul edilmemeli");
        CHECK(xdna_aie2_decode(good, sizeof(good), &b) == 0,
              "gecerli instr32 kodlamasi cozulmeli");
        CHECK(xdna_aie2_decode(good, 2, &b) != 0,
              "tampon yetmiyorsa cozulmemeli");
        CHECK(xdna_aie2_decode(good, 0, &b) != 0, "bos tampon");
    }

    step("Bundle cozucusu: instr128 alan genislikleri");
    {
        /* Tumu sifir: instr128 ldb/lda/st/lng/vec formati. */
        static const uint8_t z[16] = { 0 };
        Aie2Bundle b;
        static const struct { Aie2SlotKind kind; uint8_t width; } want[] = {
            { AIE2_SLOT_LDB, 16 }, { AIE2_SLOT_LDA, 21 },
            { AIE2_SLOT_ST,  21 }, { AIE2_SLOT_LNG, 42 },
            { AIE2_SLOT_VEC, 26 },
        };
        unsigned s;

        CHECK(xdna_aie2_decode(z, sizeof(z), &b) == 0, "sifir paket cozulmeli");
        CHECK_EQ(b.nslots, 5u, "instr128 lng formatinda 5 slot");
        for (s = 0; s < b.nslots && s < 5u; s++) {
            CHECK(b.slot[s].kind == want[s].kind,
                  "slot %u: %s beklendi, %s bulundu", s,
                  xdna_aie2_slot_name(want[s].kind),
                  xdna_aie2_slot_name(b.slot[s].kind));
            CHECK(b.slot[s].width == want[s].width,
                  "slot %u (%s): %u bit beklendi, %u bulundu", s,
                  xdna_aie2_slot_name(want[s].kind), want[s].width,
                  b.slot[s].width);
        }
    }

    step("Bundle cozucusu: uzunluk vektorleri de cozulebilmeli");
    for (i = 0; i < AIE2_VECTOR_COUNT; i++) {
        const Aie2Vector *v = &aie2_vectors[i];
        Aie2Bundle b;

        CHECK(xdna_aie2_decode(v->bytes, v->size, &b) == 0,
              "uzunluk vektoru %u (%s) cozulemedi", (unsigned)i, v->disasm);
        if (b.format) {
            CHECK(b.size == v->size, "uzunluk vektoru %u: boyut", (unsigned)i);
        }
    }
}

/* ---------------------------------------------------------------- */
/* 3. Core enable yolu                                               */
/* ---------------------------------------------------------------- */

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
    test_slot_decode();

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
