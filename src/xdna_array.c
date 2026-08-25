// SPDX-License-Identifier: GPL-2.0-only
/*
 * XDNA1 array mimari modeli -- 5 kolon x (1 shim + 1 memory + 4 compute).
 *
 * Bu dosya array'in adres uzayini, bellegini, lock'larini ve DMA
 * motorlarini modelliyor. ctrlcode (xdna_txn.c) bu API uzerinden array'i
 * konfigure ediyor ve calistiriyor.
 *
 * Tasarim: bilinen registerlar SEMANTIK olarak modelleniyor (lock'lar, BD'ler,
 * DMA kanallari, core control). Bilinmeyen registerlar seyrek bir tabloda
 * saklanip aynen geri okunuyor -- boylece ctrlcode'un yaptigi her yazma
 * kaybolmuyor ve MASKPOLL beklentileri tutarli kaliyor.
 *
 * Register haritasi ve bit alanlari icin: include/xdna/xdna_aie.h
 */

#include <stdlib.h>
#include <string.h>

#include "xdna_internal.h"

/* ---------------------------------------------------------------- */
/* Seyrek register tablosu                                           */
/* ---------------------------------------------------------------- */

#define REGMAP_EMPTY 0xFFFFFFFFu

static uint32_t regmap_hash(uint32_t key)
{
    key *= 2654435761u;
    return key ^ (key >> 16);
}

static int regmap_grow(AieRegMap *m)
{
    uint32_t new_cap = m->cap ? m->cap * 2u : 64u;
    AieRegEntry *old = m->tab;
    uint32_t old_cap = m->cap;
    uint32_t i;

    m->tab = calloc(new_cap, sizeof(*m->tab));
    if (!m->tab) {
        m->tab = old;
        return -1;
    }
    for (i = 0; i < new_cap; i++) {
        m->tab[i].off = REGMAP_EMPTY;
    }
    m->cap = new_cap;
    m->used = 0;

    for (i = 0; i < old_cap; i++) {
        if (old[i].off != REGMAP_EMPTY) {
            uint32_t j = regmap_hash(old[i].off) & (new_cap - 1u);
            while (m->tab[j].off != REGMAP_EMPTY) {
                j = (j + 1u) & (new_cap - 1u);
            }
            m->tab[j] = old[i];
            m->used++;
        }
    }
    free(old);
    return 0;
}

static uint32_t regmap_get(const AieRegMap *m, uint32_t off)
{
    uint32_t i;

    if (!m->cap) {
        return 0;
    }
    i = regmap_hash(off) & (m->cap - 1u);
    while (m->tab[i].off != REGMAP_EMPTY) {
        if (m->tab[i].off == off) {
            return m->tab[i].val;
        }
        i = (i + 1u) & (m->cap - 1u);
    }
    return 0;
}

static void regmap_set(AieRegMap *m, uint32_t off, uint32_t val)
{
    uint32_t i;

    if (off == REGMAP_EMPTY) {
        return;  /* bu offset saklanamaz; pratikte hic olusmuyor */
    }
    if (m->used * 4u >= m->cap * 3u) {
        if (regmap_grow(m) != 0) {
            return;
        }
    }
    i = regmap_hash(off) & (m->cap - 1u);
    while (m->tab[i].off != REGMAP_EMPTY) {
        if (m->tab[i].off == off) {
            m->tab[i].val = val;
            return;
        }
        i = (i + 1u) & (m->cap - 1u);
    }
    m->tab[i].off = off;
    m->tab[i].val = val;
    m->used++;
}

static void regmap_free(AieRegMap *m)
{
    free(m->tab);
    m->tab = NULL;
    m->cap = 0;
    m->used = 0;
}

/* ---------------------------------------------------------------- */
/* Tile yardimcilari                                                 */
/* ---------------------------------------------------------------- */

static AieTileKind tile_kind_for_row(uint8_t row)
{
    if (row == AIE_SHIM_ROW) {
        return AIE_TILE_SHIM;
    }
    if (row < AIE_CORE_ROW_START) {
        return AIE_TILE_MEM;
    }
    return AIE_TILE_CORE;
}

AieTile *xdna_array_tile(XdnaArray *arr, uint8_t col, uint8_t row)
{
    if (col >= AIE_NUM_COLS || row >= AIE_NUM_ROWS) {
        return NULL;
    }
    return &arr->tile[col][row];
}

static uint32_t tile_num_locks(const AieTile *t)
{
    switch (t->kind) {
    case AIE_TILE_MEM:
        return AIE_MEM_TILE_NUM_LOCKS;
    case AIE_TILE_SHIM:
        return AIE_SHIM_NUM_LOCKS;
    default:
        return AIE_TILE_NUM_LOCKS;
    }
}

static uint32_t tile_num_dma_ch(const AieTile *t)
{
    switch (t->kind) {
    case AIE_TILE_MEM:
        return AIE_MEM_TILE_DMA_NUM_CH;
    case AIE_TILE_SHIM:
        return AIE_SHIM_DMA_NUM_CH;
    default:
        return AIE_TILE_DMA_NUM_CH;
    }
}

static uint32_t tile_num_bds(const AieTile *t)
{
    switch (t->kind) {
    case AIE_TILE_MEM:
        return AIE_NUM_BDS_MEMT;
    case AIE_TILE_SHIM:
        return AIE_NUM_BDS_SHIM;
    default:
        return AIE_NUM_BDS_CORE;
    }
}

static uint32_t tile_lock_request_base(const AieTile *t)
{
    switch (t->kind) {
    case AIE_TILE_MEM:
        return AIEML_MEMT_LOCK_REQUEST;
    case AIE_TILE_SHIM:
        return AIEML_SHIM_LOCK_REQUEST;
    default:
        return AIEML_CORE_LOCK_REQUEST;
    }
}

