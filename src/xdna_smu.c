// SPDX-License-Identifier: GPL-2.0-only
/*
 * SMU (System Management Unit) davranis modeli -- guc ve saat kontrolu.
 *
 * Protokol -- aie_smu.c aie_smu_exec():
 *   RESP(SCRATCH6) <- 0
 *   ARG(SCRATCH7)  <- arg
 *   CMD(SCRATCH5)  <- cmd
 *   INTR(PUB_PWRMGMT_INTR) <- 0, sonra 1
 *   RESP sifir disi olana kadar beklenir; SMU_RESULT_OK(1) disi deger hata.
 *   Cikti OUT(SCRATCH7) registerindan okunur -- ARG ile ayni register.
 */

#include "xdna_internal.h"

#define SMU_RESULT_FAIL 2u

static void smu_complete(XdnaNpu *npu, uint32_t resp, uint32_t out)
{
    npu->scratch[7] = out;    /* OUT (== ARG) */
    npu->scratch[6] = resp;   /* RESP          */
}

void xdna_smu_kick(XdnaNpu *npu)
{
    uint32_t cmd = npu->scratch[5];
    uint32_t arg = npu->scratch[7];

    npu->stats.smu_cmds++;

    switch (cmd) {
    case AIE_SMU_POWER_ON:
        npu->power_on = true;
        xdna_log(npu, XDNA_LOG_INFO, "SMU: NPU guc acildi");
        smu_complete(npu, SMU_RESULT_OK, 0);
        return;

    case AIE_SMU_POWER_OFF:
        npu->power_on = false;
        xdna_log(npu, XDNA_LOG_INFO, "SMU: NPU guc kapandi");
        smu_complete(npu, SMU_RESULT_OK, 0);
        return;

    case AIE_SMU_SET_MPNPUCLK_FREQ:
        /*
         * Gercek SMU istenen frekansi DPM tablosuna gore kirpar ve gercek
         * degeri OUT'tan dondurur. npu1_dpm_clk_table[] icindeki degerler
         * zaten gecerli oldugundan istegi aynen kabul ediyoruz.
         */
        npu->npuclk = arg;
        smu_complete(npu, SMU_RESULT_OK, arg);
        return;

    case AIE_SMU_SET_HCLK_FREQ:
        npu->hclk = arg;
        smu_complete(npu, SMU_RESULT_OK, arg);
        return;

    case AIE_SMU_SET_SOFT_DPMLEVEL:
    case AIE_SMU_SET_HARD_DPMLEVEL:
        if (arg > XDNA_NPU1_MAX_DPM_LEVEL) {
            xdna_log(npu, XDNA_LOG_ERROR, "SMU: gecersiz DPM seviyesi %u", arg);
            smu_complete(npu, SMU_RESULT_FAIL, 0);
            return;
        }
        npu->dpm_level = arg;
        smu_complete(npu, SMU_RESULT_OK, arg);
        return;

    default:
        xdna_log(npu, XDNA_LOG_ERROR, "SMU: bilinmeyen komut 0x%x", cmd);
        smu_complete(npu, SMU_RESULT_FAIL, 0);
        return;
    }
}
