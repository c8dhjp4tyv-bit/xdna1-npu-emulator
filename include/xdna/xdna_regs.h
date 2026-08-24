/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * XDNA1 (AMD Phoenix / Hawk Point NPU) donanim arayuz sabitleri.
 *
 * Bu dosyadaki degerlerin tamami, Linux cekirdegindeki stock `amdxdna`
 * surucusunun herkese acik kaynagindan dogrulanmistir. Her blogun basinda
 * kaynak dosya belirtilmistir. Kamuya acik kaynakta bulunmayan, bizim
 * sectigimiz veya tahmin ettigimiz degerler acikca "SECIM" veya
 * "TODO(dogrula)" olarak isaretlenmistir.
 *
 * Kaynaklar (torvalds/linux, drivers/accel/amdxdna/):
 *   npu1_regs.c, aie.h, aie_psp.c, aie_smu.c,
 *   aie2_pci.c, aie2_pci.h, amdxdna_mailbox.c, aie2_msg_priv.h,
 *   amdxdna_pci_drv.c
 */

#ifndef XDNA_REGS_H
#define XDNA_REGS_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/* PCI kimligi -- amdxdna_pci_drv.c: { 0x1502, 0x0, &dev_npu1_info }    */
/* ------------------------------------------------------------------ */
#define XDNA_PCI_VENDOR_ID      0x1022  /* PCI_VENDOR_ID_AMD */
#define XDNA_PCI_DEVICE_ID_NPU1 0x1502
#define XDNA_PCI_REVISION_NPU1  0x00
/* dev_npu1_info.default_vbnv */
#define XDNA_VBNV_NPU1          "RyzenAI-npu1"
/* dev_npu1_info.fw_path -> /lib/firmware/amdnpu/1502_00/npu.sbin */
#define XDNA_FW_PATH_NPU1       "amdnpu/1502_00/"

/* ------------------------------------------------------------------ */
/* Aperture tabanlari -- npu1_regs.c                                    */
/* ------------------------------------------------------------------ */
#define MPNPU_APERTURE0_BASE    0x3000000u  /* REG / PSP / SMU */
#define MPNPU_APERTURE1_BASE    0x3080000u  /* SRAM            */
#define MPNPU_APERTURE2_BASE    0x30C0000u  /* MAILBOX         */

/* BAR indeksleri -- npu1_regs.c (NPU1_*_BAR_INDEX) */
#define XDNA_BAR_REG            0
#define XDNA_BAR_SRAM           2
#define XDNA_BAR_MBOX           4
#define XDNA_BAR_PSP            XDNA_BAR_REG
#define XDNA_BAR_SMU            XDNA_BAR_REG

/*
 * BAR boyutlari. Surucu bunlari donanimdan okur (pci_resource_len), yani
 * degeri biz belirliyoruz. Tek sert kisit: kullanilan en yuksek offset BAR
 * icinde kalmali ve SRAM BAR'i mailbox ring buffer'lari tasiyabilmeli.
 * SECIM: aperture'lar arasi mesafeye gore secildi.
 */
#define XDNA_BAR_REG_SIZE       0x80000u   /* 512 KiB, aperture0..aperture1 */
#define XDNA_BAR_SRAM_SIZE      0x40000u   /* 256 KiB, aperture1..aperture2 */
#define XDNA_BAR_MBOX_SIZE      0x10000u   /* 64 KiB                        */

/* ------------------------------------------------------------------ */
/* REG BAR register offsetleri (BAR0 basina gore) -- npu1_regs.c        */
/* ------------------------------------------------------------------ */
#define REG_OFF(addr)           ((addr) - MPNPU_APERTURE0_BASE)

#define XDNA_REG_PWAITMODE      REG_OFF(0x3010034u)
#define XDNA_REG_PUB_SEC_INTR   REG_OFF(0x3010090u)
#define XDNA_REG_PUB_PWRMGMT_IR REG_OFF(0x3010094u)
#define XDNA_REG_SCRATCH2       REG_OFF(0x30100A0u)
#define XDNA_REG_SCRATCH3       REG_OFF(0x30100A4u)
#define XDNA_REG_SCRATCH4       REG_OFF(0x30100A8u)
#define XDNA_REG_SCRATCH5       REG_OFF(0x30100ACu)
#define XDNA_REG_SCRATCH6       REG_OFF(0x30100B0u)
#define XDNA_REG_SCRATCH7       REG_OFF(0x30100B4u)
#define XDNA_REG_SCRATCH9       REG_OFF(0x30100BCu)