static uint32_t tile_lock_value_base(const AieTile *t)
{
    switch (t->kind) {
    case AIE_TILE_MEM:
        return AIEML_MEMT_LOCK0_VALUE;
    case AIE_TILE_SHIM:
        return AIEML_SHIM_LOCK0_VALUE;
    default:
        return AIEML_CORE_LOCK0_VALUE;
    }
}

static uint32_t tile_bd_base(const AieTile *t)
{
    switch (t->kind) {
    case AIE_TILE_MEM:
        return AIEML_MEMT_DMA_BD0;
    case AIE_TILE_SHIM:
        return AIEML_SHIM_DMA_BD0;
    default:
        return AIEML_CORE_DMA_BD0;
    }
}

static uint32_t tile_s2mm_base(const AieTile *t)
{
    switch (t->kind) {
    case AIE_TILE_MEM:
        return AIEML_MEMT_DMA_S2MM0_CTRL;
    case AIE_TILE_SHIM:
        return AIEML_SHIM_DMA_S2MM0_CTRL;
    default:
        return AIEML_CORE_DMA_S2MM0_CTRL;
    }
}

static uint32_t tile_mm2s_base(const AieTile *t)
{
    switch (t->kind) {
    case AIE_TILE_MEM:
        return AIEML_MEMT_DMA_MM2S0_CTRL;
    case AIE_TILE_SHIM:
        return AIEML_SHIM_DMA_MM2S0_CTRL;
    default:
        return AIEML_CORE_DMA_MM2S0_CTRL;
    }
}

/* 7 bit isaretli lock degerini coz. */
static int32_t lock_val_decode(uint32_t raw)
{
    uint32_t v = raw & AIEML_LOCK_VALUE_MASK;

    return (v & 0x40u) ? (int32_t)v - 0x80 : (int32_t)v;
}

static uint32_t bits(uint32_t word, uint32_t lsb, uint32_t width)
{
    return (word >> lsb) & ((width >= 32u) ? 0xFFFFFFFFu : ((1u << width) - 1u));
}

/* ---------------------------------------------------------------- */
/* Kurulum                                                           */
/* ---------------------------------------------------------------- */

static void tile_free_mem(AieTile *t)
{
    uint32_t i;

    free(t->data);
    free(t->prog);
    t->data = NULL;
    t->prog = NULL;
    for (i = 0; i < AIE_MAX_DMA_CH; i++) {
        free(t->in_fifo[i].buf);
        t->in_fifo[i].buf = NULL;
        t->in_fifo[i].cap = 0;
        t->in_fifo[i].len = 0;
        t->in_fifo[i].rd = 0;
    }
    regmap_free(&t->regs);
}

XdnaArray *xdna_array_new(XdnaNpu *npu)
{
    XdnaArray *arr = calloc(1, sizeof(*arr));
    uint8_t col, row;

    if (!arr) {
        return NULL;
    }
    arr->npu = npu;

    for (col = 0; col < AIE_NUM_COLS; col++) {
        for (row = 0; row < AIE_NUM_ROWS; row++) {
            AieTile *t = &arr->tile[col][row];

            t->kind = tile_kind_for_row(row);
            t->col = col;
            t->row = row;

            if (t->kind == AIE_TILE_MEM) {
                t->data_size = AIE_MEM_TILE_MEM_SIZE;
            } else if (t->kind == AIE_TILE_CORE) {
                t->data_size = AIE_CORE_DATA_MEM_SIZE;
                t->prog_size = AIE_CORE_PROG_MEM_SIZE;
            }
            if (t->data_size) {
                t->data = calloc(1, t->data_size);
                if (!t->data) {
                    goto fail;
                }
            }
            if (t->prog_size) {
                t->prog = calloc(1, t->prog_size);
                if (!t->prog) {
                    goto fail;
                }
            }
        }
    }
    return arr;

fail:
    xdna_array_free(arr);
    return NULL;
}

void xdna_array_free(XdnaArray *arr)
{
    uint8_t col, row;

    if (!arr) {
        return;
    }
    for (col = 0; col < AIE_NUM_COLS; col++) {
        for (row = 0; row < AIE_NUM_ROWS; row++) {
            tile_free_mem(&arr->tile[col][row]);
        }
    }
    free(arr);
}

void xdna_array_reset(XdnaArray *arr)
{
    uint8_t col, row;
    uint32_t i;

    for (col = 0; col < AIE_NUM_COLS; col++) {
        for (row = 0; row < AIE_NUM_ROWS; row++) {
            AieTile *t = &arr->tile[col][row];

            if (t->data) {
                memset(t->data, 0, t->data_size);
            }
            if (t->prog) {
                memset(t->prog, 0, t->prog_size);
            }
            memset(t->lock, 0, sizeof(t->lock));
            memset(t->bd, 0, sizeof(t->bd));
            memset(t->ch_ctrl, 0, sizeof(t->ch_ctrl));
            memset(t->ch_queue, 0, sizeof(t->ch_queue));
            t->core_ctrl = 0;
            t->core_status = 0;
            memset(t->ss_master, 0, sizeof(t->ss_master));
            memset(t->ss_slave, 0, sizeof(t->ss_slave));
            t->shim_mux = 0;
            t->shim_demux = 0;
            for (i = 0; i < AIE_MAX_DMA_CH; i++) {
                t->in_fifo[i].len = 0;
                t->in_fifo[i].rd = 0;
            }
            regmap_free(&t->regs);
        }
    }
    memset(&arr->stats, 0, sizeof(arr->stats));
}

/* ---------------------------------------------------------------- */
/* Stream switch                                                     */
/* ---------------------------------------------------------------- */
/*
 * Port tablolari xaiemlgbl_params.h icindeki register siralamasindan
 * cikarildi (offset sirasi = port indeksi). Master CONFIGURATION alani
 * bu indeks uzayindaki bir SLAVE portu gosterir.
 */

#define P(c, i) { AIE_PC_##c, (i) }

