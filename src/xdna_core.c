// SPDX-License-Identifier: GPL-2.0-only
/*
 * Compute tile yurutme cekirdegi -- AIE2 (AIE-ML) VLIW islemcisi.
 *
 * DURUM: cozme (decode) katmani var, YURUTME (semantik) yok.
 *
 * Uygulanan:
 *   - paket uzunlugu cozucusu (asagida),
 *   - bundle slot cozucusu: paketin hangi composite formatta oldugunu
 *     bulup ldb/lda/st/alu/mv/lng/vec alanlarini cikariyor.
 *
 * Uygulanmayan: slot iceriklerinin ne anlama geldigi -- yani AIE2
 * instruction setinin kendisi. Core enable edildiginde emulator program
 * bellegini gercekten getiriyor, paketi siniriyor, slot'lara ayiriyor ve
 * yurutemedigi icin ERROR_HALT ile duruyor; PC ve durum gercek
 * registerlarda okunabiliyor.
 *
 * Bu, "core enable edildi ama hicbir sey olmadi" durumundan cok daha
 * durust: hangi PC'de, hangi formatta, hangi slotlari olan bir paketle
 * karsilastigimizi soyluyor ve surucuye gercek bir instruction hatasi
 * bildiriyor.
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
#include "aie2_formats.h"

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

/* ------------------------------------------------------------------ */
/* Bundle slot cozucusu                                                */
/* ------------------------------------------------------------------ */
/*
 * Slot yerlesimi src/aie2_formats.h icindeki tablodan geliyor; tablo
 * tools/gen-aie2-formats.py ile llvm-aie'nin AIE2CompositeFormats.td
 * dosyasindan uretiliyor. Ureteci, ayni boyuttaki formatlarin sabit
 * bitlerinin birbirinden ayirt edilebildigini dogruluyor, dolayisiyla
 * asagidaki dogrusal arama en fazla bir formatla eslesir.
 */

static const char *const slot_names[AIE2_SLOT_KINDS] = {
    "lda", "ldb", "st", "alu", "mv", "lng", "vec", "nop",
};

const char *xdna_aie2_slot_name(Aie2SlotKind kind)
{
    if ((unsigned)kind >= AIE2_SLOT_KINDS) {
        return "?";
    }
    return slot_names[kind];
}

/* Little endian bayt dizisinden [hi:lo] bit araligini oku. */
static uint64_t bits_get(const uint8_t *b, unsigned hi, unsigned lo)
{
    uint64_t v = 0;
    unsigned i;

    for (i = 0; i <= hi - lo; i++) {
        unsigned bit = lo + i;

        if (b[bit >> 3] & (1u << (bit & 7u))) {
            v |= (uint64_t)1 << i;
        }
    }
    return v;
}

static bool format_matches(const Aie2Format *f, const uint8_t *b)
{
    unsigned i;

    for (i = 0; i < f->size; i++) {
        if ((b[i] & f->mask[i]) != f->value[i]) {
            return false;
        }
    }
    return true;
}

int xdna_aie2_decode(const uint8_t *bytes, size_t avail, Aie2Bundle *out)
{
    uint32_t word = 0;
    uint32_t size;
    size_t i;

    memset(out, 0, sizeof(*out));

    if (avail < 2) {
        return -1;
    }
    memcpy(&word, bytes, avail < 4 ? avail : 4);
    size = xdna_aie2_packet_size(word);
    if (!size || size > avail) {
        return -1;
    }
    out->size = size;

    for (i = 0; i < AIE2_FORMAT_COUNT; i++) {
        const Aie2Format *f = &aie2_formats[i];
        unsigned s;

        if (f->size != size || !format_matches(f, bytes)) {
            continue;
        }

        out->format = f->name;
        out->nslots = f->nslots;
        for (s = 0; s < f->nslots; s++) {
            out->slot[s].kind = (Aie2SlotKind)f->slots[s].kind;
            out->slot[s].width =
                (uint8_t)(f->slots[s].hi - f->slots[s].lo + 1u);
            out->slot[s].value = bits_get(bytes, f->slots[s].hi,
                                          f->slots[s].lo);
        }
        return 0;
    }

    /* Hicbir format eslesmedi: bu gecerli bir AIE2 kodlamasi degil. */
    return -1;
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
    Aie2Bundle bundle;
    uint32_t word = 0;
    uint32_t size;
    unsigned i;

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
     * Paket siniriandi; simdi slot'lari cikar. Bundle cozumu basarisiz
     * olursa desen gecerli bir AIE2 kodlamasi degil demektir.
     */
    if (xdna_aie2_decode(t->prog + t->core_pc, t->prog_size - t->core_pc,
                         &bundle) != 0) {
        xdna_log(npu, XDNA_LOG_ERROR,
                 "core(%u,%u): PC 0x%x, %u baytlik pakete uyan AIE2 composite "
                 "formati yok (ilk kelime 0x%08x)",
                 t->col, t->row, t->core_pc, size, word);
        t->core_status |= AIE_CORE_STATUS_ERROR_HALT;
        arr->stats.core_halts++;
        xdna_async_error(npu, t->col, t->row, t->kind, XDNA_ERR_INSTRUCTION);
        return;
    }

    /*
     * Slot'lar cozuldu ama semantikleri uygulanmadi: hangi slotun hangi
     * instruction'i tasidigini bilmiyoruz. Sessizce ilerlemek (PC += size)
     * yanlis olurdu: program calismamis olmasina ragmen calismis gibi
     * gorunurdu. Bunun yerine gercek donanimin gecersiz bir instruction'da
     * yaptigini yapiyoruz: ERROR_HALT.
     */
    for (i = 0; i < bundle.nslots; i++) {
        xdna_log(npu, XDNA_LOG_DEBUG, "core(%u,%u):   slot %s[%u] = 0x%llx",
                 t->col, t->row, xdna_aie2_slot_name(bundle.slot[i].kind),
                 bundle.slot[i].width,
                 (unsigned long long)bundle.slot[i].value);
    }

    xdna_log(npu, XDNA_LOG_WARN,
             "core(%u,%u): PC 0x%x, %u baytlik AIE2 paketi %s formatinda "
             "%u slota cozuldu ama instruction semantikleri uygulanmadi "
             "-- ERROR_HALT",
             t->col, t->row, t->core_pc, size, bundle.format, bundle.nslots);

    t->core_status |= AIE_CORE_STATUS_ERROR_HALT;
    t->core_status &= ~AIE_CORE_STATUS_CORE_DONE;
    arr->stats.core_halts++;
    xdna_async_error(npu, t->col, t->row, t->kind, XDNA_ERR_INSTRUCTION);
}