/*
 * PSP register eslemesi -- npu1_regs.c psp_regs_off[].
 * DIKKAT: CMD ile STATUS ayni register (SCRATCH2), ARG0 ile RESP ayni
 * register (SCRATCH3). Emulatorun bu ortakligi dogru modellemesi sart.
 */
#define XDNA_PSP_CMD_REG        XDNA_REG_SCRATCH2
#define XDNA_PSP_ARG0_REG       XDNA_REG_SCRATCH3
#define XDNA_PSP_ARG1_REG       XDNA_REG_SCRATCH4
#define XDNA_PSP_ARG2_REG       XDNA_REG_SCRATCH9
#define XDNA_PSP_INTR_REG       XDNA_REG_PUB_SEC_INTR
#define XDNA_PSP_STATUS_REG     XDNA_REG_SCRATCH2
#define XDNA_PSP_RESP_REG       XDNA_REG_SCRATCH3
#define XDNA_PSP_PWAITMODE_REG  XDNA_REG_PWAITMODE

/* SMU register eslemesi -- npu1_regs.c smu_regs_off[] */
#define XDNA_SMU_CMD_REG        XDNA_REG_SCRATCH5
#define XDNA_SMU_ARG_REG        XDNA_REG_SCRATCH7
#define XDNA_SMU_INTR_REG       XDNA_REG_PUB_PWRMGMT_IR
#define XDNA_SMU_RESP_REG       XDNA_REG_SCRATCH6
#define XDNA_SMU_OUT_REG        XDNA_REG_SCRATCH7  /* ARG ile ayni register */

/* ------------------------------------------------------------------ */
/* PSP protokolu -- aie_psp.c                                           */
/* ------------------------------------------------------------------ */
#define PSP_STATUS_READY        (1u << 31)
#define PSP_CMD_VALIDATE        1u
#define PSP_CMD_START           2u
#define PSP_CMD_RELEASE_TMR     3u
#define PSP_CMD_VALIDATE_CERT   4u
#define PSP_START_COPY_FW       1u
#define PSP_ERROR_CANCEL        0xFFFF0002u
#define PSP_ERROR_BAD_STATE     0xFFFF0007u
/* aie2_pci.c: psp_conf.arg2_mask / notify_val */
#define PSP_ARG2_MASK           0x00FFFFFFu
#define PSP_NOTIFY_VAL          1u
/* aie_psp.c: PSP_SET_CMD -> arg2 = (size | cmd << 24) & arg2_mask */
#define PSP_ARG2_CMD_SHIFT      24

/* ------------------------------------------------------------------ */
/* SMU protokolu -- aie_smu.c                                           */
/* ------------------------------------------------------------------ */
#define SMU_RESULT_OK               1u
#define AIE_SMU_POWER_ON            0x3u
#define AIE_SMU_POWER_OFF           0x4u
#define AIE_SMU_SET_MPNPUCLK_FREQ   0x5u
#define AIE_SMU_SET_HCLK_FREQ       0x6u
#define AIE_SMU_SET_SOFT_DPMLEVEL   0x7u
#define AIE_SMU_SET_HARD_DPMLEVEL   0x8u

/* npu1_dpm_clk_table[] -- npu1_regs.c (npuclk, hclk) */
#define XDNA_NPU1_MAX_DPM_LEVEL     7

/* ------------------------------------------------------------------ */
/* SRAM BAR offsetleri -- npu1_regs.c sram_offs[]                       */
/* ------------------------------------------------------------------ */
#define SRAM_OFF(addr)          ((addr) - MPNPU_APERTURE1_BASE)

/* MPNPU_SRAM_X2I_MAILBOX_0  = 0x30A0000 */
#define XDNA_SRAM_MBOX_CHANN_OFF SRAM_OFF(0x30A0000u)
/* MPNPU_SRAM_I2X_MAILBOX_15 = 0x30BF000 */
#define XDNA_SRAM_FW_ALIVE_OFF   SRAM_OFF(0x30BF000u)

/*
 * Firmware'in SRAM icindeki yerlesimi. Surucu bu adreslerin hicbirini
 * sabit kabul etmez: mgmt kanal bilgisini FW_ALIVE_OFF'taki pointer'dan,
 * ring buffer adreslerini de o yapinin icinden okur. SECIM: gercek
 * donanimin yerlesimine yakin duracak sekilde secildi.
 */
