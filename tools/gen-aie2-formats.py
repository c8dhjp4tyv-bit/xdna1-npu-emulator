#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
AIE2 (AIE-ML) VLIW bundle slot yerlesim tablosunu uretir.

Kaynak: Xilinx/llvm-aie
    llvm/lib/Target/AIE/aie2/AIE2CompositeFormats.td

Bu dosya AIE2'nin TUM composite (cok slotlu) instruction formatlarini
TableGen ile tanimliyor. Her format su uc seyi veriyor:

  * paket boyutu (2/4/6/8/10/12/14/16 bayt),
  * formati ayirt eden SABIT bitler (maske + deger),
  * her slotun bit araligi (ldb / lda / st / alu / mv / lng / vec / nop).

TableGen'de `let Inst = {a, b, 0b0101}` en anlamli bitten en az anlamliya
dogru bir birlestirme; `/*xxx01*/` bicimindeki yorumlar da hangi bitlerin
"onemsiz" oldugunu soyluyor. Bu betik o hiyerarsiyi cozup duz bir tabloya
ceviriyor.

Iki cikti:

  --table   src/aie2_formats.h        (C tablosu; llvm-mc gerekmez)
  --vectors tests/aie2_slot_vectors.h (differential test vektorleri;
                                       llvm-mc GEREKIR)

Vektor uretimi cozucuyu KENDI tablomuza degil, llvm-aie disassembler'ina
karsi dogruluyor:

  1. Her format icin llvm'in tam olarak 1 instruction olarak cozdugu
     gecerli bir paket bulunur.
  2. llvm'in yazdigi slot sayisi (";" ile ayrilmis) kaydedilir.
  3. Her slot icin, o slotun bit araligindan bir bit cevrilir ve llvm
     ciktisinda YALNIZCA bir slotun degistigi dogrulanir; degisen slotun
     indeksi kaydedilir.

C testi bu kayitlari kullanarak kendi cozucusunun ayni slotu degistirdigini
dogruluyor -- yani yerlesim, bizim tablomuzdan bagimsiz olarak sinaniyor.

Kullanim:
    python3 tools/gen-aie2-formats.py --table \\
        /path/to/llvm-aie/llvm/lib/Target/AIE/aie2/AIE2CompositeFormats.td \\
        > src/aie2_formats.h

    LLVM_MC=/path/to/llvm-mc python3 tools/gen-aie2-formats.py --vectors \\
        /path/to/AIE2CompositeFormats.td > tests/aie2_slot_vectors.h