static const AiePortDesc core_master_ports[] = {
    P(CORE, 0), P(DMA, 0), P(DMA, 1), P(CTRL, 0), P(FIFO, 0),
    P(SOUTH, 0), P(SOUTH, 1), P(SOUTH, 2), P(SOUTH, 3),
    P(WEST, 0), P(WEST, 1), P(WEST, 2), P(WEST, 3),
    P(NORTH, 0), P(NORTH, 1), P(NORTH, 2), P(NORTH, 3), P(NORTH, 4), P(NORTH, 5),
    P(EAST, 0), P(EAST, 1), P(EAST, 2), P(EAST, 3),
};

static const AiePortDesc core_slave_ports[] = {
    P(CORE, 0), P(DMA, 0), P(DMA, 1), P(CTRL, 0), P(FIFO, 0),
    P(SOUTH, 0), P(SOUTH, 1), P(SOUTH, 2), P(SOUTH, 3), P(SOUTH, 4), P(SOUTH, 5),
    P(WEST, 0), P(WEST, 1), P(WEST, 2), P(WEST, 3),
    P(NORTH, 0), P(NORTH, 1), P(NORTH, 2), P(NORTH, 3),
    P(EAST, 0), P(EAST, 1), P(EAST, 2), P(EAST, 3),
    P(TRACE, 0), P(TRACE, 1),
};

static const AiePortDesc memt_master_ports[] = {
    P(DMA, 0), P(DMA, 1), P(DMA, 2), P(DMA, 3), P(DMA, 4), P(DMA, 5),
    P(CTRL, 0),
    P(SOUTH, 0), P(SOUTH, 1), P(SOUTH, 2), P(SOUTH, 3),
    P(NORTH, 0), P(NORTH, 1), P(NORTH, 2), P(NORTH, 3), P(NORTH, 4), P(NORTH, 5),
};

static const AiePortDesc memt_slave_ports[] = {
    P(DMA, 0), P(DMA, 1), P(DMA, 2), P(DMA, 3), P(DMA, 4), P(DMA, 5),
    P(CTRL, 0),
    P(SOUTH, 0), P(SOUTH, 1), P(SOUTH, 2), P(SOUTH, 3), P(SOUTH, 4), P(SOUTH, 5),
    P(NORTH, 0), P(NORTH, 1), P(NORTH, 2), P(NORTH, 3),
    P(TRACE, 0),
};

static const AiePortDesc shim_master_ports[] = {
    P(CTRL, 0), P(FIFO, 0),
    P(SOUTH, 0), P(SOUTH, 1), P(SOUTH, 2), P(SOUTH, 3), P(SOUTH, 4), P(SOUTH, 5),
    P(WEST, 0), P(WEST, 1), P(WEST, 2), P(WEST, 3),
    P(NORTH, 0), P(NORTH, 1), P(NORTH, 2), P(NORTH, 3), P(NORTH, 4), P(NORTH, 5),
    P(EAST, 0), P(EAST, 1), P(EAST, 2), P(EAST, 3),
};

static const AiePortDesc shim_slave_ports[] = {
    P(CTRL, 0), P(FIFO, 0),
    P(SOUTH, 0), P(SOUTH, 1), P(SOUTH, 2), P(SOUTH, 3),
    P(SOUTH, 4), P(SOUTH, 5), P(SOUTH, 6), P(SOUTH, 7),
    P(WEST, 0), P(WEST, 1), P(WEST, 2), P(WEST, 3),
    P(NORTH, 0), P(NORTH, 1), P(NORTH, 2), P(NORTH, 3),
    P(EAST, 0), P(EAST, 1), P(EAST, 2), P(EAST, 3),
    P(TRACE, 0),
};

#undef P

static const AiePortDesc *master_ports(AieTileKind kind, uint32_t *n)
{
    switch (kind) {
    case AIE_TILE_MEM:
        *n = (uint32_t)(sizeof(memt_master_ports) / sizeof(AiePortDesc));
        return memt_master_ports;
    case AIE_TILE_SHIM:
        *n = (uint32_t)(sizeof(shim_master_ports) / sizeof(AiePortDesc));
        return shim_master_ports;
    default:
        *n = (uint32_t)(sizeof(core_master_ports) / sizeof(AiePortDesc));
        return core_master_ports;
    }
}

static const AiePortDesc *slave_ports(AieTileKind kind, uint32_t *n)
{
    switch (kind) {
    case AIE_TILE_MEM:
        *n = (uint32_t)(sizeof(memt_slave_ports) / sizeof(AiePortDesc));
        return memt_slave_ports;
    case AIE_TILE_SHIM:
        *n = (uint32_t)(sizeof(shim_slave_ports) / sizeof(AiePortDesc));
        return shim_slave_ports;
    default:
        *n = (uint32_t)(sizeof(core_slave_ports) / sizeof(AiePortDesc));
        return core_slave_ports;
    }
}

static int find_port(const AiePortDesc *tab, uint32_t n, AiePortClass cls,
                     uint32_t idx)
{
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (tab[i].cls == cls && tab[i].idx == idx) {
            return (int)i;
        }
    }
    return -1;
}

static int slave_port_of(AieTileKind kind, AiePortClass cls, uint32_t idx)
{
    uint32_t n;
    const AiePortDesc *tab = slave_ports(kind, &n);

    return find_port(tab, n, cls, idx);
}

static uint32_t tile_ss_master_base(const AieTile *t)
{
    return (t->kind == AIE_TILE_MEM) ? AIEML_MEMT_SS_MASTER
                                     : AIEML_CORE_SS_MASTER;
}

static uint32_t tile_ss_slave_base(const AieTile *t)
{
    return (t->kind == AIE_TILE_MEM) ? AIEML_MEMT_SS_SLAVE
                                     : AIEML_CORE_SS_SLAVE;
}

/* ---------------------------------------------------------------- */

