/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * ctrlcode uretici -- aie-rt transaction serilestirme bicimi.
 *
 * Gercek akista bu isi aiecompiler + XRT yapiyor: workload'un array
 * konfigurasyonu `XAie_TxnOpcode` islemlerinden olusan bir ikili bloba
 * serilestiriliyor ve firmware'e gonderiliyor. Burada ayni bicimde
 * ctrlcode uretiyoruz.
 *
 * Iki yerden kullaniliyor:
 *   tests/test_exec.c        -- emulatoru dogrudan surerek
 *   qemu/guest-test/init.c   -- gercek guest'te stock surucu uzerinden
 *
 * Ayni uretici olmasi onemli: guest'te kosan ctrlcode, testte dogrulanan
 * ctrlcode'un ta kendisi.
 *
 * Register offsetleri ve alan yerlesimleri `xdna/xdna_aie.h`den geliyor;
 * onlar da `aie-rt` kaynagindan dogrulandi.
 */

#ifndef XDNA_TEST_CTRLCODE_H
#define XDNA_TEST_CTRLCODE_H

#include <stdint.h>
#include <string.h>

#include "xdna/xdna_aie.h"

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

static inline void txn_init(TxnBuild *b, uint8_t *buf)
{
    memset(b, 0, sizeof(*b));
    b->buf = buf;
    b->pos = sizeof(TxnHeader);
}

