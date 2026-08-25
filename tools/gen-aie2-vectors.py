#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-only
"""
AIE2 paket uzunlugu altin vektorlerini uretir.

Yontem: Xilinx/llvm-aie'nin `aie2` hedefli disassembler'i ALTIN STANDART
olarak kullanilir. Her dusuk-nibble deseni icin gecerli bir paket bulunur,
sonra ayni paket 3 kez tekrarlanip llvm'in TAM OLARAK 3 instruction
uretmesi dogrulanir. Boyut yanlis olsaydi llvm farkli sayida instruction
uretir veya gecersiz kodlama hatasi verirdi.

Kullanim:
    LLVM_MC=/path/to/llvm-mc python3 tools/gen-aie2-vectors.py \
        > tests/aie2_vectors.h

llvm-mc'yi derlemek icin:
    git clone --depth 1 -b aie-public https://github.com/Xilinx/llvm-aie
    cmake -G Ninja ../llvm -DCMAKE_BUILD_TYPE=Release \
          -DLLVM_TARGETS_TO_BUILD= -DLLVM_EXPERIMENTAL_TARGETS_TO_BUILD=AIE
    ninja llvm-mc
"""

import os
import random
import subprocess
import sys

MC = os.environ.get("LLVM_MC", "llvm-mc")
REPEAT = 3


def our_size(w):
    """include/xdna/xdna_aie.h icindeki cozucunun ayni mantigi."""
    if (w & 1) == 0:
        return 16
    if (w & 7) == 5:
        return 6
    return {1: 2, 3: 8, 7: 12, 9: 4, 0xB: 10, 0xF: 14}.get(w & 0xF, 0)


def disasm(data):
    hx = " ".join("0x%02x" % b for b in data)
    r = subprocess.run([MC, "-disassemble", "-triple=aie2"],
                       input=hx, capture_output=True, text=True)
    lines = [l.strip() for l in r.stdout.splitlines() if l.strip()]
    return lines, r.stderr


def find_packet(tag, size, tries=4000):
    """Verilen etiket icin gecerli bir paket bul: once sifir payload."""
    candidates = [bytes([tag]) + bytes(size - 1)]
    rng = random.Random(0x5EED + tag)
    for _ in range(tries):
        candidates.append(bytes([tag]) +
                          bytes(rng.randrange(256) for _ in range(size - 1)))
    for pkt in candidates:
        lines, err = disasm(pkt)
        if lines and "invalid" not in err.lower():
            return pkt, lines[0]
    return None, None


def main():
    print("/* SPDX-License-Identifier: GPL-2.0-only */")
    print("/*")
    print(" * OTOMATIK URETILDI -- tools/gen-aie2-vectors.py")
    print(" *")
    print(" * AIE2 paket uzunlugu altin vektorleri. Her satir, Xilinx/llvm-aie")
    print(" * disassembler'inin (aie2 hedefi) TEK bir instruction olarak")
    print(" * cozdugu gercek bir bayt dizisidir; boyut, ayni paket %d kez"
          % REPEAT)
    print(" * tekrarlandiginda llvm'in %d instruction uretmesiyle" % REPEAT)
    print(" * dogrulanmistir.")
    print(" */")
    print()
    print("#ifndef AIE2_VECTORS_H")
    print("#define AIE2_VECTORS_H")
    print()
    print("#include <stdint.h>")
    print()
    print("typedef struct {")
    print("    uint8_t bytes[16];")
    print("    uint32_t size;      /* llvm ile dogrulanmis paket boyutu */")
    print("    const char *disasm; /* llvm-mc ciktisi (kisaltilmis) */")
    print("} Aie2Vector;")
    print()
    print("static const Aie2Vector aie2_vectors[] = {")

    missing = []
    for tag in range(16):
        size = our_size(tag)
        pkt, text = find_packet(tag, size)
        if pkt is None:
            missing.append(tag)
            print("    /* etiket 0x%x: gecerli kodlama bulunamadi */" % tag,
                  file=sys.stderr)
            continue
        lines, err = disasm(pkt * REPEAT)
        if len(lines) != REPEAT or "invalid" in err.lower():
            print("    /* etiket 0x%x: %d instruction, dogrulanamadi */"
                  % (tag, len(lines)), file=sys.stderr)
            missing.append(tag)
            continue
        by = ", ".join("0x%02x" % b for b in pkt)
        esc = text.replace("\\", "\\\\").replace('"', '\\"')
        esc = " ".join(esc.split())[:60]
        print("    { { %s }, %d, \"%s\" }," % (by, size, esc))

    print("};")
    print()
    print("#define AIE2_VECTOR_COUNT "
          "(sizeof(aie2_vectors) / sizeof(aie2_vectors[0]))")
    print()
    print("#endif /* AIE2_VECTORS_H */")

    if missing:
        print("UYARI: dogrulanamayan etiketler: %s" %
              ", ".join("0x%x" % t for t in missing), file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