static int stream_push(AieStream *s, const void *src, uint32_t len)
{
    if (s->len + len > s->cap) {
        size_t ncap = s->cap ? s->cap * 2u : 4096u;
        uint8_t *nbuf;

        while (ncap < s->len + len) {
            ncap *= 2u;
        }
        nbuf = realloc(s->buf, ncap);
        if (!nbuf) {
            return -1;
        }
        s->buf = nbuf;
        s->cap = ncap;
    }
    memcpy(s->buf + s->len, src, len);
    s->len += len;
    return 0;
}

static int stream_pop(AieStream *s, void *dst, uint32_t len)
{
    if (s->len - s->rd < len) {
        return -1;
    }
    memcpy(dst, s->buf + s->rd, len);
    s->rd += len;
    if (s->rd == s->len) {
        s->rd = 0;
        s->len = 0;
    }
    return 0;
}

#define SS_MAX_DEPTH 32

static void ss_inject(XdnaArray *arr, int col, int row, int slave_port,
                      const uint8_t *data, uint32_t len, int depth);

/* Bir master porta ulasan veriyi hedefine tasi. */
static void ss_deliver(XdnaArray *arr, AieTile *t, uint32_t master_port,
                       const uint8_t *data, uint32_t len, int depth)
{
    uint32_t n;
    const AiePortDesc *tab = master_ports(t->kind, &n);
    AiePortDesc p;

    if (master_port >= n) {
        return;
    }
    p = tab[master_port];

    switch (p.cls) {
    case AIE_PC_DMA:
        /* Tile DMA S2MM kanalinin giris FIFO'su */
        if (p.idx < tile_num_dma_ch(t)) {
            if (stream_push(&t->in_fifo[p.idx], data, len) != 0) {
                arr->stats.stream_drops++;
            }
        }
        return;

    case AIE_PC_SOUTH:
        if (t->row == AIE_SHIM_ROW) {
            /*
             * Shim'in guneyi NoC/PL tarafi. DEMUX bu portu DMA'ya
             * yonlendirmisse shim S2MM kanalinin FIFO'suna dusuyor.
             */
            uint32_t ch;

            for (ch = 0; ch < AIE_SHIM_DMA_NUM_CH; ch++) {
                if (AIE_SHIM_S2MM_SOUTH_PORT(ch) != p.idx) {
                    continue;
                }
                /* DEMUX alanlari: SOUTH2@4, SOUTH3@6, SOUTH4@8, SOUTH5@10 */
                if (p.idx < 2u ||
                    ((t->shim_demux >> (4u + 2u * (p.idx - 2u))) & 0x3u) !=
                        AIE_MUX_TYPE_DMA) {
                    xdna_log(arr->npu, XDNA_LOG_WARN,
                             "stream: shim(%u) SOUTH%u DEMUX DMA'ya ayarli degil",
                             t->col, p.idx);
                    arr->stats.stream_drops++;
                    return;
                }
                if (stream_push(&t->in_fifo[ch], data, len) != 0) {
                    arr->stats.stream_drops++;
                }
                return;
            }
            arr->stats.stream_drops++;
            return;
        }
        ss_inject(arr, t->col, t->row - 1,
                  slave_port_of(tile_kind_for_row((uint8_t)(t->row - 1)),
                                AIE_PC_NORTH, p.idx),
                  data, len, depth + 1);
        return;

    case AIE_PC_NORTH:
        ss_inject(arr, t->col, t->row + 1,
                  slave_port_of(tile_kind_for_row((uint8_t)(t->row + 1)),
                                AIE_PC_SOUTH, p.idx),
                  data, len, depth + 1);
        return;

    case AIE_PC_EAST:
        ss_inject(arr, t->col + 1, t->row,
                  slave_port_of(t->kind, AIE_PC_WEST, p.idx), data, len,
                  depth + 1);
        return;

    case AIE_PC_WEST:
        ss_inject(arr, t->col - 1, t->row,
                  slave_port_of(t->kind, AIE_PC_EAST, p.idx), data, len,
                  depth + 1);
        return;

    default:
        /*
         * Compute tile stream girisi, tile control ve trace portlari
         * modellenmedi (AIE interpreter yok). Veriyi sessizce yutmuyoruz.
         */
        xdna_log(arr->npu, XDNA_LOG_WARN,
                 "stream: tile(%u,%u) master port sinifi %u modellenmedi",
                 t->col, t->row, p.cls);
        arr->stats.stream_drops++;
        return;
    }
}

/* Veriyi bir tile'in slave portuna sok ve switch uzerinden dagit. */
static void ss_inject(XdnaArray *arr, int col, int row, int slave_port,
                      const uint8_t *data, uint32_t len, int depth)
{
    AieTile *t;
    uint32_t n, m, nmaster;
    const AiePortDesc *mtab;
    bool delivered = false;

    if (depth > SS_MAX_DEPTH) {
        xdna_log(arr->npu, XDNA_LOG_ERROR, "stream: yonlendirme dongusu");
        arr->stats.stream_drops++;
        return;
    }
    if (col < 0 || row < 0 || slave_port < 0) {
        arr->stats.stream_drops++;
        return;
    }
    t = xdna_array_tile(arr, (uint8_t)col, (uint8_t)row);
    if (!t) {
        arr->stats.stream_drops++;
        return;
    }

    (void)slave_ports(t->kind, &n);
    if ((uint32_t)slave_port >= n) {
        arr->stats.stream_drops++;
        return;
    }
    if (!(t->ss_slave[slave_port] & AIE_SS_SLAVE_ENABLE_MASK)) {
        xdna_log(arr->npu, XDNA_LOG_WARN,
                 "stream: tile(%u,%u) slave port %d etkin degil", t->col,
                 t->row, slave_port);
        arr->stats.stream_drops++;
        return;
    }

    arr->stats.stream_hops++;
    mtab = master_ports(t->kind, &nmaster);
    (void)mtab;
    for (m = 0; m < nmaster; m++) {
        uint32_t cfg = t->ss_master[m];

        if (!(cfg & AIE_SS_MASTER_ENABLE_MASK)) {
            continue;
        }
        if ((cfg & AIE_SS_MASTER_CFG_MASK) != (uint32_t)slave_port) {
            continue;
        }
        delivered = true;
        ss_deliver(arr, t, m, data, len, depth);
    }

    if (!delivered) {
        xdna_log(arr->npu, XDNA_LOG_WARN,
                 "stream: tile(%u,%u) slave port %d icin master yok", t->col,
                 t->row, slave_port);
        arr->stats.stream_drops++;
    }
}