#define XDNA_SRAM_MGMT_INFO_OFF  XDNA_SRAM_MBOX_CHANN_OFF /* 0x20000 */
#define XDNA_SRAM_MGMT_X2I_OFF   0x21000u
#define XDNA_SRAM_MGMT_I2X_OFF   0x22000u
#define XDNA_SRAM_MGMT_RB_SIZE   0x1000u
/* Kullanici (hwctx) kanallari icin ring alani */
#define XDNA_SRAM_CTX_RB_BASE    0x24000u
#define XDNA_SRAM_CTX_RB_SIZE    0x2000u   /* aie2_pci.h: CHAN_SLOT_SZ = 8K */
#define XDNA_SRAM_CTX_RB_STRIDE  (2u * XDNA_SRAM_CTX_RB_SIZE)

/* ------------------------------------------------------------------ */
/* MBOX BAR yerlesimi                                                   */
/* ------------------------------------------------------------------ */
/*
 * Head/tail register adreslerini de firmware bildirir, dolayisiyla yerlesim
 * bizim secimimiz. TEK SERT KISIT (aie2_pci.c:387):
 *     xdna_mailbox_intr_reg = i2x.mb_head_ptr_reg + 4
 * yani i2x head register'inin hemen ardindaki word, kanalin interrupt
 * status register'i olmak zorundadir.
 */
#define XDNA_MBOX_CHAN_STRIDE    0x20u
#define XDNA_MBOX_X2I_HEAD_OFF   0x00u
#define XDNA_MBOX_X2I_TAIL_OFF   0x04u
#define XDNA_MBOX_I2X_HEAD_OFF   0x08u
#define XDNA_MBOX_INTR_OFF       0x0Cu  /* == I2X_HEAD + 4, zorunlu */
#define XDNA_MBOX_I2X_TAIL_OFF   0x10u

#define XDNA_MBOX_CHAN_BASE(idx) ((idx) * XDNA_MBOX_CHAN_STRIDE)

/* ------------------------------------------------------------------ */
/* Mailbox mesaj cercevesi -- amdxdna_mailbox.c                         */
/* ------------------------------------------------------------------ */
#define XDNA_MBOX_MAGIC_VAL      0x1D000000u
#define XDNA_MBOX_MAGIC_MASK     0xFF000000u
#define XDNA_MBOX_TOMBSTONE      0xDEADFACEu
#define XDNA_MSG_PROTOCOL_VER    0x1u
#define XDNA_MSG_BODY_SZ_MASK    0x7FFu       /* GENMASK(10, 0)  */
#define XDNA_MSG_PROTO_VER_SHIFT 16           /* GENMASK(23, 16) */

/* aie2_pci.c: MGMT_MBOX_MAGIC "_NPU" */
#define XDNA_MGMT_MBOX_MAGIC     0x55504E5Fu

/* ------------------------------------------------------------------ */
/* Firmware / protokol surumu                                           */
/* ------------------------------------------------------------------ */
/*
 * npu1_fw_feature_table[]: major 5, min_minor 7 taban destek; 5.8 ile
 * AIE2_NPU_COMMAND acilir. 5.7 bildirmek feature_mask'i bos birakir ve
 * suruculeri en sade calistirma yoluna sokar -- ilk asama icin istedigimiz
 * budur. aie_check_protocol() bu tabloya gore -EOPNOTSUPP dondurur.
 */
#define XDNA_MGMT_PROT_MAJOR     5u
#define XDNA_MGMT_PROT_MINOR     7u

/* Emulatorun bildirdigi firmware surumu. SECIM. */
#define XDNA_FW_VER_MAJOR        5u
#define XDNA_FW_VER_MINOR        7u
#define XDNA_FW_VER_SUB          0u
#define XDNA_FW_VER_BUILD        0u

/* ------------------------------------------------------------------ */
/* Cihaz bellegi -- aie2_pci.h                                          */
/* ------------------------------------------------------------------ */
#define AIE2_DEVM_BASE           0x4000000u
#define AIE2_DEVM_SIZE           (64u * 1024u * 1024u)

/* dev_npu1_info: first_col = 1, dev_priv.hwctx_limit = 6, col_opc = 2048 */
#define XDNA_NPU1_FIRST_COL      1u
#define XDNA_NPU1_HWCTX_LIMIT    6u
#define XDNA_NPU1_COL_OPC        2048u