static inline void txn_w32(TxnBuild *b, uint8_t col, uint8_t row, uint32_t off,
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

static inline void txn_maskpoll(TxnBuild *b, uint8_t col, uint8_t row,
                                uint32_t off, uint32_t mask, uint32_t val)
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

static inline void txn_blockwrite(TxnBuild *b, uint8_t col, uint8_t row,
                                  uint32_t off, const uint32_t *data,
                                  uint32_t words)
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

/* Sabit boyutlu op'lar: yalnizca opcode + dolgu, Size alani YOK. */
static inline void txn_fixed_op(TxnBuild *b, uint8_t opcode,
                                uint32_t total_size)
{
    memset(b->buf + b->pos, 0, total_size);
    b->buf[b->pos] = opcode;
    b->pos += total_size;
    b->ops++;
}

/* TCT custom op: CustomOpHdr + {word, config} */
static inline void txn_tct(TxnBuild *b, uint8_t col, uint8_t row, uint8_t chan,
                           int mm2s)
{
    struct { OpHdr hdr; uint32_t Size; } c;
    uint32_t payload[2];

    memset(&c, 0, sizeof(c));
    c.hdr.Op = XAIE_IO_CUSTOM_OP_TCT;
    c.Size = (uint32_t)sizeof(c) + (uint32_t)sizeof(payload);
    payload[0] = ((uint32_t)col << 16) | ((uint32_t)row << 8) |
                 (mm2s ? 1u : 0u);
    payload[1] = (uint32_t)chan << 24;
    memcpy(b->buf + b->pos, &c, sizeof(c));
    memcpy(b->buf + b->pos + sizeof(c), payload, sizeof(payload));
    b->pos += c.Size;
    b->ops++;
}

static inline uint32_t txn_finish(TxnBuild *b)
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
/* Loopback ctrlcode: host -> memory tile -> host                     */
/* ---------------------------------------------------------------- */

static inline uint32_t shim_bd(uint32_t bd, uint32_t word)
{
    return AIEML_SHIM_DMA_BD0 + bd * AIE_BD_STRIDE + word * 4u;
}

static inline uint32_t memt_bd(uint32_t bd, uint32_t word)
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

/* Memory tile icinde kullanilan bayt offseti (kelime cinsinden). */
#define CTRLCODE_MEMT_WORD_OFF 0x100u

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
static inline void txn_config_stream_switch(TxnBuild *b)
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

/*
 * host -> memory tile -> host yolunu kuran ctrlcode.
 *
 * `in_addr` / `out_addr` shim BD'lerine yazilan adresler. Gercek akista
 * bunlari XRT yamiyor (DDR_PATCH) ve argüman BO'sunun cihaz adresini
 * koyuyor; emulator cihaz bellegi penceresindeki adresleri context
 * heap'ine ceviriyor.
 */
static inline uint32_t build_loopback_ctrlcode(uint8_t *buf, uint64_t in_addr,
                                               uint64_t out_addr,
                                               uint32_t words)
{
    TxnBuild b;

    txn_init(&b, buf);
    txn_config_stream_switch(&b);

    /* --- shim MM2S: host girisini stream switch'e bas --- */
    txn_w32(&b, 0, 0, shim_bd(0, AIE_SHIM_BD_LEN_WORD), words);
    txn_w32(&b, 0, 0, shim_bd(0, AIE_SHIM_BD_ADDRLO_WORD),
            (uint32_t)in_addr & AIE_SHIM_BD_ADDRLO_MASK);
    txn_w32(&b, 0, 0, shim_bd(0, AIE_SHIM_BD_ADDRHI_WORD),
            (uint32_t)(in_addr >> 32) & AIE_SHIM_BD_ADDRHI_MASK);
    txn_w32(&b, 0, 0, shim_bd(0, AIE_SHIM_BD_CTRL_WORD),
            1u << AIE_SHIM_BD_VALID_LSB);
    txn_w32(&b, 0, 0, AIEML_SHIM_DMA_MM2S0_CTRL + AIEML_DMA_QUEUE_OFF, 0);

    /* --- memory tile S2MM: stream'i tile bellegine yaz, lock 0 release --- */
    txn_w32(&b, 0, 1, memt_bd(0, AIE_MEMT_BD_LEN_WORD), words);
    txn_w32(&b, 0, 1, memt_bd(0, AIE_MEMT_BD_ADDR_WORD),
            CTRLCODE_MEMT_WORD_OFF);
    txn_w32(&b, 0, 1, memt_bd(0, AIE_MEMT_BD_CTRL_WORD),
            (1u << AIE_MEMT_BD_VALID_LSB) | (1u << AIE_MEMT_BD_REL_VAL_LSB));
    txn_w32(&b, 0, 1, AIEML_MEMT_DMA_S2MM0_CTRL + AIEML_DMA_QUEUE_OFF, 0);

    /* --- senkronizasyon: lock 0 degerinin 1 olmasini bekle --- */
    txn_maskpoll(&b, 0, 1, AIEML_MEMT_LOCK0_VALUE, 0xFFu, 1u);

    /* --- memory tile MM2S: lock 0 acquire, tile bellegini stream'e bas --- */
    txn_w32(&b, 0, 1, memt_bd(1, AIE_MEMT_BD_LEN_WORD), words);
    txn_w32(&b, 0, 1, memt_bd(1, AIE_MEMT_BD_ADDR_WORD),
            CTRLCODE_MEMT_WORD_OFF);
    txn_w32(&b, 0, 1, memt_bd(1, AIE_MEMT_BD_CTRL_WORD),
            (1u << AIE_MEMT_BD_VALID_LSB) | (1u << AIE_MEMT_BD_ACQ_EN_LSB) |
                (1u << AIE_MEMT_BD_ACQ_VAL_LSB));
    txn_w32(&b, 0, 1, AIEML_MEMT_DMA_MM2S0_CTRL + AIEML_DMA_QUEUE_OFF, 1);

    /* --- shim S2MM: stream'i host cikisina yaz --- */
    txn_w32(&b, 0, 0, shim_bd(1, AIE_SHIM_BD_LEN_WORD), words);
    txn_w32(&b, 0, 0, shim_bd(1, AIE_SHIM_BD_ADDRLO_WORD),
            (uint32_t)out_addr & AIE_SHIM_BD_ADDRLO_MASK);
    txn_w32(&b, 0, 0, shim_bd(1, AIE_SHIM_BD_ADDRHI_WORD),
            (uint32_t)(out_addr >> 32) & AIE_SHIM_BD_ADDRHI_MASK);
    txn_w32(&b, 0, 0, shim_bd(1, AIE_SHIM_BD_CTRL_WORD),
            1u << AIE_SHIM_BD_VALID_LSB);
    txn_w32(&b, 0, 0, AIEML_SHIM_DMA_S2MM0_CTRL + AIEML_DMA_QUEUE_OFF, 1);

    return txn_finish(&b);
}

#endif /* XDNA_TEST_CTRLCODE_H */