/* MM2S verisini tile'in switch'ine sok. */
static void ss_inject_from_dma(XdnaArray *arr, AieTile *t, uint32_t ch,
                               const uint8_t *data, uint32_t len)
{
    int sp;

    if (t->kind == AIE_TILE_SHIM) {
        uint32_t port = AIE_SHIM_MM2S_SOUTH_PORT(ch);

        /* MUX alanlari: SOUTH2@8, SOUTH3@10, SOUTH6@12, SOUTH7@14 */
        uint32_t shift = (port == 3u) ? 10u : 14u;

        if (((t->shim_mux >> shift) & 0x3u) != AIE_MUX_TYPE_DMA) {
            xdna_log(arr->npu, XDNA_LOG_WARN,
                     "stream: shim(%u) SOUTH%u MUX DMA'ya ayarli degil",
                     t->col, port);
            arr->stats.stream_drops++;
            return;
        }
        sp = slave_port_of(t->kind, AIE_PC_SOUTH, port);
    } else {
        sp = slave_port_of(t->kind, AIE_PC_DMA, ch);
    }
    ss_inject(arr, t->col, t->row, sp, data, len, 0);
}

/* ---------------------------------------------------------------- */
/* Lock semantigi                                                    */
/* ---------------------------------------------------------------- */

/*
 * AIE-ML lock'lari semafor: release deger ekler, acquire "buyuk esit"
 * kosuluyla deger cikarir. Basari 1, basarisizlik 0 doner.
 */
static uint32_t lock_op(XdnaNpu *npu, AieTile *t, uint32_t id, int32_t val,
                        bool acquire)
{
    if (id >= tile_num_locks(t)) {
        xdna_log(npu, XDNA_LOG_WARN, "array: tile(%u,%u) gecersiz lock %u",
                 t->col, t->row, id);
        return 0;
    }

    if (acquire) {
        if (t->lock[id] < val) {
            return 0;
        }
        t->lock[id] -= val;
        return AIEML_LOCK_RESULT_SUCCESS;
    }

    if (t->lock[id] + val > AIEML_LOCK_VAL_MAX ||
        t->lock[id] + val < AIEML_LOCK_VAL_MIN) {
        xdna_log(npu, XDNA_LOG_WARN,
                 "array: tile(%u,%u) lock %u sinir disi (%d + %d)",
                 t->col, t->row, id, t->lock[id], val);
        return 0;
    }
    t->lock[id] += val;
    return AIEML_LOCK_RESULT_SUCCESS;
}

/* ---------------------------------------------------------------- */
/* DMA                                                               */
/* ---------------------------------------------------------------- */

typedef struct {
    uint32_t len_words;
    uint64_t addr;       /* shim: host fiziksel; diger: tile bayt offseti */
    bool valid;
    bool use_next;
    uint32_t next;
    bool acq_enable;
    uint32_t acq_id;
    int32_t acq_val;
    uint32_t rel_id;
    int32_t rel_val;
} AieBd;

