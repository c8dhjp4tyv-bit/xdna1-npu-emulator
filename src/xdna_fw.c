// SPDX-License-Identifier: GPL-2.0-only
/*
 * Firmware boot davranisi.
 *
 * Gercek donanimda PSP_START'tan sonra NPU mikrodenetleyicisi uzerinde MERT
 * ayaga kalkar, SRAM'e yonetim kanali tanimini yazar ve FW_ALIVE_OFF'a bu
 * yapinin cihaz adresini koyar. Surucu (aie2_get_mgmt_chann_info) once
 * FW_ALIVE_OFF'un sifir disi olmasini bekler, sonra oradan gosterilen
 * yapiyi okur ve magic degerini dogrular.
 *
 * Biz bu adimi firmware'i native calistirmadan, ayni gozlemlenebilir
 * sonucu uretecek sekilde yapiyoruz.
 */

#include <string.h>

#include "xdna_internal.h"

void xdna_fw_boot(XdnaNpu *npu)
{
    XdnaMgmtMboxInfo info;
    XdnaChannel *mgmt = &npu->chan[0];
    uint32_t chan_base = XDNA_MBOX_CHAN_BASE(0);

    /* Yonetim kanalini kur. */
    xdna_mbox_channel_reset(npu, 0);
    mgmt->active = true;
    mgmt->index = XDNA_MGMT_MSIX_VECTOR;
    mgmt->x2i_buf = XDNA_SRAM_MGMT_X2I_OFF;
    mgmt->x2i_size = XDNA_SRAM_MGMT_RB_SIZE;
    mgmt->i2x_buf = XDNA_SRAM_MGMT_I2X_OFF;
    mgmt->i2x_size = XDNA_SRAM_MGMT_RB_SIZE;

    memset(npu->sram + mgmt->x2i_buf, 0, mgmt->x2i_size);
    memset(npu->sram + mgmt->i2x_buf, 0, mgmt->i2x_size);

    memset(&info, 0, sizeof(info));
    info.x2i_head = xdna_mbox_dev_addr(chan_base + XDNA_MBOX_X2I_HEAD_OFF);
    info.x2i_tail = xdna_mbox_dev_addr(chan_base + XDNA_MBOX_X2I_TAIL_OFF);
    info.x2i_buf = xdna_sram_dev_addr(mgmt->x2i_buf);
    info.x2i_buf_sz = mgmt->x2i_size;
    info.i2x_head = xdna_mbox_dev_addr(chan_base + XDNA_MBOX_I2X_HEAD_OFF);
    info.i2x_tail = xdna_mbox_dev_addr(chan_base + XDNA_MBOX_I2X_TAIL_OFF);
    info.i2x_buf = xdna_sram_dev_addr(mgmt->i2x_buf);
    info.i2x_buf_sz = mgmt->i2x_size;
    info.magic = XDNA_MGMT_MBOX_MAGIC;
    info.msi_id = XDNA_MGMT_MSIX_VECTOR;
    info.prot_major = XDNA_MGMT_PROT_MAJOR;
    info.prot_minor = XDNA_MGMT_PROT_MINOR;

    memcpy(npu->sram + XDNA_SRAM_MGMT_INFO_OFF, &info, sizeof(info));

    /*
     * FW_ALIVE: yapinin CIHAZ adresi. Surucu bunu AIE2_SRAM_OFF() ile
     * tekrar BAR offsetine cevirir.
     */
    xdna_sram_write32(npu, XDNA_SRAM_FW_ALIVE_OFF,
                      xdna_sram_dev_addr(XDNA_SRAM_MGMT_INFO_OFF));

    npu->fw_alive = true;
    npu->fw_suspended = false;
    npu->pwaitmode |= 1u;  /* aie_psp_waitmode_poll() bit0 == 1 bekler */

    xdna_log(npu, XDNA_LOG_INFO,
             "MERT ayakta: protokol %u.%u, mgmt kanali MSI-X %u",
             XDNA_MGMT_PROT_MAJOR, XDNA_MGMT_PROT_MINOR,
             XDNA_MGMT_MSIX_VECTOR);
}

void xdna_fw_shutdown(XdnaNpu *npu)
{
    unsigned i;

    for (i = 0; i < XDNA_NPU1_HWCTX_LIMIT; i++) {
        npu->ctx[i].valid = false;
    }
    npu->col_used = 0;
    npu->stats.active_contexts = 0;
    npu->async_registered = false;

    xdna_mbox_reset(npu);
    xdna_sram_write32(npu, XDNA_SRAM_FW_ALIVE_OFF, 0);
    npu->fw_alive = false;
    npu->pwaitmode &= ~1u;

    xdna_log(npu, XDNA_LOG_INFO, "MERT durduruldu");
}
