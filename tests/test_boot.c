// SPDX-License-Identifier: GPL-2.0-only
/*
 * Stock amdxdna surucusunun boot dizisini birebir taklit eden test kosumu.
 *
 * Surucu modeli tests/drv_model.c icinde; buradaki testler yalnizca MMIO
 * uzerinden emulatoru suruyor.
 *
 * Bu test gecerse: emulator, stock surucunun probe yolunu bastan sona
 * gecirebiliyor demektir (yol haritasi asama 2 ve 4).
 */

#include <stdlib.h>
#include <string.h>

#include "drv_model.h"

static void test_boot(Host *host, XdnaNpu *npu)
{
    Drv drv = { .npu = npu, .host = host, .next_id = 1 };
    Drv *d = &drv;
    StatusResp st;
    FwVerResp fw;
    AieVerResp av;
    TileInfoResp ti;
    CreateCtxResp cctx;
    uint32_t clk;

    step("SMU guc dizisi + PSP firmware yukleme");
    CHECK(drv_hw_start(d) == 0, "hw_start basarisiz");
    if (g_failures) {
        return;
    }

    step("Yonetim kanali ve protokol surumu");
    CHECK_EQ(d->prot_major, XDNA_MGMT_PROT_MAJOR, "protokol major");
    CHECK(d->prot_minor >= 7, "protokol minor >= 7 olmali (npu1 feature tbl)");
    CHECK_EQ(d->x2i.rb_size, XDNA_SRAM_MGMT_RB_SIZE, "x2i ring boyutu");
    CHECK_EQ(d->intr_reg, d->i2x.head_reg + 4, "interrupt register konumu");
    CHECK_EQ(rd32(d, XDNA_BAR_PSP, XDNA_PSP_PWAITMODE_REG) & 1, 1,
             "pwaitmode bit0");

    step("aie2_mgmt_fw_init: runtime config");
    {
        /* npu1_default_rt_cfg[] INIT kategorisi: {2,1} ve {4,1} */
        SetRtCfgReq req = { .type = 2, .value = 1 };
        CHECK(mbox_send_recv(d, MSG_OP_SET_RUNTIME_CONFIG, &req, sizeof(req),
                             &st, sizeof(st)) == 0, "rt cfg 2 gonderilemedi");
        CHECK_EQ(st.status, 0, "rt cfg 2 durumu");
        req.type = 4;
        CHECK(mbox_send_recv(d, MSG_OP_SET_RUNTIME_CONFIG, &req, sizeof(req),
                             &st, sizeof(st)) == 0, "rt cfg 4 gonderilemedi");
        CHECK_EQ(st.status, 0, "rt cfg 4 durumu");
    }

    step("aie2_assign_mgmt_pasid");
    {
        PasidReq req = { .pasid = 0 };
        CHECK(mbox_send_recv(d, MSG_OP_ASSIGN_MGMT_PASID, &req, sizeof(req),
                             &st, sizeof(st)) == 0, "pasid gonderilemedi");
        CHECK_EQ(st.status, 0, "pasid durumu");
    }

    step("aie2_xdna_reset: SUSPEND + RESUME");
    {
        uint32_t ph = 0;
        CHECK(mbox_send_recv(d, MSG_OP_SUSPEND, &ph, sizeof(ph), &st,
                             sizeof(st)) == 0, "suspend gonderilemedi");
        CHECK_EQ(st.status, 0, "suspend durumu");
        CHECK(mbox_send_recv(d, MSG_OP_RESUME, &ph, sizeof(ph), &st,
                             sizeof(st)) == 0, "resume gonderilemedi");
        CHECK_EQ(st.status, 0, "resume durumu");
    }

    step("aie2_mgmt_fw_query: firmware / AIE surumu / tile bilgisi");
    {
        uint32_t rsv = 0;
        CHECK(mbox_send_recv(d, MSG_OP_GET_FIRMWARE_VERSION, &rsv, sizeof(rsv),
                             &fw, sizeof(fw)) == 0, "fw version");
        CHECK_EQ(fw.status, 0, "fw version durumu");
        CHECK_EQ(fw.major, XDNA_FW_VER_MAJOR, "fw major");

        CHECK(mbox_send_recv(d, MSG_OP_QUERY_AIE_VERSION, &rsv, sizeof(rsv),
                             &av, sizeof(av)) == 0, "aie version");
        CHECK_EQ(av.status, 0, "aie version durumu");

        CHECK(mbox_send_recv(d, MSG_OP_QUERY_AIE_TILE_INFO, &rsv, sizeof(rsv),
                             &ti, sizeof(ti)) == 0, "tile info");
        CHECK_EQ(ti.status, 0, "tile info durumu");
        CHECK_EQ(ti.info.cols, XDNA_AIE_COLS, "kolon sayisi");
        CHECK_EQ(ti.info.rows, XDNA_AIE_ROWS, "satir sayisi");
        CHECK_EQ(ti.info.core_rows, XDNA_AIE_CORE_ROWS, "compute satirlari");
    }

    step("SMU DPM / saat ayarlari");
    {
        clk = 400;
        CHECK(smu_exec(d, AIE_SMU_SET_MPNPUCLK_FREQ, clk, &clk) == 0,
              "mpnpuclk");
        CHECK_EQ(clk, 400, "mpnpuclk geri okuma");
        clk = 800;
        CHECK(smu_exec(d, AIE_SMU_SET_HCLK_FREQ, clk, &clk) == 0, "hclk");
        CHECK_EQ(clk, 800, "hclk geri okuma");
        CHECK(smu_exec(d, AIE_SMU_SET_HARD_DPMLEVEL, 7, NULL) == 0, "hard dpm");
        CHECK(smu_exec(d, AIE_SMU_SET_SOFT_DPMLEVEL, 7, NULL) == 0, "soft dpm");
        CHECK(smu_exec(d, AIE_SMU_SET_HARD_DPMLEVEL, 99, NULL) != 0,
              "gecersiz DPM seviyesi reddedilmeli");
    }

    step("Hardware context olustur / heap bagla / yok et");
    {
        CreateCtxReq req = {
            .aie_type = 1,
            .start_col = 0,
            .num_col = 4,
            .num_cq_pairs_requested = 1,
            .pasid = 13,
            .context_priority = 3,
        };
        MapBufReq map;
        DestroyCtxReq dreq;

        CHECK(mbox_send_recv(d, MSG_OP_CREATE_CONTEXT, &req, sizeof(req),
                             &cctx, sizeof(cctx)) == 0, "create context");
        CHECK_EQ(cctx.status, 0, "create context durumu");
        CHECK(cctx.context_id != 0, "context id sifir olmamali");
        CHECK_EQ(cctx.num_cq_pairs_allocated, 1, "cq pair sayisi");
        CHECK(cctx.msix_id != XDNA_MGMT_MSIX_VECTOR,
              "context MSI-X vektoru mgmt ile cakismamali");
        CHECK_EQ(cctx.cq_pair[0].i2x_q.head_addr + 4,
                 MPNPU_APERTURE2_BASE +
                     XDNA_MBOX_CHAN_BASE(cctx.msix_id) + XDNA_MBOX_INTR_OFF,
                 "context interrupt register konumu");
        CHECK(cctx.cq_pair[0].x2i_q.buf_addr >= MPNPU_APERTURE1_BASE,
              "x2i buffer SRAM aperture icinde olmali");

        map.context_id = cctx.context_id;
        map.buf_addr = HOST_MEM_BASE + 0x200000;
        map.buf_size = 0x100000;
        CHECK(mbox_send_recv(d, MSG_OP_MAP_HOST_BUFFER, &map, sizeof(map), &st,
                             sizeof(st)) == 0, "map host buffer");
        CHECK_EQ(st.status, 0, "map host buffer durumu");

        dreq.context_id = cctx.context_id;
        CHECK(mbox_send_recv(d, MSG_OP_DESTROY_CONTEXT, &dreq, sizeof(dreq),
                             &st, sizeof(st)) == 0, "destroy context");
        CHECK_EQ(st.status, 0, "destroy context durumu");
    }

    step("Context limiti (hwctx_limit = 6)");
    {
        CreateCtxReq req = {
            .aie_type = 1, .num_col = 1, .num_cq_pairs_requested = 1,
            .context_priority = 3,
        };
        uint32_t ids[XDNA_NPU1_HWCTX_LIMIT];
        unsigned i;

        for (i = 0; i < XDNA_NPU1_HWCTX_LIMIT; i++) {
            CHECK(mbox_send_recv(d, MSG_OP_CREATE_CONTEXT, &req, sizeof(req),
                                 &cctx, sizeof(cctx)) == 0, "ctx olusturma");
            CHECK_EQ(cctx.status, 0, "ctx durumu");
            ids[i] = cctx.context_id;
        }
        CHECK(mbox_send_recv(d, MSG_OP_CREATE_CONTEXT, &req, sizeof(req),
                             &cctx, sizeof(cctx)) == 0, "7. ctx cevabi");
        CHECK(cctx.status != 0, "7. context reddedilmeli");

        for (i = 0; i < XDNA_NPU1_HWCTX_LIMIT; i++) {
            DestroyCtxReq dreq = { .context_id = ids[i] };
            CHECK(mbox_send_recv(d, MSG_OP_DESTROY_CONTEXT, &dreq,
                                 sizeof(dreq), &st, sizeof(st)) == 0,
                  "ctx yok etme");
            CHECK_EQ(st.status, 0, "ctx yok etme durumu");
        }
    }

    step("Ring buffer sarmalanmasi (TOMBSTONE yolu, her iki yon)");
    {
        uint32_t rsv = 0;
        unsigned i;
        unsigned wraps_before = 0;

        (void)wraps_before;
        for (i = 0; i < 300; i++) {
            if (mbox_send_recv(d, MSG_OP_GET_FIRMWARE_VERSION, &rsv,
                               sizeof(rsv), &fw, sizeof(fw)) != 0) {
                printf("  [BASARISIZ] %u. mesajda sarmalama hatasi\n", i);
                break;
            }
            if (fw.status != 0) {
                CHECK_EQ(fw.status, 0, "sarmalama sirasinda fw durumu");
                break;
            }
        }
        CHECK(i == 300, "300 mesajin tamami islenmeli (islenen: %u)", i);
    }

    step("Array asamasi henuz yok: exec mesajlari acikca reddedilmeli");
    {
        uint8_t req[80] = { 0 };
        CHECK(mbox_send_recv(d, MSG_OP_EXECUTE_BUFFER_CF, req, sizeof(req),
                             &st, sizeof(st)) == 0, "execbuf cevabi");
        CHECK(st.status != 0, "execbuf sessizce basarili donmemeli");
    }
}

