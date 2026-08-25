// SPDX-License-Identifier: GPL-2.0-only
//
// ctrlcode altin vektor ureticisi -- DERLEYICININ KENDI ENCODER'I ile.
//
// Kaynak: Xilinx/mlir-aie  include/aie/Runtime/TxnEncoding.h
//
// O baslik, mlir-aie'nin kendi ifadesiyle "the single in-tree source of truth
// for the instruction format, used by both the compiler (AIETargetNPU.cpp) and
// generated host code" ve MLIR/LLVM bagimliligi YOK. Yani derleyicinin gercek
// workload'lar icin urettigi ctrlcode'un bit-birebir ayni ureticisi.
//
// Bu program o encoder'i kullanarak bizim loopback ctrlcode'umuzu uretiyor ve
// C testine altin vektor olarak veriyor. Boylece ctrlcode yorumlayicimiz
// "okudugum baslikta boyle yaziyordu"ya degil, DERLEYICI CIKTISINA karsi
// dogrulanmis oluyor.
//
// Onemli fark: derleyici op basligindaki Col/Row alanlarini SIFIR birakiyor ve
// tile'i mutlak adrese katiyor (write32'de w1 rezerve, blockwrite'da col/row
// w1'de ama adres yine mutlak). Dolayisiyla yorumlayici tile'i ADRESTEN
// cozmek zorunda.
//
// Derleme:
//   g++ -std=c++17 -I<mlir-aie>/include -Iinclude
//       -o gen-txn-vectors tools/gen-txn-vectors.cpp
//   ./gen-txn-vectors > tests/txn_vectors.h

#include <cstdint>
#include <cstdio>
#include <vector>

#include "aie/Runtime/TxnEncoding.h"

extern "C" {
#include "xdna/xdna_aie.h"
}

using namespace aie_runtime;

/* tests/drv_model.h ile ayni olmali; test tarafinda kontrol ediliyor. */
static const uint64_t kHostBase = 0x100000000ULL;
static const uint64_t kInputAddr = kHostBase + 0x400000;
static const uint64_t kOutputAddr = kHostBase + 0x410000;

static const uint32_t kWords = 64;               /* 256 bayt */
static const uint32_t kMemtWordOff = 0x100;      /* memory tile ic offseti */

static uint32_t addr(uint32_t col, uint32_t row, uint32_t off)
{
    return AIE_ADDR(col, row, off);
}

static uint32_t shimBd(uint32_t bd, uint32_t word)
{
    return AIEML_SHIM_DMA_BD0 + bd * AIE_BD_STRIDE + word * 4u;
}

static uint32_t memtBd(uint32_t bd, uint32_t word)
{
    return AIEML_MEMT_DMA_BD0 + bd * AIE_BD_STRIDE + word * 4u;
}

/* tests/ctrlcode.h ile ayni port indeksleri. */
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

static void w32(std::vector<uint32_t> &txn, uint32_t &n, uint32_t a, uint32_t v)
{
    txn_append_write32(txn, a, v);
    n++;
}

/*
 * Loopback ctrlcode: host -> shim MM2S -> memory tile -> shim S2MM -> host.
 * tests/ctrlcode.h'deki ile AYNI register dizisi; tek fark, bu sefer
 * op'lari derleyicinin encoder'i seriliyor.
 *
 * MASKPOLL yok: TxnEncoding.h onu uretmiyor (derleyici lock beklemesini
 * TCT/senkron ile hallediyor). Emulatorde DMA'lar senkron tamamlandigi icin
 * memory tile MM2S'i lock zaten serbest bulmus oluyor.
 */