static void bd_decode(const AieTile *t, uint32_t idx, AieBd *bd)
{
    const uint32_t *w = t->bd[idx];

    memset(bd, 0, sizeof(*bd));

    switch (t->kind) {
    case AIE_TILE_SHIM: {
        uint32_t c = w[AIE_SHIM_BD_CTRL_WORD];

        bd->len_words = w[AIE_SHIM_BD_LEN_WORD];
        bd->addr = (uint64_t)(w[AIE_SHIM_BD_ADDRLO_WORD] & AIE_SHIM_BD_ADDRLO_MASK) |
                   ((uint64_t)(w[AIE_SHIM_BD_ADDRHI_WORD] & AIE_SHIM_BD_ADDRHI_MASK) << 32);
        bd->valid = bits(c, AIE_SHIM_BD_VALID_LSB, 1) != 0;
        bd->use_next = bits(c, AIE_SHIM_BD_USE_NEXT_LSB, 1) != 0;
        bd->next = bits(c, AIE_SHIM_BD_NEXT_LSB, AIE_SHIM_BD_NEXT_WIDTH);
        bd->acq_enable = bits(c, AIE_SHIM_BD_ACQ_EN_LSB, 1) != 0;
        bd->acq_id = bits(c, AIE_SHIM_BD_ACQ_ID_LSB, AIE_SHIM_BD_ACQ_ID_WIDTH);
        bd->acq_val = lock_val_decode(bits(c, AIE_SHIM_BD_ACQ_VAL_LSB,
                                           AIE_BD_LOCK_VAL_WIDTH));
        bd->rel_id = bits(c, AIE_SHIM_BD_REL_ID_LSB, AIE_SHIM_BD_REL_ID_WIDTH);
        bd->rel_val = lock_val_decode(bits(c, AIE_SHIM_BD_REL_VAL_LSB,
                                           AIE_BD_LOCK_VAL_WIDTH));
        break;
    }
    case AIE_TILE_MEM: {
        uint32_t c = w[AIE_MEMT_BD_CTRL_WORD];

        bd->len_words = bits(w[AIE_MEMT_BD_LEN_WORD], 0, AIE_MEMT_BD_LEN_WIDTH);
        bd->addr = (uint64_t)bits(w[AIE_MEMT_BD_ADDR_WORD], 0,
                                  AIE_MEMT_BD_ADDR_WIDTH) * 4u;
        bd->use_next = bits(w[AIE_MEMT_BD_ADDR_WORD],
                            AIE_MEMT_BD_USE_NEXT_LSB, 1) != 0;
        bd->next = bits(w[AIE_MEMT_BD_ADDR_WORD], AIE_MEMT_BD_NEXT_LSB,
                        AIE_MEMT_BD_NEXT_WIDTH);
        bd->valid = bits(c, AIE_MEMT_BD_VALID_LSB, 1) != 0;
        bd->acq_enable = bits(c, AIE_MEMT_BD_ACQ_EN_LSB, 1) != 0;
        bd->acq_id = bits(c, AIE_MEMT_BD_ACQ_ID_LSB, AIE_MEMT_BD_ACQ_ID_WIDTH);
        bd->acq_val = lock_val_decode(bits(c, AIE_MEMT_BD_ACQ_VAL_LSB,
                                           AIE_BD_LOCK_VAL_WIDTH));
        bd->rel_id = bits(c, AIE_MEMT_BD_REL_ID_LSB, AIE_MEMT_BD_REL_ID_WIDTH);
        bd->rel_val = lock_val_decode(bits(c, AIE_MEMT_BD_REL_VAL_LSB,
                                           AIE_BD_LOCK_VAL_WIDTH));
        break;
    }
    default: {
        uint32_t c = w[AIE_CORE_BD_CTRL_WORD];

        bd->len_words = bits(w[AIE_CORE_BD_LEN_WORD], 0, AIE_CORE_BD_LEN_WIDTH);
        bd->addr = (uint64_t)bits(w[AIE_CORE_BD_LEN_WORD], AIE_CORE_BD_ADDR_LSB,
                                  AIE_CORE_BD_ADDR_WIDTH) * 4u;
        bd->valid = bits(c, AIE_CORE_BD_VALID_LSB, 1) != 0;
        bd->use_next = bits(c, AIE_CORE_BD_USE_NEXT_LSB, 1) != 0;
        bd->next = bits(c, AIE_CORE_BD_NEXT_LSB, AIE_CORE_BD_NEXT_WIDTH);
        bd->acq_enable = bits(c, AIE_CORE_BD_ACQ_EN_LSB, 1) != 0;
        bd->acq_id = bits(c, AIE_CORE_BD_ACQ_ID_LSB, AIE_CORE_BD_ACQ_ID_WIDTH);
        bd->acq_val = lock_val_decode(bits(c, AIE_CORE_BD_ACQ_VAL_LSB,
                                           AIE_BD_LOCK_VAL_WIDTH));
        bd->rel_id = bits(c, AIE_CORE_BD_REL_ID_LSB, AIE_CORE_BD_REL_ID_WIDTH);
        bd->rel_val = lock_val_decode(bits(c, AIE_CORE_BD_REL_VAL_LSB,
                                           AIE_BD_LOCK_VAL_WIDTH));
        break;
    }
    }
}

#define DMA_CHUNK 4096u

static int dma_transfer_bd(XdnaArray *arr, AieTile *t, const AieBd *bd, int dir,
                           uint32_t ch)
{
    XdnaNpu *npu = arr->npu;
    uint32_t remaining = bd->len_words * 4u;
    uint64_t addr = bd->addr;
    uint8_t chunk[DMA_CHUNK];

    while (remaining) {
        uint32_t n = remaining < DMA_CHUNK ? remaining : DMA_CHUNK;

        if (dir == AIE_DMA_MM2S) {
            /* Bellekten oku, stream'e yaz. */
            if (t->kind == AIE_TILE_SHIM) {
                if (!npu->ops->dma_read ||
                    npu->ops->dma_read(npu->opaque, addr, chunk, n) != 0) {
                    xdna_log(npu, XDNA_LOG_ERROR,
                             "DMA: host okuma hatasi 0x%llx",
                             (unsigned long long)addr);
                    return -1;
                }
            } else {
                if (!t->data || addr + n > t->data_size) {
                    xdna_log(npu, XDNA_LOG_ERROR,
                             "DMA: tile(%u,%u) bellek disi okuma 0x%llx+%u",
                             t->col, t->row, (unsigned long long)addr, n);
                    return -1;
                }
                memcpy(chunk, t->data + addr, n);
            }
            ss_inject_from_dma(arr, t, ch, chunk, n);
        } else {
            /* Stream'den oku, bellege yaz. */
            if (stream_pop(&t->in_fifo[ch], chunk, n) != 0) {
                xdna_log(npu, XDNA_LOG_ERROR,
                         "DMA: tile(%u,%u) kanal %u giris FIFO'su bos",
                         t->col, t->row, ch);
                return -1;
            }
            if (t->kind == AIE_TILE_SHIM) {
                if (!npu->ops->dma_write ||
                    npu->ops->dma_write(npu->opaque, addr, chunk, n) != 0) {
                    xdna_log(npu, XDNA_LOG_ERROR,
                             "DMA: host yazma hatasi 0x%llx",
                             (unsigned long long)addr);
                    return -1;
                }
            } else {
                if (!t->data || addr + n > t->data_size) {
                    xdna_log(npu, XDNA_LOG_ERROR,
                             "DMA: tile(%u,%u) bellek disi yazma 0x%llx+%u",
                             t->col, t->row, (unsigned long long)addr, n);
                    return -1;
                }
                memcpy(t->data + addr, chunk, n);
            }
        }
        addr += n;
        remaining -= n;
        arr->stats.dma_bytes += n;
    }
    return 0;
}

