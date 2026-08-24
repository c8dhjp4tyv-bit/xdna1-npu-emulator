// SPDX-License-Identifier: GPL-2.0-only
/*
 * Mailbox ring buffer protokolu -- cihaz tarafi.
 *
 * Cerceve (amdxdna_mailbox.c):
 *   [ total_size | sz_ver | id | opcode ][ payload (total_size bayt) ]
 * Ring buffer'lar SRAM BAR'inda, head/tail registerlari MBOX BAR'inda durur.
 *
 * Yon adlandirmasi surucununkiyle ayni:
 *   x2i = host -> firmware  (surucu tail yazar, cihaz head ilerletir)
 *   i2x = firmware -> host  (cihaz tail ilerletir, surucu head yazar)
 *
 * Surucunun yazma yolu, ring sonuna sigmayan mesajdan once TOMBSTONE
 * (0xDEADFACE) yazip basa doner; okuma yolumuz ayni kurali uygular.
 */

#include <string.h>

#include "xdna_internal.h"

#define MBOX_MAX_MSG_PER_KICK 256

void xdna_mbox_channel_reset(XdnaNpu *npu, unsigned idx)
{
    if (idx >= XDNA_MAX_CHANNELS) {
        return;
    }
    memset(&npu->chan[idx], 0, sizeof(npu->chan[idx]));
    npu->chan[idx].index = idx;
}

void xdna_mbox_reset(XdnaNpu *npu)
{
    unsigned i;

    for (i = 0; i < XDNA_MAX_CHANNELS; i++) {
        xdna_mbox_channel_reset(npu, i);
    }
}

static XdnaChannel *chan_from_off(XdnaNpu *npu, uint32_t off, uint32_t *reg)
{
    unsigned idx = off / XDNA_MBOX_CHAN_STRIDE;

    if (idx >= XDNA_MAX_CHANNELS) {
        return NULL;
    }
    *reg = off % XDNA_MBOX_CHAN_STRIDE;
    return &npu->chan[idx];
}

uint32_t xdna_mbox_reg_read(XdnaNpu *npu, uint32_t off)
{
    uint32_t reg;
    XdnaChannel *ch = chan_from_off(npu, off, &reg);

    if (!ch) {
        return 0;
    }
    switch (reg) {
    case XDNA_MBOX_X2I_HEAD_OFF:
        return ch->x2i_head;
    case XDNA_MBOX_X2I_TAIL_OFF:
        return ch->x2i_tail;
    case XDNA_MBOX_I2X_HEAD_OFF:
        return ch->i2x_head;
    case XDNA_MBOX_INTR_OFF:
        return ch->intr;
    case XDNA_MBOX_I2X_TAIL_OFF:
        return ch->i2x_tail;
    default:
        return 0;
    }
}

void xdna_mbox_reg_write(XdnaNpu *npu, uint32_t off, uint32_t val)
{
    uint32_t reg;
    XdnaChannel *ch = chan_from_off(npu, off, &reg);

    if (!ch) {
        return;
    }
    switch (reg) {
    case XDNA_MBOX_X2I_TAIL_OFF:
        ch->x2i_tail = val;
        xdna_mbox_process(npu, ch->index);
        return;
    case XDNA_MBOX_I2X_HEAD_OFF:
        ch->i2x_head = val;
        return;
    case XDNA_MBOX_INTR_OFF:
        ch->intr = val;
        return;
    case XDNA_MBOX_X2I_HEAD_OFF:
    case XDNA_MBOX_I2X_TAIL_OFF:
        /* Cihaz tarafinin sahibi oldugu registerlar; surucu yazmaz. */
        xdna_log(npu, XDNA_LOG_WARN,
                 "mailbox: cihaz register'ina yazma denemesi off 0x%x", off);
        return;
    default:
        return;
    }
}

/* ---------------------------------------------------------------- */
/* x2i: surucuden gelen mesajlari tuket                              */
/* ---------------------------------------------------------------- */