static std::vector<uint32_t> buildLoopback(uint32_t &opCount)
{
    std::vector<uint32_t> txn;
    uint32_t n = 0;

    txn_init(txn);

    /* --- host -> array yonlendirmesi --- */
    w32(txn, n, addr(0, 0, AIEML_SHIM_MUX_CONFIG), AIE_MUX_TYPE_DMA << 10);
    w32(txn, n, addr(0, 0, SS_MASTER(AIEML_SHIM_SS_SLAVE, SHIM_SLAVE_SOUTH(3))),
        AIE_SS_SLAVE_ENABLE_MASK);
    w32(txn, n,
        addr(0, 0, SS_MASTER(AIEML_SHIM_SS_MASTER, SHIM_MASTER_NORTH(0))),
        SS_ENABLE(SHIM_SLAVE_SOUTH(3)));
    w32(txn, n, addr(0, 1, SS_MASTER(AIEML_MEMT_SS_SLAVE, MEMT_SLAVE_SOUTH(0))),
        AIE_SS_SLAVE_ENABLE_MASK);
    w32(txn, n, addr(0, 1, SS_MASTER(AIEML_MEMT_SS_MASTER, MEMT_MASTER_DMA(0))),
        SS_ENABLE(MEMT_SLAVE_SOUTH(0)));

    /* --- array -> host yonlendirmesi --- */
    w32(txn, n, addr(0, 1, SS_MASTER(AIEML_MEMT_SS_SLAVE, MEMT_SLAVE_DMA(0))),
        AIE_SS_SLAVE_ENABLE_MASK);
    w32(txn, n,
        addr(0, 1, SS_MASTER(AIEML_MEMT_SS_MASTER, MEMT_MASTER_SOUTH(0))),
        SS_ENABLE(MEMT_SLAVE_DMA(0)));
    w32(txn, n, addr(0, 0, SS_MASTER(AIEML_SHIM_SS_SLAVE, SHIM_SLAVE_NORTH(0))),
        AIE_SS_SLAVE_ENABLE_MASK);
    w32(txn, n,
        addr(0, 0, SS_MASTER(AIEML_SHIM_SS_MASTER, SHIM_MASTER_SOUTH(2))),
        SS_ENABLE(SHIM_SLAVE_NORTH(0)));
    w32(txn, n, addr(0, 0, AIEML_SHIM_DEMUX_CONFIG), AIE_MUX_TYPE_DMA << 4);

    /*
     * shim MM2S BD 0 -- adres kelimelerini BLOCKWRITE ile yaziyoruz.
     * Gercek derleyici de BD'yi tek blockwrite ile basiyor; bu ayni
     * zamanda BLOCKWRITE yerlesimini de sinar.
     */
    {
        uint32_t bd[3] = {
            kWords,
            (uint32_t)(kInputAddr & AIE_SHIM_BD_ADDRLO_MASK),
            (uint32_t)((kInputAddr >> 32) & AIE_SHIM_BD_ADDRHI_MASK),
        };
        txn_append_blockwrite(txn, addr(0, 0, shimBd(0, AIE_SHIM_BD_LEN_WORD)),
                              bd, 3, 0, 0);
        n++;
    }
    w32(txn, n, addr(0, 0, shimBd(0, AIE_SHIM_BD_CTRL_WORD)),
        1u << AIE_SHIM_BD_VALID_LSB);
    w32(txn, n, addr(0, 0, AIEML_SHIM_DMA_MM2S0_CTRL + AIEML_DMA_QUEUE_OFF), 0);

    /* memory tile S2MM BD 0: lock 0 release */
    w32(txn, n, addr(0, 1, memtBd(0, AIE_MEMT_BD_LEN_WORD)), kWords);
    w32(txn, n, addr(0, 1, memtBd(0, AIE_MEMT_BD_ADDR_WORD)), kMemtWordOff);
    w32(txn, n, addr(0, 1, memtBd(0, AIE_MEMT_BD_CTRL_WORD)),
        (1u << AIE_MEMT_BD_VALID_LSB) | (1u << AIE_MEMT_BD_REL_VAL_LSB));
    w32(txn, n, addr(0, 1, AIEML_MEMT_DMA_S2MM0_CTRL + AIEML_DMA_QUEUE_OFF), 0);

    /* memory tile MM2S BD 1: lock 0 acquire -- MASKWRITE yerlesimini sinar */
    w32(txn, n, addr(0, 1, memtBd(1, AIE_MEMT_BD_LEN_WORD)), kWords);
    w32(txn, n, addr(0, 1, memtBd(1, AIE_MEMT_BD_ADDR_WORD)), kMemtWordOff);
    txn_append_maskwrite32(
        txn, addr(0, 1, memtBd(1, AIE_MEMT_BD_CTRL_WORD)),
        (1u << AIE_MEMT_BD_VALID_LSB) | (1u << AIE_MEMT_BD_ACQ_EN_LSB) |
            (1u << AIE_MEMT_BD_ACQ_VAL_LSB),
        (1u << AIE_MEMT_BD_VALID_LSB) | (1u << AIE_MEMT_BD_ACQ_EN_LSB) |
            (1u << AIE_MEMT_BD_ACQ_VAL_LSB));
    n++;
    w32(txn, n, addr(0, 1, AIEML_MEMT_DMA_MM2S0_CTRL + AIEML_DMA_QUEUE_OFF), 1);

    /* shim S2MM BD 1 -> host cikisi */
    w32(txn, n, addr(0, 0, shimBd(1, AIE_SHIM_BD_LEN_WORD)), kWords);
    w32(txn, n, addr(0, 0, shimBd(1, AIE_SHIM_BD_ADDRLO_WORD)),
        (uint32_t)(kOutputAddr & AIE_SHIM_BD_ADDRLO_MASK));
    w32(txn, n, addr(0, 0, shimBd(1, AIE_SHIM_BD_ADDRHI_WORD)),
        (uint32_t)((kOutputAddr >> 32) & AIE_SHIM_BD_ADDRHI_MASK));
    w32(txn, n, addr(0, 0, shimBd(1, AIE_SHIM_BD_CTRL_WORD)),
        1u << AIE_SHIM_BD_VALID_LSB);
    w32(txn, n, addr(0, 0, AIEML_SHIM_DMA_S2MM0_CTRL + AIEML_DMA_QUEUE_OFF), 1);

    opCount = n;
    return txn;
}