/* Kanaldaki BD zincirini yurut. */
static void dma_run_channel(XdnaArray *arr, AieTile *t, int dir, uint32_t ch)
{
    XdnaNpu *npu = arr->npu;
    uint32_t queue = t->ch_queue[dir][ch];
    uint32_t bd_id = queue & AIE_DMA_QUEUE_START_BD_MASK;
    uint32_t repeat = bits(queue, AIE_DMA_QUEUE_REPEAT_LSB, 8) + 1u;
    uint32_t guard = 0;
    uint32_t r;

    for (r = 0; r < repeat; r++) {
        uint32_t cur = bd_id;

        for (;;) {
            AieBd bd;

            if (++guard > AIE_MAX_BDS * 64u) {
                xdna_log(npu, XDNA_LOG_ERROR,
                         "DMA: tile(%u,%u) BD zinciri dongude", t->col, t->row);
                return;
            }
            if (cur >= tile_num_bds(t)) {
                xdna_log(npu, XDNA_LOG_ERROR, "DMA: gecersiz BD id %u", cur);
                return;
            }
            bd_decode(t, cur, &bd);

            if (!bd.valid) {
                xdna_log(npu, XDNA_LOG_WARN,
                         "DMA: tile(%u,%u) BD %u gecerli degil", t->col,
                         t->row, cur);
                return;
            }
            if (bd.acq_enable &&
                !lock_op(npu, t, bd.acq_id, bd.acq_val, true)) {
                xdna_log(npu, XDNA_LOG_WARN,
                         "DMA: tile(%u,%u) BD %u lock %u alinamadi (deger %d)",
                         t->col, t->row, cur, bd.acq_id, t->lock[bd.acq_id]);
                return;
            }
            if (dma_transfer_bd(arr, t, &bd, dir, ch) != 0) {
                return;
            }
            if (bd.rel_val) {
                lock_op(npu, t, bd.rel_id, bd.rel_val, false);
            }
            arr->stats.dma_tasks++;

            if (!bd.use_next) {
                break;
            }
            cur = bd.next;
        }
    }
}

/* ---------------------------------------------------------------- */
/* Array adres uzayi erisimi                                         */
/* ---------------------------------------------------------------- */

/*
 * Bir offsetin hangi kaynaga dustugunu cozer. Bulunamayanlar seyrek
 * tabloya gider.
 */
static bool decode_dma_reg(AieTile *t, uint32_t off, int *dir, uint32_t *ch,
                           bool *is_queue)
{
    uint32_t s2mm = tile_s2mm_base(t);
    uint32_t mm2s = tile_mm2s_base(t);
    uint32_t nch = tile_num_dma_ch(t);

    if (off >= s2mm && off < s2mm + nch * AIEML_DMA_CH_STRIDE) {
        *dir = AIE_DMA_S2MM;
        *ch = (off - s2mm) / AIEML_DMA_CH_STRIDE;
        *is_queue = ((off - s2mm) % AIEML_DMA_CH_STRIDE) == AIEML_DMA_QUEUE_OFF;
        return true;
    }
    if (off >= mm2s && off < mm2s + nch * AIEML_DMA_CH_STRIDE) {
        *dir = AIE_DMA_MM2S;
        *ch = (off - mm2s) / AIEML_DMA_CH_STRIDE;
        *is_queue = ((off - mm2s) % AIEML_DMA_CH_STRIDE) == AIEML_DMA_QUEUE_OFF;
        return true;
    }
    return false;
}

uint32_t xdna_array_read32(XdnaArray *arr, uint8_t col, uint8_t row,
                           uint32_t off)
{
    AieTile *t = xdna_array_tile(arr, col, row);
    uint32_t lock_base, val_base, bd_base;
    int dir;
    uint32_t ch;
    bool is_queue;

    if (!t) {
        xdna_log(arr->npu, XDNA_LOG_WARN, "array: gecersiz tile (%u,%u)", col,
                 row);
        return 0;
    }
    off &= AIE_TILE_OFF_MASK;

    /* Lock istegi: OKUMA islemi tetikler (aie-rt MaskPoll ile yapar). */
    lock_base = tile_lock_request_base(t);
    if (off >= lock_base &&
        off < lock_base + tile_num_locks(t) * AIEML_LOCK_ID_STRIDE) {
        uint32_t rel = off - lock_base;
        uint32_t id = rel / AIEML_LOCK_ID_STRIDE;
        uint32_t within = rel % AIEML_LOCK_ID_STRIDE;
        bool acquire = within >= AIEML_LOCK_RELACQ_OFF;
        int32_t v;

        if (acquire) {
            within -= AIEML_LOCK_RELACQ_OFF;
        }
        v = lock_val_decode(within >> AIEML_LOCK_VALUE_SHIFT);
        arr->stats.lock_ops++;
        return lock_op(arr->npu, t, id, v, acquire);
    }

    /* Lock deger registerlari */
    val_base = tile_lock_value_base(t);
    if (off >= val_base &&
        off < val_base + tile_num_locks(t) * AIEML_LOCK_VALUE_STRIDE) {
        uint32_t id = (off - val_base) / AIEML_LOCK_VALUE_STRIDE;
        return (uint32_t)t->lock[id];
    }

    /* Tile bellegi */
    if (t->data && off < t->data_size) {
        uint32_t v;
        memcpy(&v, t->data + off, 4);
        return v;
    }
    if (t->prog && off >= AIEML_CORE_PROG_MEM &&
        off < AIEML_CORE_PROG_MEM + t->prog_size) {
        uint32_t v;
        memcpy(&v, t->prog + (off - AIEML_CORE_PROG_MEM), 4);
        return v;
    }

    /* BD registerlari */
    bd_base = tile_bd_base(t);
    if (off >= bd_base && off < bd_base + tile_num_bds(t) * AIE_BD_STRIDE) {
        uint32_t rel = off - bd_base;
        return t->bd[rel / AIE_BD_STRIDE][(rel % AIE_BD_STRIDE) / 4u];
    }

    /* DMA kanal registerlari */
    if (decode_dma_reg(t, off, &dir, &ch, &is_queue)) {
        return is_queue ? t->ch_queue[dir][ch] : t->ch_ctrl[dir][ch];
    }

    /* Stream switch */
    {
        uint32_t mbase = tile_ss_master_base(t);
        uint32_t sbase = tile_ss_slave_base(t);
        uint32_t nm, ns;

        (void)master_ports(t->kind, &nm);
        (void)slave_ports(t->kind, &ns);
        if (off >= mbase && off < mbase + nm * 4u) {
            return t->ss_master[(off - mbase) / 4u];
        }
        if (off >= sbase && off < sbase + ns * 4u) {
            return t->ss_slave[(off - sbase) / 4u];
        }
    }

    if (t->kind == AIE_TILE_SHIM) {
        if (off == AIEML_SHIM_MUX_CONFIG) {
            return t->shim_mux;
        }
        if (off == AIEML_SHIM_DEMUX_CONFIG) {
            return t->shim_demux;
        }
    }

    if (t->kind == AIE_TILE_CORE) {
        if (off == AIEML_CORE_CONTROL) {
            return t->core_ctrl;
        }
        if (off == AIEML_CORE_STATUS) {
            return t->core_status;
        }
    }

    return regmap_get(&t->regs, off);
}

