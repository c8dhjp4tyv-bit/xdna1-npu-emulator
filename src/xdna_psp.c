// SPDX-License-Identifier: GPL-2.0-only
/*
 * PSP (Platform Security Processor) davranis modeli.
 *
 * Gercek donanimda PSP, imzali NPU firmware'ini (npu.sbin) host belleginden
 * alir, dogrular ve NPU mikrodenetleyicisine yukler. Biz firmware'i komut
 * seviyesinde SEMANTIK olarak emule ediyoruz: imaji gercekten host
 * bellegindin okuyup kabul ediyor, ardindan MERT davranis modelini
 * ayaga kaldiriyoruz. Firmware komutlari native calismiyor; gozlemlenebilir
 * davranis ayni.
 *
 * Protokol -- aie_psp.c psp_exec():
 *   1. surucu STATUS(SCRATCH2) icinde READY(bit31) bekler
 *   2. CMD/ARG0/ARG1/ARG2 registerlarini yazar
 *   3. INTR(PUB_SEC_INTR) <- 0, sonra notify_val(1)
 *   4. STATUS icinde tekrar READY bekler
 *   5. RESP(SCRATCH3) okur; sifir disi deger hata demektir
 *
 * DIKKAT: CMD == STATUS == SCRATCH2 ve ARG0 == RESP == SCRATCH3.
 */

#include <string.h>

#include "xdna_internal.h"

/* PSP'nin firmware imajindan gercekten okudugu bayt sayisi (dogrulama). */
#define PSP_PEEK_BYTES 64

static void psp_complete(XdnaNpu *npu, uint32_t resp)
{
    npu->scratch[3] = resp;                 /* RESP */
    npu->scratch[2] = PSP_STATUS_READY;     /* STATUS: islem bitti */
}

static uint32_t psp_fetch_image(XdnaNpu *npu, uint64_t paddr, uint32_t size)
{
    uint8_t peek[PSP_PEEK_BYTES];
    uint32_t n = size < PSP_PEEK_BYTES ? size : PSP_PEEK_BYTES;

    if (!size) {
        return PSP_ERROR_BAD_STATE;
    }
    /*
     * PSP fiziksel adresle calisir (surucu virt_to_phys() veriyor), bu
     * yuzden IOMMU'dan gecmeyen yolu tercih ediyoruz.
     */
    int (*rd)(void *, uint64_t, void *, size_t) =
        npu->ops->phys_read ? npu->ops->phys_read : npu->ops->dma_read;

    if (!rd || rd(npu->opaque, paddr, peek, n) != 0) {
        xdna_log(npu, XDNA_LOG_ERROR,
                 "PSP firmware imaji okunamadi: paddr 0x%llx size 0x%x",
                 (unsigned long long)paddr, size);
        return PSP_ERROR_CANCEL;
    }
    return 0;
}

void xdna_psp_kick(XdnaNpu *npu)
{
    uint32_t cmd = npu->scratch[2];
    uint32_t arg0 = npu->scratch[3];
    uint32_t arg1 = npu->scratch[4];
    uint32_t arg2 = npu->scratch[9];
    uint64_t paddr = ((uint64_t)arg1 << 32) | arg0;
    uint32_t size = arg2 & PSP_ARG2_MASK;
    uint32_t resp;

    npu->stats.psp_cmds++;

    switch (cmd) {
    case PSP_CMD_VALIDATE:
        resp = psp_fetch_image(npu, paddr, size);
        if (!resp) {
            npu->fw_paddr = paddr;
            npu->fw_size = size;
            npu->psp_state = XDNA_PSP_VALIDATED;
            xdna_log(npu, XDNA_LOG_INFO,
                     "PSP: firmware dogrulandi, paddr 0x%llx size 0x%x",
                     (unsigned long long)paddr, size);
        }
        break;

    case PSP_CMD_VALIDATE_CERT:
        /* CERT firmware istege bagli; imaj okunabiliyorsa kabul ediyoruz. */
        resp = psp_fetch_image(npu, paddr, size);
        break;

    case PSP_CMD_START:
        if (npu->psp_state != XDNA_PSP_VALIDATED) {
            xdna_log(npu, XDNA_LOG_ERROR,
                     "PSP: START oncesi VALIDATE yapilmamis");
            resp = PSP_ERROR_BAD_STATE;
            break;
        }
        if (arg0 != PSP_START_COPY_FW) {
            xdna_log(npu, XDNA_LOG_WARN, "PSP: beklenmeyen START arg0 0x%x",
                     arg0);
        }
        npu->psp_state = XDNA_PSP_RUNNING;
        xdna_fw_boot(npu);
        resp = 0;
        break;

    case PSP_CMD_RELEASE_TMR:
        xdna_fw_shutdown(npu);
        npu->psp_state = XDNA_PSP_COLD;
        resp = 0;
        break;

    default:
        xdna_log(npu, XDNA_LOG_ERROR, "PSP: bilinmeyen komut 0x%x", cmd);
        resp = PSP_ERROR_BAD_STATE;
        break;
    }

    psp_complete(npu, resp);
}