/*
 * Ikinci vektor: tile'in ADRESTEN cozuldugunu VERIYLE olcer.
 *
 * memory tile (0,1) veri bellegine bir desen blok-yazilir, sonra ayni
 * yerden DMA ile host'a cikarilir. Tile yanlis cozulurse (orn. op
 * basligindaki sifir col/row kullanilirsa) desen shim'in register
 * uzayina yazilir ve host'a hicbir sey ya da yanlis veri gider.
 */
static std::vector<uint32_t> buildBlockWriteOut(uint32_t &opCount,
                                                std::vector<uint32_t> &pattern)
{
    std::vector<uint32_t> txn;
    uint32_t n = 0;

    txn_init(txn);

    /* memory tile DMA0 -> south -> shim S2MM yonlendirmesi */
    w32(txn, n, addr(0, 1, SS_MASTER(AIEML_MEMT_SS_SLAVE, MEMT_SLAVE_DMA(0))),
        AIE_SS_SLAVE_ENABLE_MASK);
    w32(txn, n,
        addr(0, 1, SS_MASTER(AIEML_MEMT_SS_MASTER, MEMT_MASTER_SOUTH(0))),
        SS_ENABLE(MEMT_SLAVE_DMA(0)));
    w32(txn, n, addr(0, 0, SS_MASTER(AIEML_SHIM_SS_SLAVE, SHIM_SLAVE_NORTH(0))),
        AIE_SS_SLAVE_ENABLE_MASK);
    w32(txn, n,
        addr(0, 0, SS_MASTER(AIEML_SHIM_SS_MASTER, SHIM_MASTER_SOUTH(2))),
        SS_ENABLE(SHIM_SLAVE_NORTH(0)));
    w32(txn, n, addr(0, 0, AIEML_SHIM_DEMUX_CONFIG), AIE_MUX_TYPE_DMA << 4);

    /* Deseni memory tile (0,1) veri bellegine blok-yaz. */
    pattern.clear();
    for (uint32_t i = 0; i < kWords; i++) {
        pattern.push_back(0xB10C0000u + i);
    }
    txn_append_blockwrite(txn, addr(0, 1, kMemtWordOff * 4u), pattern.data(),
                          pattern.size(), 0, 1);
    n++;

    /* memory tile MM2S BD 2 -> stream (lock yok) */
    w32(txn, n, addr(0, 1, memtBd(2, AIE_MEMT_BD_LEN_WORD)), kWords);
    w32(txn, n, addr(0, 1, memtBd(2, AIE_MEMT_BD_ADDR_WORD)), kMemtWordOff);
    txn_append_maskwrite32(txn, addr(0, 1, memtBd(2, AIE_MEMT_BD_CTRL_WORD)),
                           1u << AIE_MEMT_BD_VALID_LSB,
                           1u << AIE_MEMT_BD_VALID_LSB);
    n++;
    w32(txn, n, addr(0, 1, AIEML_MEMT_DMA_MM2S0_CTRL + AIEML_DMA_QUEUE_OFF), 2);

    /* shim S2MM BD 2 -> host cikisi */
    w32(txn, n, addr(0, 0, shimBd(2, AIE_SHIM_BD_LEN_WORD)), kWords);
    w32(txn, n, addr(0, 0, shimBd(2, AIE_SHIM_BD_ADDRLO_WORD)),
        (uint32_t)(kOutputAddr & AIE_SHIM_BD_ADDRLO_MASK));
    w32(txn, n, addr(0, 0, shimBd(2, AIE_SHIM_BD_ADDRHI_WORD)),
        (uint32_t)((kOutputAddr >> 32) & AIE_SHIM_BD_ADDRHI_MASK));
    w32(txn, n, addr(0, 0, shimBd(2, AIE_SHIM_BD_CTRL_WORD)),
        1u << AIE_SHIM_BD_VALID_LSB);
    w32(txn, n, addr(0, 0, AIEML_SHIM_DMA_S2MM0_CTRL + AIEML_DMA_QUEUE_OFF), 2);

    /* TCT (sync) op'u -- derleyicinin urettigi bicimde. */
    txn_append_sync(txn, /*col=*/0, /*row=*/0, /*dir=*/0, /*chan=*/0,
                    /*ncol=*/1, /*nrow=*/1);
    n++;

    opCount = n;
    return txn;
}

