# Yol haritasi

Her asamanin bir **kabul testi** var. "Calisiyor gibi gorunuyor" yeterli
degil; bir asama ancak kendi testi yesil oldugunda kapanir.

| # | Asama | Durum |
| --- | --- | --- |
| 1 | PCI kabugu | **derlendi ve dogrulandi** |
| 2 | Surucu boot'u (PSP/SMU/firmware) | **gercek guest'te dogrulandi** |
| 3 | Guest IOMMU: SVA/PASID | deneyle karakterize edildi; vIOMMU engeli |
| 4 | Yonetim firmware'i (MERT) | **gercek guest'te dogrulandi** |
| 5 | Bellek modeli (BO, DMA, heap) | **tile bellegi + DMA tamam, testli** |
| 6 | XDNA array mimari modeli | **tamam, stream switch dahil** |
| 7 | AIE instruction interpreter | baslanmadi (asil kalan is) |
| 8 | ctrlcode motoru | **tamam, testli** |
| 9 | Uctan uca gercek workload | veri hareketi calisiyor; compute eksik |
| 10 | Uyumluluk + performans | baslanmadi |

---

## 1. PCI kabugu

`1022:1502` rev `00`, BAR 0/2/4 (64-bit), MSI-X, config space.

**Kabul:** guest icinde `lspci -nn` aygiti gosterir; `amdxdna` modulu
`probe`'a girer.

**Durum:** QEMU master agacinda derlendi ve calistirildi. `-device xdna-npu`
ile ornekleniyor; `info pci` ciktisi:

```
class Class 1200, addr 00:01.0, pci id 1022:1502
bar 0: mem [0x7fffe]   bar 2: mem [0x3fffe]   bar 4: mem [0xfffe]
```

PCI kimligi, sinif kodu ve uc 64-bit BAR tasarlandigi gibi. Guest icinde
`lspci`/`modprobe amdxdna` ile dogrulama henuz yapilmadi -- bunun icin
`CONFIG_DRM_ACCEL_AMDXDNA` etkin bir kernel gerekiyor.

## 2. Surucu boot'u

PSP/SMU register durum makineleri, firmware boot el sikismasi, mailbox
kanalinin kurulmasi.

**Kabul:** stock `amdxdna` probe'u hatasiz tamamlanir, `/dev/accel/accel0`
olusur, `dmesg` icinde firmware ve AIE surumu gorunur.

**Durum: KAPANDI.** Gercek bir guest'te, degistirilmemis `amdxdna` ile:

```
amdxdna 0000:00:03.0: [drm] Load firmware amdnpu/1502_00/npu.sbin
[drm] Initialized amdxdna_accel_driver 0.10.0 for 0000:00:03.0 on minor 0
/sys/class/accel/accel0/device/vbnv = RyzenAI-npu1
/sys/class/accel/accel0/device/fw_version = 5.7.0.0
```

Kurulum ve tekrar uretim: `qemu/guest-test/`.
Ayrica `make test` -> `tests/test_boot.c` 1390 kontrol.

## 3. Guest IOMMU: SVA/PASID

`amdxdna`, userspace client acildiginda `iommu_sva_bind_device()` cagirip
PASID aliyor. VM icinde bu zincirin de calismasi gerekiyor:

```
guest amdxdna -> guest SVA -> sanal AMD IOMMU -> QEMU
```

Ayrintili degerlendirme: `docs/03-acik-sorular.md`.

**Kabul:** guest'te `iommu_sva_bind_device()` gercekten basarili doner ve
her process kendi PASID'i ile ayri context alir.

**Durum:** deneyle karakterize edildi, kapanmadi. Aygit tarafinda
yapilabilecek her sey yapildi: ATS + PRI + PASID genisletilmis yetenekleri
bildiriliyor ve guest bunlari goruyor. Kalan engel QEMU vIOMMU / kernel SVA
etkinlestirme yolunda. `force_iova=1` kurtarici degil -- probe'u tamamen
basarisiz kiliyor. Tum deney sonuclari: `docs/03-acik-sorular.md`.

## 4. Yonetim firmware'i (MERT)