static void test_error_paths(Host *host, XdnaNpu *npu)
{
    Drv drv = { .npu = npu, .host = host, .next_id = 1 };
    Drv *d = &drv;

    step("VALIDATE yapilmadan START reddedilmeli");
    xdna_npu_reset(npu);
    CHECK(psp_exec(d, PSP_CMD_START, PSP_START_COPY_FW, 0, 0) != 0,
          "dogrulanmamis firmware baslatilmamali");
    CHECK_EQ(rd32(d, XDNA_BAR_SRAM, XDNA_SRAM_FW_ALIVE_OFF), 0,
             "firmware alive olmamali");

    step("Erisilemeyen firmware adresi reddedilmeli");
    CHECK(psp_exec(d, PSP_CMD_VALIDATE, 0xDEAD0000u, 0xFFFFu, 0x1000) != 0,
          "gecersiz firmware adresi kabul edilmemeli");

    step("Suspend/resume dongusu: RELEASE_TMR sonrasi yeniden boot");
    xdna_npu_reset(npu);
    CHECK(drv_hw_start(d) == 0, "ilk boot");
    CHECK(psp_exec(d, PSP_CMD_RELEASE_TMR, 0, 0, 0) == 0, "release tmr");
    CHECK_EQ(rd32(d, XDNA_BAR_SRAM, XDNA_SRAM_FW_ALIVE_OFF), 0,
             "release sonrasi fw alive temizlenmeli");
    CHECK(smu_exec(d, AIE_SMU_POWER_OFF, 0, NULL) == 0, "power off");

    d->next_id = 1;
    CHECK(drv_hw_start(d) == 0, "ikinci boot");
    {
        uint32_t rsv = 0;
        FwVerResp fw;
        CHECK(mbox_send_recv(d, MSG_OP_GET_FIRMWARE_VERSION, &rsv, sizeof(rsv),
                             &fw, sizeof(fw)) == 0, "yeniden boot sonrasi fw");
        CHECK_EQ(fw.status, 0, "yeniden boot sonrasi fw durumu");
    }
}

int main(int argc, char **argv)
{
    Host *host;
    XdnaNpu *npu;

    host = host_new(argc > 1 && strcmp(argv[1], "-v") == 0);
    if (!host) {
        return 1;
    }
    npu = xdna_npu_new(&drv_host_ops, host);
    if (!npu) {
        host_free(host);
        return 1;
    }

    printf("=== XDNA1 emulatoru: stock amdxdna boot dizisi ===\n");
    test_boot(host, npu);

    printf("\n=== Hata yollari ve yeniden boot ===\n");
    test_error_paths(host, npu);

    {
        XdnaStats stats;
        xdna_npu_get_stats(npu, &stats);
        printf("\nIstatistik: psp=%llu smu=%llu mbox_in=%llu mbox_out=%llu "
               "irq=%llu\n",
               (unsigned long long)stats.psp_cmds,
               (unsigned long long)stats.smu_cmds,
               (unsigned long long)stats.mbox_msgs_in,
               (unsigned long long)stats.mbox_msgs_out,
               (unsigned long long)stats.irqs_raised);
    }

    xdna_npu_free(npu);
    host_free(host);

    printf("\n%d kontrol, %d basarisiz\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
