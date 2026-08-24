// SPDX-License-Identifier: GPL-2.0-only
/*
 * XDNA1 emulatoru -- cihaz govdesi ve MMIO yonlendirmesi.
 *
 * BAR0 (REG/PSP/SMU): register semantigi. Yazma tetikleyicileri:
 *   PUB_SEC_INTR     -> PSP komut calistirma
 *   PUB_PWRMGMT_INTR -> SMU komut calistirma
 * BAR2 (SRAM): duz bellek + firmware'in yazdigi yapilar.
 * BAR4 (MBOX): kanal head/tail/interrupt registerlari; x2i tail yazimi
 *              mesaj islemeyi tetikler.
 */

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "xdna_internal.h"

void xdna_log(XdnaNpu *npu, int level, const char *fmt, ...)
{
    char buf[512];
    va_list ap;

    if (!npu->ops->log) {
        return;
    }
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    npu->ops->log(npu->opaque, level, buf);
}

uint32_t xdna_npu_bar_size(int bar)
{
    switch (bar) {
    case XDNA_BAR_REG:
        return XDNA_BAR_REG_SIZE;
    case XDNA_BAR_SRAM:
        return XDNA_BAR_SRAM_SIZE;
    case XDNA_BAR_MBOX:
        return XDNA_BAR_MBOX_SIZE;
    default:
        return 0;
    }
}

XdnaNpu *xdna_npu_new(const XdnaHostOps *ops, void *opaque)
{
    XdnaNpu *npu = calloc(1, sizeof(*npu));

    if (!npu) {
        return NULL;
    }
    npu->sram = calloc(1, XDNA_BAR_SRAM_SIZE);
    if (!npu->sram) {
        free(npu);
        return NULL;
    }
    npu->ops = ops;
    npu->opaque = opaque;
    xdna_npu_reset(npu);
    return npu;
}

void xdna_npu_free(XdnaNpu *npu)
{
    if (!npu) {
        return;
    }
    free(npu->sram);
    free(npu);
}

void xdna_npu_reset(XdnaNpu *npu)
{
    const XdnaHostOps *ops = npu->ops;
    void *opaque = npu->opaque;
    uint8_t *sram = npu->sram;

    memset(sram, 0, XDNA_BAR_SRAM_SIZE);
    memset(npu, 0, sizeof(*npu));
    npu->ops = ops;
    npu->opaque = opaque;
    npu->sram = sram;

    npu->psp_state = XDNA_PSP_COLD;
    npu->power_on = false;
    npu->fw_alive = false;
    npu->next_ctx_id = 1;

    /*
     * PSP soguk sifirlamadan sonra komut kabul etmeye hazirdir; surucu ilk
     * isi olarak STATUS register'inda READY bitini bekler (aie_psp.c
     * psp_exec ilk readx_poll_timeout).
     */
    npu->scratch[2] = PSP_STATUS_READY;

    xdna_mbox_reset(npu);
}

void xdna_npu_get_stats(const XdnaNpu *npu, XdnaStats *out)
{
    *out = npu->stats;
    out->fw_alive = npu->fw_alive;
    out->power_on = npu->power_on;
}

/* ---------------------------------------------------------------- */
/* REG BAR                                                           */
/* ---------------------------------------------------------------- */

static uint32_t reg_bar_read(XdnaNpu *npu, uint32_t off)
{
    switch (off) {
    case XDNA_REG_PWAITMODE:
        return npu->pwaitmode;
    case XDNA_REG_PUB_SEC_INTR:
        return npu->sec_intr;
    case XDNA_REG_PUB_PWRMGMT_IR:
        return npu->pwrmgmt_intr;
    case XDNA_REG_SCRATCH2:
        return npu->scratch[2];
    case XDNA_REG_SCRATCH3:
        return npu->scratch[3];
    case XDNA_REG_SCRATCH4:
        return npu->scratch[4];
    case XDNA_REG_SCRATCH5:
        return npu->scratch[5];
    case XDNA_REG_SCRATCH6:
        return npu->scratch[6];
    case XDNA_REG_SCRATCH7:
        return npu->scratch[7];
    case XDNA_REG_SCRATCH9:
        return npu->scratch[9];
    default:
        /*
         * Bilinmeyen register. Gercek donanimda buradan okunan degerler
         * suruculer tarafindan kullanilmiyor; 0 dondurmek probe'u
         * etkilemiyor. Yeni bir offset gorursek loglayip modele ekleriz.
         */
        xdna_log(npu, XDNA_LOG_DEBUG, "reg bar okunmayan offset 0x%x", off);
        return 0;
    }
}

