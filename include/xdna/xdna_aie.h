/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * AIE-ML (AIE2) array mimarisi sabitleri -- XDNA1 / Phoenix / Hawk Point.
 *
 * Degerler AMD/Xilinx `aie-rt` surucusunun acik kaynagindan dogrulanmistir:
 *   driver/src/global/xaiemlgbl_params.h   (register offsetleri, bit alanlari)
 *   driver/src/global/xaiemlgbl_reginit.c  (lock modulu parametreleri)
 *   driver/src/locks/xaie_locks_aieml.c    (lock adres kodlamasi)
 *   driver/src/lite/xaie_lite_hwcfg.h      (shift'ler, tile sayilari)
 *   driver/tests/stest/hw_config.h         (AIE_GEN 2, DEVICE 0 topolojisi)
 *   driver/src/common/xaie_txn.h           (transaction opcode'lari)
 *   driver/src/global/xaiegbl.h            (transaction serilestirme yapilari)
 */

#ifndef XDNA_AIE_H
#define XDNA_AIE_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Adres kodlamasi -- xaie_lite_hwcfg.h (AIE_GEN 2)                     */
/* ------------------------------------------------------------------ */
#define AIE_COL_SHIFT       25u
#define AIE_ROW_SHIFT       20u
#define AIE_TILE_OFF_MASK   ((1u << AIE_ROW_SHIFT) - 1u)  /* 0xFFFFF */

#define AIE_ADDR(col, row, off) \
    (((uint32_t)(col) << AIE_COL_SHIFT) | ((uint32_t)(row) << AIE_ROW_SHIFT) | \
     ((uint32_t)(off) & AIE_TILE_OFF_MASK))

#define AIE_ADDR_COL(a) (((a) >> AIE_COL_SHIFT) & 0x7Fu)
#define AIE_ADDR_ROW(a) (((a) >> AIE_ROW_SHIFT) & 0x1Fu)
#define AIE_ADDR_OFF(a) ((a) & AIE_TILE_OFF_MASK)

/*
 * Topoloji -- hw_config.h icindeki AIE_GEN 2 / DEVICE 0 ("SystemC Config")
 * blogu Phoenix'in yerlesimiyle birebir ayni: 5 kolon, 6 satir,
 * shim satir 0, mem tile satir 1, compute tile satir 2..5.
 */
#define AIE_NUM_COLS        5u
#define AIE_NUM_ROWS        6u
#define AIE_SHIM_ROW        0u
#define AIE_MEM_ROW_START   1u
#define AIE_MEM_NUM_ROWS    1u
#define AIE_CORE_ROW_START  2u
#define AIE_CORE_NUM_ROWS   4u

/* Bellek boyutlari (AIE-ML). 5 x 512 KiB = 2560 KiB on-chip L2. */
#define AIE_CORE_DATA_MEM_SIZE  (64u * 1024u)
#define AIE_CORE_PROG_MEM_SIZE  (16u * 1024u)
#define AIE_MEM_TILE_MEM_SIZE   (512u * 1024u)

/* Lock ve DMA kanal sayilari -- xaie_lite_hwcfg.h */
#define AIE_TILE_NUM_LOCKS      16u
#define AIE_MEM_TILE_NUM_LOCKS  64u
#define AIE_SHIM_NUM_LOCKS      16u
#define AIE_TILE_DMA_NUM_CH     2u
#define AIE_MEM_TILE_DMA_NUM_CH 6u
#define AIE_SHIM_DMA_NUM_CH     2u

#define AIE_MAX_LOCKS           AIE_MEM_TILE_NUM_LOCKS
#define AIE_MAX_DMA_CH          AIE_MEM_TILE_DMA_NUM_CH
/*
 * BD sayisi tile turune gore farkli: shim ve compute tile'da BD id alani
 * 4 bit (16), memory tile'da 6 bit (48). Tek sabit kullanmak yanlis olur --
 * shim'de 48 BD, DMA kontrol registerlarinin uzerine biner (0x1D200).
 */
#define AIE_NUM_BDS_SHIM        16u
#define AIE_NUM_BDS_CORE        16u
#define AIE_NUM_BDS_MEMT        48u
#define AIE_MAX_BDS             AIE_NUM_BDS_MEMT
#define AIE_BD_WORDS            8u
#define AIE_BD_STRIDE           0x20u

/* ------------------------------------------------------------------ */
/* Tile ici register offsetleri -- xaiemlgbl_params.h                   */
/* ------------------------------------------------------------------ */

/* Compute tile (MEMORY_MODULE / CORE_MODULE) */
#define AIEML_CORE_DATA_MEM         0x00000000u
#define AIEML_CORE_PROG_MEM         0x00020000u
#define AIEML_CORE_CONTROL          0x00032000u
#define AIEML_CORE_STATUS           0x00032004u
#define AIEML_CORE_DMA_BD0          0x0001D000u
#define AIEML_CORE_DMA_S2MM0_CTRL   0x0001DE00u
#define AIEML_CORE_DMA_MM2S0_CTRL   0x0001DE10u
#define AIEML_CORE_LOCK_REQUEST     0x00040000u
#define AIEML_CORE_LOCK0_VALUE      0x0001F000u

/* Memory tile (MEM_TILE_MODULE) */
#define AIEML_MEMT_DATA_MEM         0x00000000u
#define AIEML_MEMT_DMA_BD0          0x000A0000u
#define AIEML_MEMT_DMA_S2MM0_CTRL   0x000A0600u
#define AIEML_MEMT_DMA_MM2S0_CTRL   0x000A0630u
#define AIEML_MEMT_LOCK_REQUEST     0x000D0000u
#define AIEML_MEMT_LOCK0_VALUE      0x000C0000u

/* Shim / NOC tile (NOC_MODULE) */
#define AIEML_SHIM_DMA_BD0          0x0001D000u
#define AIEML_SHIM_DMA_S2MM0_CTRL   0x0001D200u
#define AIEML_SHIM_DMA_MM2S0_CTRL   0x0001D210u
#define AIEML_SHIM_LOCK_REQUEST     0x00040000u
#define AIEML_SHIM_LOCK0_VALUE      0x00014000u

/*
 * DMA kanal registerlari: her kanal 8 bayt; CTRL'nin 4 bayt sonrasi
 * "task queue" (shim) / "start queue" (core, mem tile) registeri.
 * Ikisi de ayni islevi goruyor: bir BD zincirini kanala kuyruklar.
 */
#define AIEML_DMA_CH_STRIDE         0x8u
#define AIEML_DMA_QUEUE_OFF         0x4u

/* Lock value registerlari: LOCK0_VALUE + id * 0x10 */
#define AIEML_LOCK_VALUE_STRIDE     0x10u

/* ------------------------------------------------------------------ */
/* Lock adres kodlamasi -- xaie_locks_aieml.c + xaiemlgbl_reginit.c     */
/* ------------------------------------------------------------------ */
/*
 * Islem, LOCK_REQUEST bolgesine yapilan bir OKUMA ile gerceklesir
 * (aie-rt bunu XAie_MaskPoll ile yapar). Donen degerin bit0'i sonuctur.
 *
 *   offset = LOCK_REQUEST + id * 0x400 + (acquire ? 0x200 : 0)
 *                         + ((deger & 0x7F) << 2)
 *
 * Deger 7 bit isaretli (-64..63). Release: lock += v. Acquire: lock >= v
 * ise lock -= v ve basari, degilse basarisiz.
 */
#define AIEML_LOCK_ID_STRIDE        0x400u
#define AIEML_LOCK_RELACQ_OFF       0x200u
#define AIEML_LOCK_VALUE_MASK       0x7Fu
#define AIEML_LOCK_VALUE_SHIFT      2u
#define AIEML_LOCK_RESULT_MASK      0x1u
#define AIEML_LOCK_RESULT_SUCCESS   0x1u
#define AIEML_LOCK_VAL_MAX          63
#define AIEML_LOCK_VAL_MIN          (-64)

/* ------------------------------------------------------------------ */
/* Buffer descriptor bit alanlari -- xaiemlgbl_params.h                 */
/* ------------------------------------------------------------------ */
/*
 * BD yerlesimi tile turune gore FARKLIDIR. Ucu de ayri ayri dogrulandi;
 * ortak bir "BD formati" varsaymak yanlis olur.
 */

/* --- Shim / NOC tile BD (NOC_MODULE_DMA_BD*) --- */
#define AIE_SHIM_BD_LEN_WORD        0u   /* w0[31:0], 32-bit kelime sayisi */
#define AIE_SHIM_BD_ADDRLO_WORD     1u   /* w1[31:2] adres bitleri [31:2]  */
#define AIE_SHIM_BD_ADDRHI_WORD     2u   /* w2[15:0] adres bitleri [47:32] */
#define AIE_SHIM_BD_CTRL_WORD       7u
#define AIE_SHIM_BD_ADDRLO_MASK     0xFFFFFFFCu
#define AIE_SHIM_BD_ADDRHI_MASK     0x0000FFFFu
#define AIE_SHIM_BD_VALID_LSB       25u
#define AIE_SHIM_BD_USE_NEXT_LSB    26u
#define AIE_SHIM_BD_NEXT_LSB        27u
#define AIE_SHIM_BD_NEXT_WIDTH      4u
#define AIE_SHIM_BD_REL_VAL_LSB     18u
#define AIE_SHIM_BD_REL_ID_LSB      13u
#define AIE_SHIM_BD_REL_ID_WIDTH    4u
#define AIE_SHIM_BD_ACQ_EN_LSB      12u
#define AIE_SHIM_BD_ACQ_VAL_LSB     5u
#define AIE_SHIM_BD_ACQ_ID_LSB      0u
#define AIE_SHIM_BD_ACQ_ID_WIDTH    4u

/* --- Memory tile BD (MEM_TILE_MODULE_DMA_BD*) --- */
#define AIE_MEMT_BD_LEN_WORD        0u   /* w0[16:0] kelime sayisi         */
#define AIE_MEMT_BD_LEN_WIDTH       17u
#define AIE_MEMT_BD_ADDR_WORD       1u   /* w1[18:0] kelime adresi         */
#define AIE_MEMT_BD_ADDR_WIDTH      19u
#define AIE_MEMT_BD_USE_NEXT_LSB    19u
#define AIE_MEMT_BD_NEXT_LSB        20u
#define AIE_MEMT_BD_NEXT_WIDTH      6u
#define AIE_MEMT_BD_CTRL_WORD       7u
#define AIE_MEMT_BD_VALID_LSB       31u
#define AIE_MEMT_BD_REL_VAL_LSB     24u
#define AIE_MEMT_BD_REL_ID_LSB      16u
#define AIE_MEMT_BD_REL_ID_WIDTH    8u
#define AIE_MEMT_BD_ACQ_EN_LSB      15u
#define AIE_MEMT_BD_ACQ_VAL_LSB     8u
#define AIE_MEMT_BD_ACQ_ID_LSB      0u
#define AIE_MEMT_BD_ACQ_ID_WIDTH    8u

/* --- Compute tile BD (MEMORY_MODULE_DMA_BD*) --- */
#define AIE_CORE_BD_LEN_WORD        0u   /* w0[13:0] kelime sayisi         */
#define AIE_CORE_BD_LEN_WIDTH       14u
#define AIE_CORE_BD_ADDR_LSB        14u  /* w0[27:14] kelime adresi        */
#define AIE_CORE_BD_ADDR_WIDTH      14u
#define AIE_CORE_BD_CTRL_WORD       5u
#define AIE_CORE_BD_VALID_LSB       25u
#define AIE_CORE_BD_USE_NEXT_LSB    26u
#define AIE_CORE_BD_NEXT_LSB        27u
#define AIE_CORE_BD_NEXT_WIDTH      4u
#define AIE_CORE_BD_REL_VAL_LSB     18u
#define AIE_CORE_BD_REL_ID_LSB      13u
#define AIE_CORE_BD_REL_ID_WIDTH    4u
#define AIE_CORE_BD_ACQ_EN_LSB      12u
#define AIE_CORE_BD_ACQ_VAL_LSB     5u
#define AIE_CORE_BD_ACQ_ID_LSB      0u
#define AIE_CORE_BD_ACQ_ID_WIDTH    4u

/* Lock deger alanlari her yerde 7 bit isaretli. */
#define AIE_BD_LOCK_VAL_WIDTH       7u

/* ------------------------------------------------------------------ */
/* Stream switch -- xaiemlgbl_params.h + xaie_plif.c                    */
/* ------------------------------------------------------------------ */
/*
 * Devre anahtarlamali (circuit-switched) yonlendirme:
 *   MASTER_CONFIG[m].CONFIGURATION = bu master portu besleyen slave port
 *   MASTER_CONFIG[m].MASTER_ENABLE = baglanti etkin mi
 *   SLAVE_CONFIG[s].SLAVE_ENABLE   = slave port etkin mi
 *
 * Komsu baglantisi (mesh): master NORTH<k> -> ustteki tile'in slave
 * SOUTH_<k> portu; SOUTH -> alttakinin NORTH'u; EAST -> sagdakinin WEST'i;
 * WEST -> soldakinin EAST'i. Port sayilari bu eslemeyi dogruluyor
 * (orn. compute tile 6 NORTH master, ustteki tile 6 SOUTH slave).
 */
#define AIEML_CORE_SS_MASTER        0x0003F000u
#define AIEML_CORE_SS_SLAVE         0x0003F100u
#define AIEML_MEMT_SS_MASTER        0x000B0000u
#define AIEML_MEMT_SS_SLAVE         0x000B0100u
#define AIEML_SHIM_SS_MASTER        0x0003F000u
#define AIEML_SHIM_SS_SLAVE         0x0003F100u

#define AIE_SS_MASTER_ENABLE_MASK   0x80000000u
#define AIE_SS_MASTER_PACKET_MASK   0x40000000u
#define AIE_SS_MASTER_CFG_MASK      0x0000007Fu
#define AIE_SS_SLAVE_ENABLE_MASK    0x80000000u
#define AIE_SS_SLAVE_PACKET_MASK    0x40000000u

#define AIE_SS_MAX_PORTS            25u

/*
 * Shim MUX/DEMUX: shim'in guney portlarinin NoC/DMA/PL'den hangisine
 * bagli oldugunu secer. 2 bit alanlar.
 *   MUX   (slave tarafi):  SOUTH2@8, SOUTH3@10, SOUTH6@12, SOUTH7@14
 *   DEMUX (master tarafi): SOUTH2@4, SOUTH3@6,  SOUTH4@8,  SOUTH5@10
 */
#define AIEML_SHIM_MUX_CONFIG       0x0001F000u
#define AIEML_SHIM_DEMUX_CONFIG     0x0001F004u
#define AIE_MUX_TYPE_PL             0u
#define AIE_MUX_TYPE_DMA            1u
#define AIE_MUX_TYPE_NOC            2u

/*
 * Shim DMA'nin stream switch portlari -- xaie_plif.c:
 *   XAie_EnableShimDmaToAieStrmPort  : slave SOUTH port 3 veya 7 (host -> AIE)
 *   XAie_EnableAieToShimDmaStrmPort  : master SOUTH port 2 veya 3 (AIE -> host)
 * Kanal <-> port eslemesi (ch0 -> ilk port) SIRALAMADAN CIKARIM;
 * gercek donanimla dogrulanmali.
 */
#define AIE_SHIM_MM2S_SOUTH_PORT(ch) ((ch) == 0 ? 3u : 7u)
#define AIE_SHIM_S2MM_SOUTH_PORT(ch) ((ch) == 0 ? 2u : 3u)

/* Task/start queue */
#define AIE_DMA_QUEUE_START_BD_MASK 0x0000000Fu
#define AIE_DMA_QUEUE_REPEAT_LSB    16u
#define AIE_DMA_QUEUE_REPEAT_MASK   0x00FF0000u

/* Core control */
#define AIE_CORE_CTRL_ENABLE_MASK   0x00000001u
#define AIE_CORE_CTRL_RESET_MASK    0x00000002u

/* ------------------------------------------------------------------ */
/* Transaction (ctrlcode) formati -- xaie_txn.h + xaiegbl.h             */
/* ------------------------------------------------------------------ */
enum XdnaTxnOpcode {
    XAIE_IO_WRITE                = 0,
    XAIE_IO_BLOCKWRITE           = 1,
    XAIE_IO_BLOCKSET             = 2,
    XAIE_IO_MASKWRITE            = 3,
    XAIE_IO_MASKPOLL             = 4,
    XAIE_CONFIG_SHIMDMA_BD       = 5,
    XAIE_CONFIG_SHIMDMA_DMABUF_BD = 6,
    XAIE_IO_CUSTOM_OP_BEGIN      = 128,
    XAIE_IO_CUSTOM_OP_TCT        = 128,
    XAIE_IO_CUSTOM_OP_DDR_PATCH  = 129,
};

#endif /* XDNA_AIE_H */