void xdna_array_write32(XdnaArray *arr, uint8_t col, uint8_t row, uint32_t off,
                        uint32_t val)
{
    AieTile *t = xdna_array_tile(arr, col, row);
    uint32_t lock_base, val_base, bd_base;
    int dir;
    uint32_t ch;
    bool is_queue;

    if (!t) {
        xdna_log(arr->npu, XDNA_LOG_WARN, "array: gecersiz tile (%u,%u)", col,
                 row);
        return;
    }
    off &= AIE_TILE_OFF_MASK;

    /* Lock deger registerlari: dogrudan atama (lock init). */
    val_base = tile_lock_value_base(t);
    if (off >= val_base &&
        off < val_base + tile_num_locks(t) * AIEML_LOCK_VALUE_STRIDE) {
        uint32_t id = (off - val_base) / AIEML_LOCK_VALUE_STRIDE;
        t->lock[id] = lock_val_decode(val);
        return;
    }

    lock_base = tile_lock_request_base(t);
    if (off >= lock_base &&
        off < lock_base + tile_num_locks(t) * AIEML_LOCK_ID_STRIDE) {
        /* Lock islemleri okuma ile yapilir; yazma bir sey ifade etmez. */
        xdna_log(arr->npu, XDNA_LOG_DEBUG,
                 "array: lock request bolgesine yazma (offset 0x%x)", off);
        return;
    }

    if (t->data && off < t->data_size) {
        memcpy(t->data + off, &val, 4);
        return;
    }
    if (t->prog && off >= AIEML_CORE_PROG_MEM &&
        off < AIEML_CORE_PROG_MEM + t->prog_size) {
        memcpy(t->prog + (off - AIEML_CORE_PROG_MEM), &val, 4);
        return;
    }

    bd_base = tile_bd_base(t);
    if (off >= bd_base && off < bd_base + tile_num_bds(t) * AIE_BD_STRIDE) {
        uint32_t rel = off - bd_base;
        t->bd[rel / AIE_BD_STRIDE][(rel % AIE_BD_STRIDE) / 4u] = val;
        return;
    }

    if (decode_dma_reg(t, off, &dir, &ch, &is_queue)) {
        if (is_queue) {
            /*
             * Kuyruga yazmak gorevi baslatir. Gercek donanimda bu asenkron;
             * emulatorde senkron yurutuyoruz (bkz. qemu/README.md).
             */
            t->ch_queue[dir][ch] = val;
            dma_run_channel(arr, t, dir, ch);
        } else {
            t->ch_ctrl[dir][ch] = val;
        }
        return;
    }

    {
        uint32_t mbase = tile_ss_master_base(t);
        uint32_t sbase = tile_ss_slave_base(t);
        uint32_t nm, ns;

        (void)master_ports(t->kind, &nm);
        (void)slave_ports(t->kind, &ns);
        if (off >= mbase && off < mbase + nm * 4u) {
            t->ss_master[(off - mbase) / 4u] = val;
            return;
        }
        if (off >= sbase && off < sbase + ns * 4u) {
            t->ss_slave[(off - sbase) / 4u] = val;
            return;
        }
    }

    if (t->kind == AIE_TILE_SHIM) {
        if (off == AIEML_SHIM_MUX_CONFIG) {
            t->shim_mux = val;
            return;
        }
        if (off == AIEML_SHIM_DEMUX_CONFIG) {
            t->shim_demux = val;
            return;
        }
    }

    if (t->kind == AIE_TILE_CORE) {
        if (off == AIEML_CORE_CONTROL) {
            t->core_ctrl = val;
            if (val & AIE_CORE_CTRL_RESET_MASK) {
                t->core_status = 0;
            }
            if (val & AIE_CORE_CTRL_ENABLE_MASK) {
                /*
                 * Compute tile'i "calistir". Instruction interpreter'i
                 * (yol haritasi asama 7) henuz yok; core enable edilmis
                 * gorunuyor ama program yurutulmuyor. Bunu sessizce
                 * gecmiyoruz, sayacini tutuyoruz.
                 */
                t->core_status |= AIE_CORE_CTRL_ENABLE_MASK;
                arr->stats.core_starts++;
                xdna_log(arr->npu, XDNA_LOG_WARN,
                         "array: tile(%u,%u) core enable -- AIE interpreter "
                         "yok, program yurutulmedi", t->col, t->row);
            }
            return;
        }
        if (off == AIEML_CORE_STATUS) {
            t->core_status = val;
            return;
        }
    }

    regmap_set(&t->regs, off, val);
}

void xdna_array_block_write(XdnaArray *arr, uint8_t col, uint8_t row,
                            uint32_t off, const uint32_t *data, uint32_t words)
{
    uint32_t i;

    for (i = 0; i < words; i++) {
        xdna_array_write32(arr, col, row, off + i * 4u, data[i]);
    }
}