"""

import os
import random
import re
import subprocess
import sys

MAX_SLOTS = 6

# Slot adi -> C enum adi. 'nop' ve 'nop16' ayni tek-bitlik dolgu slotu.
SLOT_KIND = {
    "lda": "AIE2_SLOT_LDA",
    "ldb": "AIE2_SLOT_LDB",
    "st": "AIE2_SLOT_ST",
    "alu": "AIE2_SLOT_ALU",
    "mv": "AIE2_SLOT_MV",
    "lng": "AIE2_SLOT_LNG",
    "vec": "AIE2_SLOT_VEC",
    "nop": "AIE2_SLOT_NOP",
    "nop16": "AIE2_SLOT_NOP",
}


# ---------------------------------------------------------------------
# TableGen ayristirma
# ---------------------------------------------------------------------

def parse_classes(src):
    out = {}
    pat = (r'^class\s+(\w+)<dag slot_ins>\s*(?::\s*(\w+)<slot_ins>)?\s*\n'
           r'\{(.*?)\n\}')
    for m in re.finditer(pat, src, re.S | re.M):
        out[m.group(1)] = (m.group(2), m.group(3))
    return out


def parse_body(body):
    widths = {}
    for m in re.finditer(r'bits<(\d+)>\s+(\w+)\s*;', body):
        widths[m.group(2)] = int(m.group(1))
    lets = {}
    for m in re.finditer(r'let\s+(\w+)\s*=\s*(.*?);', body, re.S):
        lets[m.group(1)] = m.group(2)
    size = int(lets["Size"].strip()) if "Size" in lets else None
    return widths, lets, size


def split_terms(expr):
    """{a, b, 0b0101 /*xx01*/} -> ['a', 'b', '0b0101 <care xx01>']"""
    expr = re.sub(r'/\*(.*?)\*/',
                  lambda m: '\x01' + m.group(1).strip() + '\x02',
                  expr, flags=re.S).strip()
    if not (expr.startswith('{') and expr.endswith('}')):
        return [expr]
    depth, cur, out = 0, '', []
    for ch in expr[1:-1]:
        if ch == '{':
            depth += 1
        elif ch == '}':
            depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip():
        out.append(cur.strip())
    return out


class Piece(object):
    def __init__(self, kind, name=None, width=0, value=0, mask=0):
        self.kind, self.name, self.width = kind, name, width
        self.value, self.mask = value, mask


def resolve(term, widths, lets):
    care = None
    if '\x01' in term:
        term, rest = term.split('\x01', 1)
        care = rest.split('\x02')[0].strip()
        term = term.strip()

    m = re.fullmatch(r'0b([01]+)', term)
    if m:
        bits = m.group(1)
        w = len(bits)
        val, mask = int(bits, 2), (1 << w) - 1
        if care is not None:
            if len(care) != w:
                raise ValueError("dontcare yorumu uyusmuyor: %r %r" % (bits, care))
            mask = int(''.join('0' if c == 'x' else '1' for c in care), 2)
            val &= mask
        return [Piece('fix', width=w, value=val, mask=mask)]

    m = re.fullmatch(r'(\w+)\{(\d+)-(\d+)\}', term)
    if m:                                    # dontcare{11-1}
        hi, lo = int(m.group(2)), int(m.group(3))
        return [Piece('fix', width=hi - lo + 1, value=0, mask=0)]

    if term in lets:
        out = []
        for t in split_terms(lets[term]):
            out += resolve(t, widths, lets)
        return out

    if term not in widths:
        raise ValueError("bilinmeyen terim: %r" % term)
    return [Piece('slot', name=term, width=widths[term])]


def parse_formats(path):
    classes = parse_classes(open(path).read())
    formats = []
    for name, (base, body) in classes.items():
        if not name.startswith('AIE2__instr'):
            continue
        widths, lets, _ = parse_body(body)
        bw, bl, size = parse_body(classes[base][1])
        widths = dict(bw, **widths)
        lets = dict(bl, **lets)

        pieces = []
        for t in split_terms(lets['Inst']):
            pieces += resolve(t, widths, lets)
        total = sum(p.width for p in pieces)
        if total != size * 8:
            raise ValueError("%s: %d bit, %d bekleniyordu" %
                             (name, total, size * 8))

        pos, slots, mask, value = total, [], 0, 0
        for p in pieces:
            pos -= p.width
            if p.kind == 'slot':
                slots.append((p.name, pos + p.width - 1, pos))
            else:
                mask |= p.mask << pos
                value |= p.value << pos
        if len(slots) > MAX_SLOTS:
            raise ValueError("%s: %d slot, en fazla %d" %
                             (name, len(slots), MAX_SLOTS))
        formats.append(dict(name=name, size=size, mask=mask, value=value,
                            slots=slots))

    formats.sort(key=lambda f: (f['size'], f['name']))
    check_disjoint(formats)
    return formats


def check_disjoint(formats):
    """Ayni boyuttaki formatlar birbirinden ayirt edilebilmeli."""
    for i, a in enumerate(formats):
        for b in formats[i + 1:]:
            if a['size'] != b['size']:
                continue
            common = a['mask'] & b['mask']
            if (a['value'] & common) == (b['value'] & common):
                raise ValueError("belirsiz format cifti: %s / %s" %
                                 (a['name'], b['name']))


# ---------------------------------------------------------------------
# C tablosu
# ---------------------------------------------------------------------

def bytes_of(v, size):
    return [(v >> (8 * i)) & 0xFF for i in range(size)]


def c_bytes(v, size):
    return ", ".join("0x%02x" % b for b in bytes_of(v, size))


def emit_table(formats):
    w = sys.stdout.write
    w("/* SPDX-License-Identifier: GPL-2.0-only */\n")
    w("/*\n")
    w(" * OTOMATIK URETILDI -- tools/gen-aie2-formats.py --table\n")
    w(" *\n")
    w(" * AIE2 composite (VLIW bundle) format tablosu. Kaynak:\n")
    w(" * Xilinx/llvm-aie llvm/lib/Target/AIE/aie2/AIE2CompositeFormats.td\n")
    w(" *\n")
    w(" * Elle DUZENLEME. Tum degerler TableGen'den cikarildi; ayni boyuttaki\n")
    w(" * formatlarin sabit bitlerinin birbirinden ayirt edilebildigi\n")
    w(" * ureticide dogrulaniyor.\n")
    w(" */\n\n")
    w("#ifndef AIE2_FORMATS_H\n#define AIE2_FORMATS_H\n\n")
    w("#include <stdint.h>\n\n")
    w("#define AIE2_FORMAT_MAX_SLOTS %du\n\n" % MAX_SLOTS)
    w("typedef struct {\n")
    w("    const char *name;\n")
    w("    uint8_t size;      /* paket boyutu, bayt */\n")
    w("    uint8_t nslots;\n")
    w("    uint8_t mask[16];  /* sabit bit maskesi (little endian) */\n")
    w("    uint8_t value[16]; /* sabit bit degerleri */\n")
    w("    struct {\n")
    w("        uint8_t kind;  /* Aie2SlotKind */\n")
    w("        uint8_t hi, lo;\n")
    w("    } slots[AIE2_FORMAT_MAX_SLOTS];\n")
    w("} Aie2Format;\n\n")
    w("static const Aie2Format aie2_formats[] = {\n")
    for f in formats:
        w("    { \"%s\", %d, %d,\n" % (f['name'], f['size'], len(f['slots'])))
        w("      { %s },\n" % c_bytes(f['mask'], f['size']))
        w("      { %s },\n" % c_bytes(f['value'], f['size']))
        w("      { %s } },\n" %
          ", ".join("{ %s, %d, %d }" % (SLOT_KIND[n], hi, lo)
                    for n, hi, lo in f['slots']))
    w("};\n\n")
    w("#define AIE2_FORMAT_COUNT "
      "(sizeof(aie2_formats) / sizeof(aie2_formats[0]))\n\n")
    w("#endif /* AIE2_FORMATS_H */\n")


# ---------------------------------------------------------------------
# llvm-aie differential vektorleri
# ---------------------------------------------------------------------

MC = os.environ.get("LLVM_MC", "llvm-mc")


def disasm(data):
    hx = " ".join("0x%02x" % b for b in data)
    r = subprocess.run([MC, "-disassemble", "-triple=aie2"],
                       input=hx, capture_output=True, text=True)
    lines = [l.strip() for l in r.stdout.splitlines() if l.strip()]
    bad = "invalid" in r.stderr.lower() or "error" in r.stderr.lower()
    return lines, bad


def printed_slots(line):
    return [s.strip() for s in line.split(";")]


def find_valid(fmt, rng, tries=600):
    size, mask, val = fmt['size'], fmt['mask'], fmt['value']
    free = ((1 << (size * 8)) - 1) & ~mask
    cands = [val] + [val | (rng.getrandbits(size * 8) & free)
                     for _ in range(tries)]
    for c in cands:
        lines, bad = disasm(bytes(bytes_of(c, size)))
        if len(lines) == 1 and not bad:
            return c, lines[0]
    return None, None


def find_flip(fmt, base, slot, rng, tries=64):
    """Slotun icinden, llvm ciktisinda TEK bir slotu degistiren bir bit bul."""
    size = fmt['size']
    _, hi, lo = slot
    ref, bad = disasm(bytes(bytes_of(base, size)))
    if bad or len(ref) != 1:
        return None
    ref_slots = printed_slots(ref[0])
    order = list(range(lo, hi + 1))
    rng.shuffle(order)
    for bit in order[:tries]:
        lines, bad = disasm(bytes(bytes_of(base ^ (1 << bit), size)))
        if bad or len(lines) != 1:
            continue
        got = printed_slots(lines[0])
        if len(got) != len(ref_slots):
            continue
        diff = [i for i in range(len(got)) if got[i] != ref_slots[i]]
        if len(diff) == 1:
            return bit, diff[0]
    return None


def emit_vectors(formats):
    rng = random.Random(0xA1E2)
    w = sys.stdout.write
    rows = []
    unresolved = []

    for f in formats:
        base, text = find_valid(f, rng)
        if base is None:
            print("UYARI: %s icin gecerli kodlama bulunamadi" % f['name'],
                  file=sys.stderr)
            unresolved.append(f['name'])
            continue
        llvm_slots = len(printed_slots(text))
        flips = []
        for slot in f['slots']:
            hit = find_flip(f, base, slot, rng)
            if hit is None:
                unresolved.append("%s.%s" % (f['name'], slot[0]))
                continue
            flips.append(hit)
        rows.append((f, base, text, llvm_slots, flips))

    w("/* SPDX-License-Identifier: GPL-2.0-only */\n")
    w("/*\n")
    w(" * OTOMATIK URETILDI -- tools/gen-aie2-formats.py --vectors\n")
    w(" *\n")
    w(" * AIE2 bundle slot yerlesimi icin differential test vektorleri.\n")
    w(" * Her satir Xilinx/llvm-aie disassembler'inin (aie2 hedefi) TEK bir\n")
    w(" * instruction olarak cozdugu gercek bir pakettir.\n")
    w(" *\n")
    w(" *   llvm_slots : llvm'in yazdigi \";\" ile ayrilmis slot sayisi.\n")
    w(" *   flips      : (bit, slot indeksi) ciftleri. Paketteki o bit\n")
    w(" *                cevrildiginde llvm ciktisinda YALNIZCA verilen\n")
    w(" *                indeksteki slot degisiyor.\n")
    w(" *\n")
    w(" * Bu kayitlar bizim tablomuzdan bagimsizdir: C testi kendi\n")
    w(" * cozucusunun ayni slotu degistirdigini dogruluyor.\n")
    w(" */\n\n")
    w("#ifndef AIE2_SLOT_VECTORS_H\n#define AIE2_SLOT_VECTORS_H\n\n")
    w("#include <stdint.h>\n\n")
    w("typedef struct {\n")
    w("    uint8_t bytes[16];\n")
    w("    uint8_t size;\n")
    w("    uint8_t llvm_slots;   /* llvm-mc'nin yazdigi slot sayisi */\n")
    w("    uint8_t nflips;\n")
    w("    struct { uint8_t bit, slot; } flips[%d];\n" % MAX_SLOTS)
    w("    const char *format;   /* beklenen format adi */\n")
    w("    const char *disasm;   /* llvm-mc ciktisi (kisaltilmis) */\n")
    w("} Aie2SlotVector;\n\n")
    w("static const Aie2SlotVector aie2_slot_vectors[] = {\n")
    for f, base, text, llvm_slots, flips in rows:
        esc = " ".join(text.replace("\\", "\\\\").replace('"', '\\"').split())
        w("    { { %s }, %d, %d, %d,\n" %
          (c_bytes(base, f['size']), f['size'], llvm_slots, len(flips)))
        # Bos dizi baslatici C11'de gecersiz; nflips zaten 0 oluyor.
        w("      { %s },\n" %
          ", ".join("{ %d, %d }" % (b, s) for b, s in flips or [(0, 0)]))
        w("      \"%s\", \"%s\" },\n" % (f['name'], esc[:70]))
    w("};\n\n")
    w("#define AIE2_SLOT_VECTOR_COUNT "
      "(sizeof(aie2_slot_vectors) / sizeof(aie2_slot_vectors[0]))\n\n")
    w("#endif /* AIE2_SLOT_VECTORS_H */\n")

    if unresolved:
        print("UYARI: ayirt edilemeyen: %s" % ", ".join(unresolved),
              file=sys.stderr)
    return 0


def main():
    if len(sys.argv) != 3 or sys.argv[1] not in ("--table", "--vectors"):
        print(__doc__, file=sys.stderr)
        return 2
    formats = parse_formats(sys.argv[2])
    if sys.argv[1] == "--table":
        emit_table(formats)
        return 0
    return emit_vectors(formats)


if __name__ == "__main__":
    sys.exit(main())
