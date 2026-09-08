# Yol haritasi

Her asamanin bir **kabul testi** var. "Calisiyor gibi gorunuyor" yeterli
degil; bir asama ancak kendi testi yesil oldugunda kapanir.

| # | Asama | Durum |
| --- | --- | --- |
| 1 | PCI kabugu | **QEMU 11.1.1 temiz agacinda derlendi (`-Werror`)**; gercek guest kabul testi bekliyor |
| 2 | Surucu boot'u (PSP/SMU/firmware) | **cekirdek tamam, `make test` testli**; gercek guest probe'u bekliyor |
| 3 | Guest IOMMU: SVA/PASID | acik problem |
| 4 | Yonetim firmware'i (MERT) | **temel mesajlar tamam, testli** |
| 5 | Bellek modeli (BO, DMA, heap) | baslanmadi |
| 6 | XDNA array mimari modeli | baslanmadi |
| 7 | AIE instruction interpreter | acik problem |
| 8 | ctrlcode motoru | baslanmadi |
| 9 | Uctan uca gercek workload | baslanmadi |
| 10 | Uyumluluk + performans | baslanmadi |

---

## 1. PCI kabugu

`1022:1502` rev `00`, BAR 0/2/4 (64-bit), MSI-X, config space.

**Kabul:** guest icinde `lspci -nn` aygiti gosterir; `amdxdna` modulu
`probe`'a girer. `scripts/guest-integration.sh` bu kontrolu ve sonraki
XRT/context kontrollerini makine-okunur kanitla birlikte yapar.

**Durum:** `qemu/hw/misc/xdna_npu.c`, temiz QEMU v11.1.1 agacina
`scripts/build-qemu.sh` ile entegre edilip `-Werror` altinda derleniyor.
Tamamlanmasi icin gercek guest'te PCI kimligi, driver probe'u ve XRT context
yasam dongusu A/B/C kabul testi yesil olmalidir.

## 2. Surucu boot'u

PSP/SMU register durum makineleri, firmware boot el sikismasi, mailbox
kanalinin kurulmasi.

**Kabul:** stock `amdxdna` probe'u hatasiz tamamlanir, `/dev/accel/accel0`
olusur, `dmesg` icinde firmware ve AIE surumu gorunur.

**Durum:** cekirdek tarafi tamam. `make test` -> `tests/test_boot.c`,
surucunun boot dizisini birebir taklit ediyor. Gercek guest kaniti ve stock
XRT sonucu `evidence/guest/<timestamp>/summary.json` icinde gorulmeden bu
asama kapanmis sayilmaz.

## 3. Guest IOMMU: SVA/PASID

`amdxdna`, userspace client acildiginda `iommu_sva_bind_device()` cagirip
PASID aliyor. VM icinde bu zincirin de calismasi gerekiyor:

```
guest amdxdna -> guest SVA -> sanal AMD IOMMU -> QEMU
```

Ayrintili degerlendirme: `docs/03-acik-sorular.md`.

**Kabul:** guest'te `iommu_sva_bind_device()` gercekten basarili doner ve
her process kendi PASID'i ile ayri context alir.

## 4. Yonetim firmware'i (MERT)

Mesaj protokolu, sorgular, context yasam dongusu, runtime config, telemetri,
reset.

**Kabul:** `xrt-smi examine` cihazi ve ozelliklerini dogru raporlar; context
olustur/yok et dongusu limitleriyle birlikte dogru davranir.

**Durum:** su an desteklenen opcode'lar: `GET_PROTOCOL_VERSION`,
`GET_FIRMWARE_VERSION`, `QUERY_AIE_VERSION`, `QUERY_AIE_TILE_INFO`,
`SET/GET_RUNTIME_CONFIG`, `ASSIGN_MGMT_PASID`, `SUSPEND`, `RESUME`,
`INVOKE_SELF_TEST`, `CREATE/DESTROY_CONTEXT`, `MAP/ADD_HOST_BUFFER`,
`REGISTER_ASYNC_EVENT_MSG`, `QUERY_COL_STATUS`, `GET_TELEMETRY`.

Yurutme opcode'lari (`CONFIG_CU`, `EXECUTE_BUFFER_CF`, `EXEC_DPU`,
`CHAIN_EXEC_*`, `SYNC_BO`) bilerek **acikca hata donduruyor** -- sessizce
"basarili" demek yanlis sonuc uretirdi.

## 5. Bellek modeli

BO'lar, adres cevirisi, instruction buffer (context basina 64 MB host
tamponu), host bellegine DMA, memory tile'lar.

**Kabul:** guest'ten yazilan bir BO'nun icerigi emulator tarafindan dogru
okunur; `SYNC_BO` her iki yonde dogru calisir.

## 6. XDNA array mimari modeli

20 compute tile, 5 memory tile, stream switch'ler, lock'lar, DMA motorlari
icin mimari durum.

```c
struct XdnaArray {
    MemoryTile  mem[5];
    ComputeTile compute[4][5];
    StreamSwitch streams;
    DmaEngine   dma[5];
    Context     contexts[6];
};
```

Her compute tile: PC, skaler ve vektor register'lari, program bellegi, veri
bellegi, durum, lock'lar, giris/cikis stream'leri.

**Kabul:** `QUERY_COL_STATUS` gercek array durumundan anlamli bir dokum
uretir (su an sifir dokuyor).

## 7. AIE instruction interpreter

Compute tile ELF'i gercekten decode edilip host CPU'da instruction
instruction calistirilir:

```
fetch -> decode -> execute -> register/bellek guncelle -> PC ilerlet
```

Tipki bir CPU emulatoru gibi. En buyuk belirsizlik burada; bkz.
`docs/03-acik-sorular.md`.

**Kabul:** bilinen bir cekirdek (orn. tek tile'lik bir GEMM) bilinen girdi
icin bit-birebir dogru cikti uretir.

## 8. ctrlcode motoru

`XAie_TxnOpcode` transaction'lari islenir ve gercek array durumunu degistirir:
overlay yukleme, stream switch konfigurasyonu, DMA baslatma, tile
baslatma/senkronizasyon.

**Kabul:** gercek bir workload'un ctrlcode'u bastan sona hatasiz yurutulur.

## 9. Uctan uca gercek workload

Degistirilmemis gercek XDNA binary'si stock XRT uzerinden calisir ve gercek
Hawk Point ile ayni ciktiyi uretir.

**Kabul:**

```
            AYNI TEST
          test.xclbin, ayni girdi
           /                \
   gercek Hawk Point      emulator
       cikti A              cikti B
           \                /
              A == B ?
```

Yalnizca cikti degil; context davranisi, DMA sirasi, interrupt'lar, hatalar,
tile durumu ve register'lar da karsilastirilir.

Bu asamaya ulasildiginda proje "xrt-smi spoof" olmaktan tamamen cikar.

## 10. Uyumluluk ve hiz

Coklu context, hata yollari, reset, suspend/resume. Sonra performans:
interpreter -> temel blok onbellegi -> JIT -> host SIMD.
