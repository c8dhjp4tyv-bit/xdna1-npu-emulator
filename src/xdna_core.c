// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compute tile yurutme cekirdegi -- AIE2 (AIE-ML) VLIW islemcisi.
 *
 * DURUM: instruction seti henuz UYGULANMADI. Bu dosya yurutme iskeletini
 * ve paket uzunlugu cozucusunu iceriyor. Core enable edildiginde emulator
 * program bellegini gercekten getiriyor, paketi dogru siniriyor ve
 * calistiramadigi ilk instruction'da ERROR_HALT ile duruyor -- PC'yi ve
 * durumu gercek registerlarda gosteriyor.
 *
 * Bu, "core enable edildi ama hicbir sey olmadi" durumundan cok daha
 * durust: hangi PC'de, hangi boyutta bir paketle karsilastigimizi
 * soyluyor ve surucuye gercek bir instruction hatasi bildiriyor.
 *
 * ---------------------------------------------------------------------
 * Paket uzunlugu kodlamasi -- Xilinx/llvm-aie
 *   llvm/lib/Target/AIE/aie2/AIE2CompositeFormats.td
 *
 * AIE2 degisken uzunluklu bir VLIW: paket boyutu, ilk kelimenin DUSUK
 * bitlerindeki onek koduyla belirtiliyor.
 *
 *   class AIE2_instr128_Composite ... let Inst = {instr128, 0b0};    Size 16
 *   class AIE2_instr48_Composite  ... let Inst = {instr48,  0b101};  Size 6
 *   class AIE2_instr16_Composite  ... let Inst = {instr16, .., 0b0001}; Size 2
 *   class AIE2_instr64_Composite  ... let Inst = {instr64,  0b0011}; Size 8
 *   class AIE2_instr96_Composite  ... let Inst = {instr96,  0b0111}; Size 12
 *   class AIE2_instr32_Composite  ... let Inst = {instr32,  0b1001}; Size 4
 *   class AIE2_instr80_Composite  ... let Inst = {instr80,  0b1011}; Size 10
 *   class AIE2_instr112_Composite ... let Inst = {instr112, 0b1111}; Size 14
 *
 * Bu tam bir onek kodu: bit0 = 0 ise 128 bit; degilse dusuk 3 bit 0b101
 * ise 48 bit; degilse dusuk 4 bit boyutu veriyor. Tum kombinasyonlar
 * kapsaniyor, bosluk yok.
 */

#include <string.h>

#include "xdna_internal.h"

uint32_t xdna_aie2_packet_size(uint32_t first_word)
{
    if ((first_word & 0x1u) == 0u) {
        return 16u;                 /* instr128 */
    }
    if ((first_word & 0x7u) == 0x5u) {
        return 6u;                  /* instr48  */
    }
    switch (first_word & 0xFu) {
    case 0x1u: return 2u;           /* instr16  */
    case 0x3u: return 8u;           /* instr64  */
    case 0x7u: return 12u;          /* instr96  */
    case 0x9u: return 4u;           /* instr32  */
    case 0xBu: return 10u;          /* instr80  */
    case 0xFu: return 14u;          /* instr112 */
    default:
        /*
         * Ulasilamaz: yukaridaki kontroller tum bit desenlerini kapsiyor.
         * Yine de savunma amacli 0 donuyoruz.
         */
        return 0u;
    }
}

/* Core durum bitleri -- xaiemlgbl_params.h CORE_STATUS alanlari */
#define AIE_CORE_STATUS_DEBUG_HALT  (1u << 16)
#define AIE_CORE_STATUS_ERROR_HALT  (1u << 19)
#define AIE_CORE_STATUS_CORE_DONE   (1u << 20)

/*
 * Core'u calistir. Instruction seti uygulanmadigi icin ilk paketi
 * getirip siniriyor ve ERROR_HALT ile duruyoruz.
 */
void xdna_core_run(XdnaArray *arr, AieTile *t)
{
    XdnaNpu *npu = arr->npu;
    uint32_t word = 0;
    uint32_t size;

    if (t->kind != AIE_TILE_CORE || !t->prog) {
        return;
    }

    if (t->core_pc + 4u > t->prog_size) {
        xdna_log(npu, XDNA_LOG_ERROR,
                 "core(%u,%u): PC 0x%x program bellegi disinda", t->col,
                 t->row, t->core_pc);
        t->core_status |= AIE_CORE_STATUS_ERROR_HALT;
        xdna_async_error(npu, t->col, t->row, t->kind, XDNA_ERR_INSTRUCTION);
        return;
    }

    memcpy(&word, t->prog + t->core_pc, 4);
    size = xdna_aie2_packet_size(word);
    arr->stats.core_fetches++;

    if (!size || t->core_pc + size > t->prog_size) {
        xdna_log(npu, XDNA_LOG_ERROR,
                 "core(%u,%u): PC 0x%x gecersiz paket (ilk kelime 0x%08x, "
                 "boyut %u)",
                 t->col, t->row, t->core_pc, word, size);
        t->core_status |= AIE_CORE_STATUS_ERROR_HALT;
        xdna_async_error(npu, t->col, t->row, t->kind, XDNA_ERR_INSTRUCTION);
        return;
    }

    /*
     * Paket dogru siniriandi ama slot'lari cozup calistiramiyoruz.
     * Sessizce ilerlemek (PC += size) yanlis olurdu: program calismamis
     * olmasina ragmen calismis gibi gorunurdu. Bunun yerine gercek
     * donanimin yaptigini yapiyoruz: ERROR_HALT.
     */
    xdna_log(npu, XDNA_LOG_WARN,
             "core(%u,%u): PC 0x%x, %u baytlik AIE2 paketi cozuldu ama "
             "instruction seti uygulanmadi -- ERROR_HALT",
             t->col, t->row, t->core_pc, size);

    t->core_status |= AIE_CORE_STATUS_ERROR_HALT;
    t->core_status &= ~AIE_CORE_STATUS_CORE_DONE;
    arr->stats.core_halts++;
    xdna_async_error(npu, t->col, t->row, t->kind, XDNA_ERR_INSTRUCTION);
}