void xdna_mbox_process(XdnaNpu *npu, unsigned chan_idx)
{
    XdnaChannel *ch;
    unsigned guard = 0;

    if (chan_idx >= XDNA_MAX_CHANNELS) {
        return;
    }
    ch = &npu->chan[chan_idx];
    if (!ch->active) {
        xdna_log(npu, XDNA_LOG_WARN,
                 "mailbox: pasif kanal %u uzerinde mesaj", chan_idx);
        return;
    }

    while (ch->x2i_head != ch->x2i_tail) {
        XdnaMsgHeader hdr;
        uint8_t payload[XDNA_SRAM_CTX_RB_SIZE];
        uint32_t peek, msg_size;

        if (++guard > MBOX_MAX_MSG_PER_KICK) {
            xdna_log(npu, XDNA_LOG_ERROR, "mailbox: kanal %u dongude sikisti",
                     chan_idx);
            return;
        }
        if (ch->x2i_head >= ch->x2i_size) {
            ch->x2i_head = 0;
            continue;
        }

        peek = xdna_sram_read32(npu, ch->x2i_buf + ch->x2i_head);
        if (peek == XDNA_MBOX_TOMBSTONE) {
            ch->x2i_head = 0;
            continue;
        }
        if (!peek || (peek & 3) != 0) {
            xdna_log(npu, XDNA_LOG_ERROR,
                     "mailbox: kanal %u gecersiz mesaj boyutu 0x%x",
                     chan_idx, peek);
            return;
        }

        msg_size = (uint32_t)sizeof(hdr) + peek;
        if (msg_size > ch->x2i_size - ch->x2i_head) {
            xdna_log(npu, XDNA_LOG_ERROR,
                     "mailbox: kanal %u mesaj ring sinirini asiyor", chan_idx);
            return;
        }
        if (peek > sizeof(payload)) {
            xdna_log(npu, XDNA_LOG_ERROR,
                     "mailbox: kanal %u payload cok buyuk (0x%x)", chan_idx,
                     peek);
            return;
        }

        memcpy(&hdr, npu->sram + ch->x2i_buf + ch->x2i_head, sizeof(hdr));
        memcpy(payload, npu->sram + ch->x2i_buf + ch->x2i_head + sizeof(hdr),
               peek);

        ch->x2i_head += msg_size;
        npu->stats.mbox_msgs_in++;

        xdna_mert_handle(npu, chan_idx, &hdr, payload);
    }
}

/* ---------------------------------------------------------------- */
/* i2x: surucuye mesaj uret                                          */
/* ---------------------------------------------------------------- */

int xdna_mbox_send(XdnaNpu *npu, unsigned chan_idx, uint32_t id,
                   uint32_t opcode, const void *payload, uint32_t len)
{
    XdnaMsgHeader hdr;
    XdnaChannel *ch;
    uint32_t usable, tail, head, pkg_size;

    if (chan_idx >= XDNA_MAX_CHANNELS) {
        return -1;
    }
    ch = &npu->chan[chan_idx];
    if (!ch->active) {
        return -1;
    }

    pkg_size = (uint32_t)sizeof(hdr) + len;
    /* Surucunun yazma yolundaki ile ayni hesap: son word tombstone icin. */
    usable = ch->i2x_size - (uint32_t)sizeof(uint32_t);
    if (pkg_size > usable) {
        xdna_log(npu, XDNA_LOG_ERROR, "mailbox: cevap ring'e sigmiyor (%u)",
                 pkg_size);
        return -1;
    }

    head = ch->i2x_head;
    tail = ch->i2x_tail;

    if (tail >= head && tail + pkg_size > usable) {
        xdna_sram_write32(npu, ch->i2x_buf + tail, XDNA_MBOX_TOMBSTONE);
        tail = 0;
    }
    if (tail < head && tail + pkg_size >= head) {
        xdna_log(npu, XDNA_LOG_ERROR, "mailbox: kanal %u i2x ring dolu",
                 chan_idx);
        return -1;
    }

    hdr.total_size = len;
    hdr.sz_ver = (len & XDNA_MSG_BODY_SZ_MASK) |
                 (XDNA_MSG_PROTOCOL_VER << XDNA_MSG_PROTO_VER_SHIFT);
    hdr.id = id;
    hdr.opcode = opcode;

    memcpy(npu->sram + ch->i2x_buf + tail, &hdr, sizeof(hdr));
    if (len) {
        memcpy(npu->sram + ch->i2x_buf + tail + sizeof(hdr), payload, len);
    }

    ch->i2x_tail = tail + pkg_size;
    ch->intr = 1;
    npu->stats.mbox_msgs_out++;

    if (npu->ops->raise_irq) {
        npu->ops->raise_irq(npu->opaque, ch->index);
        npu->stats.irqs_raised++;
    }
    return 0;
}