static void reg_bar_write(XdnaNpu *npu, uint32_t off, uint32_t val)
{
    switch (off) {
    case XDNA_REG_PWAITMODE:
        npu->pwaitmode = val;
        return;
    case XDNA_REG_PUB_SEC_INTR:
        npu->sec_intr = val;
        if (val == PSP_NOTIFY_VAL) {
            xdna_psp_kick(npu);
        }
        return;
    case XDNA_REG_PUB_PWRMGMT_IR:
        npu->pwrmgmt_intr = val;
        if (val == 1) {
            xdna_smu_kick(npu);
        }
        return;
    case XDNA_REG_SCRATCH2:
        npu->scratch[2] = val;
        return;
    case XDNA_REG_SCRATCH3:
        npu->scratch[3] = val;
        return;
    case XDNA_REG_SCRATCH4:
        npu->scratch[4] = val;
        return;
    case XDNA_REG_SCRATCH5:
        npu->scratch[5] = val;
        return;
    case XDNA_REG_SCRATCH6:
        npu->scratch[6] = val;
        return;
    case XDNA_REG_SCRATCH7:
        npu->scratch[7] = val;
        return;
    case XDNA_REG_SCRATCH9:
        npu->scratch[9] = val;
        return;
    default:
        xdna_log(npu, XDNA_LOG_DEBUG, "reg bar yazilmayan offset 0x%x = 0x%x",
                 off, val);
        return;
    }
}

/* ---------------------------------------------------------------- */
/* Genel MMIO girisi                                                 */
/* ---------------------------------------------------------------- */

void xdna_npu_mmio_read_buf(XdnaNpu *npu, int bar, uint32_t off, void *buf,
                            uint32_t len)
{
    uint8_t *dst = buf;
    uint32_t i;

    if (bar == XDNA_BAR_SRAM) {
        if ((uint64_t)off + len > XDNA_BAR_SRAM_SIZE) {
            memset(dst, 0, len);
            return;
        }
        memcpy(dst, npu->sram + off, len);
        return;
    }

    for (i = 0; i < len; i += 4) {
        uint32_t v;
        uint32_t n = (len - i) < 4 ? (len - i) : 4;

        if (bar == XDNA_BAR_REG) {
            v = reg_bar_read(npu, off + i);
        } else if (bar == XDNA_BAR_MBOX) {
            v = xdna_mbox_reg_read(npu, off + i);
        } else {
            v = 0;
        }
        memcpy(dst + i, &v, n);
    }
}

void xdna_npu_mmio_write_buf(XdnaNpu *npu, int bar, uint32_t off,
                             const void *buf, uint32_t len)
{
    const uint8_t *src = buf;
    uint32_t i;

    if (bar == XDNA_BAR_SRAM) {
        if ((uint64_t)off + len > XDNA_BAR_SRAM_SIZE) {
            return;
        }
        memcpy(npu->sram + off, src, len);
        return;
    }

    for (i = 0; i < len; i += 4) {
        uint32_t v = 0;
        uint32_t n = (len - i) < 4 ? (len - i) : 4;

        memcpy(&v, src + i, n);
        if (bar == XDNA_BAR_REG) {
            reg_bar_write(npu, off + i, v);
        } else if (bar == XDNA_BAR_MBOX) {
            xdna_mbox_reg_write(npu, off + i, v);
        }
    }
}

uint64_t xdna_npu_mmio_read(XdnaNpu *npu, int bar, uint32_t off, unsigned len)
{
    uint64_t val = 0;

    if (len > sizeof(val)) {
        len = sizeof(val);
    }
    xdna_npu_mmio_read_buf(npu, bar, off, &val, len);
    return val;
}

void xdna_npu_mmio_write(XdnaNpu *npu, int bar, uint32_t off, uint64_t val,
                         unsigned len)
{
    if (len > sizeof(val)) {
        len = sizeof(val);
    }
    xdna_npu_mmio_write_buf(npu, bar, off, &val, len);
}