static void emit(const char *name, const std::vector<uint32_t> &txn,
                 uint32_t ops)
{
    const uint8_t *b = reinterpret_cast<const uint8_t *>(txn.data());
    size_t len = txn.size() * sizeof(uint32_t);

    printf("static const uint8_t %s[] = {", name);
    for (size_t i = 0; i < len; i++) {
        printf("%s0x%02x,", (i % 12) == 0 ? "\n    " : " ", b[i]);
    }
    printf("\n};\n");
    printf("#define %s_OPS %uu\n\n", name, ops);
}

int main(void)
{
    uint32_t loopOps = 0, bwOps = 0;
    std::vector<uint32_t> pattern;
    std::vector<uint32_t> loop = buildLoopback(loopOps);
    std::vector<uint32_t> bw = buildBlockWriteOut(bwOps, pattern);

    TxnDeviceInfo info;
    info.devGen = 3;                 /* 3 = NPU (PHX/HWK) */
    info.numRows = AIE_NUM_ROWS;
    info.numCols = AIE_NUM_COLS;
    info.numMemTileRows = AIE_MEM_NUM_ROWS;
    txn_prepend_header(loop, loopOps, info);
    txn_prepend_header(bw, bwOps, info);

    printf("/* SPDX-License-Identifier: GPL-2.0-only */\n");
    printf("/*\n");
    printf(" * OTOMATIK URETILDI -- tools/gen-txn-vectors.cpp\n");
    printf(" *\n");
    printf(" * Bu ctrlcode blob'lari DERLEYICININ KENDI ENCODER'I ile\n");
    printf(" * uretildi: Xilinx/mlir-aie include/aie/Runtime/TxnEncoding.h,\n");
    printf(" * yani gercek workload'lar icin kullanilan ureticinin ta\n");
    printf(" * kendisi. Yorumlayicimiz bunlari dogru calistirabiliyorsa\n");
    printf(" * ctrlcode bicimini gercekten dogru okuyor demektir.\n");
    printf(" *\n");
    printf(" * Dikkat: derleyici op basligindaki Col/Row alanlarini SIFIR\n");
    printf(" * birakip tile'i mutlak adrese katiyor.\n");
    printf(" */\n\n");
    printf("#ifndef TXN_VECTORS_H\n#define TXN_VECTORS_H\n\n");
    printf("#include <stdint.h>\n\n");
    printf("#define TXN_VEC_HOST_BASE   0x%llxULL\n",
           (unsigned long long)kHostBase);
    printf("#define TXN_VEC_INPUT_ADDR  0x%llxULL\n",
           (unsigned long long)kInputAddr);
    printf("#define TXN_VEC_OUTPUT_ADDR 0x%llxULL\n",
           (unsigned long long)kOutputAddr);
    printf("#define TXN_VEC_WORDS       %uu\n", kWords);
    printf("#define TXN_VEC_MEMT_OFF    0x%xu\n\n", kMemtWordOff);

    emit("txn_vec_loopback", loop, loopOps);
    emit("txn_vec_blockwrite_out", bw, bwOps);

    printf("/* txn_vec_blockwrite_out cikista bu deseni uretmeli. */\n");
    printf("static const uint32_t txn_vec_pattern[] = {");
    for (size_t i = 0; i < pattern.size(); i++) {
        printf("%s0x%08xu,", (i % 6) == 0 ? "\n    " : " ", pattern[i]);
    }
    printf("\n};\n\n");

    printf("#endif /* TXN_VECTORS_H */\n");
    return 0;
}