Mesaj protokolu, sorgular, context yasam dongusu, runtime config, telemetri,
reset.

**Kabul:** `xrt-smi examine` cihazi ve ozelliklerini dogru raporlar; context
olustur/yok et dongusu limitleriyle birlikte dogru davranir.

**Durum:** gercek guest'te dogrulandi -- surucu surum sorgularini,
tile bilgisini ve telemetriyi mailbox uzerinden basariyla aliyor
(`fw_version = 5.7.0.0` bizim emule ettigimiz surum).

Su an desteklenen opcode'lar: `GET_PROTOCOL_VERSION`,
`GET_FIRMWARE_VERSION`, `QUERY_AIE_VERSION`, `QUERY_AIE_TILE_INFO`,
`SET/GET_RUNTIME_CONFIG`, `ASSIGN_MGMT_PASID`, `SUSPEND`, `RESUME`,
`INVOKE_SELF_TEST`, `CREATE/DESTROY_CONTEXT`, `MAP/ADD_HOST_BUFFER`,
`REGISTER_ASYNC_EVENT_MSG`, `QUERY_COL_STATUS`, `GET_TELEMETRY`.

Yurutme yolu da eklendi: `CONFIG_CU`, `EXEC_DPU`, `CHAIN_EXEC_DPU`,
`SYNC_BO`, `CONFIG_DEBUG_BO`. Bunlar asama 8'deki ctrlcode motoruna
baglaniyor.

Compute tile programi gerektiren `EXECUTE_BUFFER_CF`,
`CHAIN_EXEC_BUFFER_CF` ve `CHAIN_EXEC_NPU` bilerek **acikca hata
donduruyor** -- AIE interpreter olmadan sessizce "basarili" demek yanlis
sonuc uretirdi.

## 5. Bellek modeli

BO'lar, adres cevirisi, instruction buffer (context basina 64 MB host
tamponu), host bellegine DMA, memory tile'lar.

**Kabul:** guest'ten yazilan bir BO'nun icerigi emulator tarafindan dogru
okunur; `SYNC_BO` her iki yonde dogru calisir.

**Durum:** shim DMA host bellegini gercekten okuyup yaziyor; memory ve
compute tile bellekleri modellendi; `SYNC_BO` host-host yolunda calisiyor.
Cihaz bellegi (AIE2_DEVM) yolu ve BO/IOVA muhasebesi henuz yok.

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

**Durum:** 20 compute tile, 5 memory tile, 5 shim; lock'lar, BD'ler, DMA
motorlari ve **stream switch** `aie-rt`'den dogrulanmis register haritasiyla
modellendi. Devre anahtarlamali yonlendirme, mesh komsu baglantilari
(NORTH<->SOUTH, EAST<->WEST) ve shim MUX/DEMUX dahil.
Ayrintilar: `docs/04-array-ve-ctrlcode.md`.

Kalan: paket anahtarlamali (packet-switched) akis ve trace portlari.

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

**Durum:** baslanmadi. `CORE_CONTROL` enable yazmasi kabul ediliyor ve
`core_status` guncelleniyor, ama program yurutulmuyor -- her seferinde
UYARI loglaniyor, sessizce basarili donmuyor. Bu, projenin kalan en buyuk
parcasi; `llvm-aie` bulgusu icin `docs/03-acik-sorular.md`.

## 8. ctrlcode motoru

`XAie_TxnOpcode` transaction'lari islenir ve gercek array durumunu degistirir:
overlay yukleme, stream switch konfigurasyonu, DMA baslatma, tile
baslatma/senkronizasyon.

**Kabul:** gercek bir workload'un ctrlcode'u bastan sona hatasiz yurutulur.

**Durum:** transaction bicimi `aie-rt`'den dogrulandi ve yorumlayici yazildi.
WRITE, MASKWRITE, MASKPOLL, BLOCKWRITE, BLOCKSET uygulandi. Custom op'lar
(TCT, DDR_PATCH) ve SHIMDMA_BD op'lari **atlaniyor ve uyari veriliyor** --
payload yerlesimleri dogrulanmadan uygulanmalari yanlis sonuc uretirdi.

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
