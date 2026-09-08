# Yol haritasi

Her asamanin bir **kabul testi** var. "Calisiyor gibi gorunuyor" yeterli
degil; bir asama ancak kendi testi yesil oldugunda kapanir.

| # | Asama | Durum |
| --- | --- | --- |
| 1 | PCI kabugu | **tamamlandi** -- temiz QEMU 11.1.1 agacinda `-Werror` ve gercek guest A kabul testi yesil |
| 2 | Surucu boot'u (PSP/SMU/firmware) | **tamamlandi (gercek guest)** -- stock `amdxdna` probe'u, firmware ve `/dev/accel/accel0` yesil |
| 3 | Guest IOMMU: SVA/PASID | **gozlemlendi** -- normal SVA/PASID ve `amdxdna.force_iova=1` yollarinin ikisi de gercek guest'te yesil |
| 4 | Yonetim firmware'i (MERT) | **temel mesajlar ve XRT context yasam dongusu tamam, testli** |
| 5 | Bellek modeli (BO, DMA, heap) | **baslanmadi** |
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
XRT/context kontrollerini makine-okunur kanitla birlikte yapar. Bu kabul
gercek guest'te `1022:1502 rev 00`, stock driver binding, `xrt-smi examine`,
tekrarlanan open/close ve context create/destroy ile tamamlandi.

**Durum:** `qemu/hw/misc/xdna_npu.c`, temiz QEMU v11.1.1 agacina
`scripts/build-qemu.sh` ile entegre edilip `-Werror` altinda derleniyor.
Gercek guest A/B/C ve XRT D kaniti
[`docs/evidence/guest-20260908T204620Z.md`](evidence/guest-20260908T204620Z.md)
icindedir.

## 2. Surucu boot'u

PSP/SMU register durum makineleri, firmware boot el sikismasi, mailbox
kanalinin kurulmasi.

**Kabul:** stock `amdxdna` probe'u hatasiz tamamlanir, `/dev/accel/accel0`
olusur, `dmesg` icinde firmware ve AIE surumu gorunur.

**Durum:** cekirdek tarafi ve gercek guest tarafi tamam. `make test` ->
`tests/test_boot.c`, surucunun boot dizisini birebir taklit ediyor. Stock
`amdxdna` 0.10.0 probe'u firmware'i yukledi, `/dev/accel/accel0` olustu ve
stock XRT 2.26.0 `xrt-smi examine` ile cihazi gordu.

## 3. Guest IOMMU: SVA/PASID

`amdxdna`, userspace client acildiginda `iommu_sva_bind_device()` cagirip
PASID aliyor. VM icinde bu zincirin de calismasi gerekiyor:

```
guest amdxdna -> guest SVA -> sanal AMD IOMMU -> QEMU
```

Ayrintili degerlendirme: `docs/03-acik-sorular.md`.

**Kabul:** guest'te `iommu_sva_bind_device()` gercekten basarili doner ve
her process kendi PASID'i ile ayri context alir. Ayrica `force_iova=1`
uyumluluk yolu stock driver degistirilmeden calisir.

**Gozlenen sonuc (QEMU 11.1.1):** Normal kipte Intel VT-d SVM (`svm=on`,
PASID/ATS/PRI ve coherent SVM ECAP) ile stock driver SVA yoluna girdi; PCI
config space'te ATS/PASID/PRI capability'leri ve MERT context kayitlarinda
`pasid 1` goruldu. Bu, probe/context uyumluluk yolunun gozlemidir; QEMU
11.1.1'in genel PCI DMA API'si PASID'e gore address space secmiyor ve
`MemTxAttrs.pid` yalnizca 8 bit, dolayisiyla emulator PASID-tag'li execution
DMA'si iddia etmiyor. PSP firmware dogrulamasi icin `virt_to_phys()` tamponu
QEMU'nun fiziksel DMA callback'i ile okundu, diger kontrol-duzlemi DMA'si
PASID'siz `pci_dma_*` yolunda kaldi. `iommu=on amdxdna.force_iova=1` kipinde
driver kendi IOVA domain'ini kurdu; ayni stock firmware/XRT open/context
testleri gecti ve dmesg `Enabled force_iova mode` kaydetti. Ayrinti ve ham
artifact yollar
[`docs/evidence/guest-20260908T204620Z.md`](evidence/guest-20260908T204620Z.md)
icindedir.

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

## 5. Bellek modeli (baslanmadi)

BO'lar, adres cevirisi, instruction buffer (context basina 64 MB host
tamponu), host bellegine DMA, memory tile'lar.

Bu asama henuz baslatilmadi. Yurutme ve `SYNC_BO` opcode'lari bellek/array
modeli hazir olana kadar basarisiz donmeye devam eder; guest probe'u ve XRT
context yasam dongusunun gecmesi bu asamayi otomatik olarak baslatmaz.

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