/* ------------------------------------------------------------------ */
/* MERT mesaj opcode'lari -- aie2_msg_priv.h                            */
/* ------------------------------------------------------------------ */
enum xdna_msg_opcode {
    MSG_OP_CREATE_CONTEXT           = 0x2,
    MSG_OP_DESTROY_CONTEXT          = 0x3,
    MSG_OP_GET_TELEMETRY            = 0x4,
    MSG_OP_SYNC_BO                  = 0x7,
    MSG_OP_EXECUTE_BUFFER_CF        = 0xC,
    MSG_OP_QUERY_COL_STATUS         = 0xD,
    MSG_OP_QUERY_AIE_TILE_INFO      = 0xE,
    MSG_OP_QUERY_AIE_VERSION        = 0xF,
    MSG_OP_EXEC_DPU                 = 0x10,
    MSG_OP_CONFIG_CU                = 0x11,
    MSG_OP_CHAIN_EXEC_BUFFER_CF     = 0x12,
    MSG_OP_CHAIN_EXEC_DPU           = 0x13,
    MSG_OP_CONFIG_DEBUG_BO          = 0x14,
    MSG_OP_CHAIN_EXEC_NPU           = 0x18,
    MSG_OP_SUSPEND                  = 0x101,
    MSG_OP_RESUME                   = 0x102,
    MSG_OP_ASSIGN_MGMT_PASID        = 0x103,
    MSG_OP_INVOKE_SELF_TEST         = 0x104,
    MSG_OP_MAP_HOST_BUFFER          = 0x106,
    MSG_OP_GET_FIRMWARE_VERSION     = 0x108,
    MSG_OP_SET_RUNTIME_CONFIG       = 0x10A,
    MSG_OP_GET_RUNTIME_CONFIG       = 0x10B,
    MSG_OP_REGISTER_ASYNC_EVENT_MSG = 0x10C,
    MSG_OP_UPDATE_PROPERTY          = 0x113,
    MSG_OP_GET_APP_HEALTH           = 0x114,
    MSG_OP_ADD_HOST_BUFFER          = 0x115,
    MSG_OP_GET_DEV_REVISION         = 0x117,
    MSG_OP_GET_PROTOCOL_VERSION     = 0x301,
};

/* MERT durum kodlari -- aie2_msg_priv.h (enum aie2_msg_status) */
enum xdna_msg_status {
    AIE2_STATUS_SUCCESS                 = 0x0,
    AIE2_STATUS_AIE_INSTRUCTION_ERROR   = 0x1000006,
    AIE2_STATUS_AIE_LOCK_ERROR          = 0x1000008,
    AIE2_STATUS_AIE_DMA_ERROR           = 0x1000009,
    AIE2_STATUS_MGMT_ERT_NOAVAIL        = 0x2000003,
    AIE2_STATUS_MGMT_ERT_INVALID_PARAM  = 0x2000004,
    AIE2_STATUS_MGMT_ERT_BUSY           = 0x2000006,
    AIE2_STATUS_APP_INVALID_INSTR       = 0x3000002,
    AIE2_STATUS_INVALID_INPUT_BUFFER    = 0x4000001,
    AIE2_STATUS_INVALID_COMMAND         = 0x4000002,
    AIE2_STATUS_INVALID_PARAM           = 0x4000003,
    AIE2_STATUS_INVALID_OPERATION       = 0x4000006,
};

/* ------------------------------------------------------------------ */
/* XDNA1 array topolojisi                                               */
/* ------------------------------------------------------------------ */
/*
 * TODO(dogrula): Bu degerler AMD'nin acik XDNA dokumantasyonundan ve
 * Phoenix/Hawk Point icin bilinen 5 kolon x 4 compute tile topolojisinden
 * turetildi. QUERY_AIE_TILE_INFO cevabinda kullaniliyorlar ve surucunun
 * metadata.cols alanini besliyorlar. Gercek bir Hawk Point'te
 * `xrt-smi examine` / ioctl ciktisiyla birebir karsilastirilmalidir.
 */
#define XDNA_AIE_COLS            5u
#define XDNA_AIE_CORE_ROWS       4u
#define XDNA_AIE_MEM_ROWS        1u
#define XDNA_AIE_SHIM_ROWS       1u
#define XDNA_AIE_ROWS            (XDNA_AIE_SHIM_ROWS + XDNA_AIE_MEM_ROWS + \
                                  XDNA_AIE_CORE_ROWS)
#define XDNA_AIE_SHIM_ROW_START  0u
#define XDNA_AIE_MEM_ROW_START   1u
#define XDNA_AIE_CORE_ROW_START  2u
#define XDNA_AIE_VERSION_MAJOR   2u  /* AIE-ML / AIE2 */
#define XDNA_AIE_VERSION_MINOR   0u

#endif /* XDNA_REGS_H */
