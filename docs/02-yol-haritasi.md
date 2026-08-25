# Yol haritasi

Her asamanin bir **kabul testi** var. "Calisiyor gibi gorunuyor" yeterli
degil; bir asama ancak kendi testi yesil oldugunda kapanir.

| # | Asama | Durum |
| --- | --- | --- |
| 1 | PCI kabugu | **derlendi ve dogrulandi** |
| 2 | Surucu boot'u (PSP/SMU/firmware) | **gercek guest'te dogrulandi** |
| 3 | Guest IOMMU: SVA/PASID | SVA yok; **carveout yoluyla asildi** |
| 4 | Yonetim firmware'i (MERT) | **gercek guest'te dogrulandi** |
| 5 | Bellek modeli (BO, DMA, heap) | **tile bellegi + DMA tamam, testli** |
| 6 | XDNA array mimari modeli | **tamam, stream switch dahil** |
| 7 | AIE instruction interpreter | **cozme katmani tamam**; semantik yok |
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

PCI kimligi, sinif kodu ve uc 64-bit BAR tasarlandigi gibi. Gercek bir
guest'te de dogrulandi (asama 2): kernel aygiti
`pci 0000:00:03.0: [1022:1502] type 00 class 0x120000` olarak buluyor ve
`amdxdna` surucusu bagliyor.

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

**Durum:** SVA'nin kendisi kapanmadi ama **pratik engel asildi**.

Aygit tarafinda yapilabilecek her sey yapildi: ATS + PRI + PASID
genisletilmis yetenekleri bildiriliyor ve guest bunlari goruyor. Kalan
engel QEMU vIOMMU / kernel SVA etkinlestirme yolunda. `force_iova=1`
kurtarici degil -- probe'u tamamen basarisiz kiliyor. Tum deney sonuclari:
`docs/03-acik-sorular.md`.

**Yedek yol: carveout.** Stock surucu, PASID alinamadiginda carveout
bellek yapilandirilmissa `open()`'i basarili sayiyor
(`amdxdna_pci_drv.c`: "PASID unavailable and carveout not configured").
Carveout, surucunun kendi debugfs arayuzunden ayarlanan fiziksel olarak
surekli bir blok:

```
/sys/kernel/debug/accel/<pci-adresi>/carveout  <-  "0x4000000@0x60000000"
```

Guest tarafinda hicbir sey degistirilmiyor; bu surucunun kendi ozelligi.
Bolgeyi cekirdek komut satirinda `memmap=64M$0x60000000` ile ayirmak
yeterli. Sonrasinda gercek guest'te:

```
/dev/accel/accel0 ACILDI (carveout ile, fd=3)
QUERY_AIE_VERSION      = 2.0
QUERY_FIRMWARE_VERSION = 5.7.0.0
QUERY_AIE_METADATA     = 5 sutun, sutun boyu 8192
CREATE_BO(DEV_HEAP)    = handle 1, 67108864 bayt
CREATE_HWCTX           = handle 1, syncobj 1
DESTROY_HWCTX          = tamam
```

Yani asama 4 ve 5, artik **gercek DRM UAPI uzerinden** de dogrulanmis
durumda: sorgular, BO olusturma/mmap ve context yasam dongusu guest'ten
emulatore kadar isliyor.

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
compute tile bellekleri modellendi. `SYNC_BO` dort kombinasyonu da
isliyor (host/dev x host/dev): cihaz adresleri
`heap_addr + (D - AIE2_DEVM_BASE)` ile context'in host heap'ine
cevriliyor, heap disi adresler reddediliyor.

Kalan: BO/IOVA muhasebesi ve BD adreslerinde cihaz adresi cevirisi
(gercek ctrlcode bunlari DDR_PATCH ile yamiyor).

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

**Durum:** **cozme (decode) katmani tamam**, yurutme (semantik) yok.
Yazilanlar: yurutme iskeleti, **paket uzunlugu cozucusu** ve **bundle slot
cozucusu**. Eksik olan, slot iceriklerinin ne anlama geldigi -- yani
instruction setinin kendisi.

AIE2 degisken uzunluklu bir VLIW: paket boyutu ilk kelimenin dusuk
bitlerindeki onek koduyla belirtiliyor. Kodlama `Xilinx/llvm-aie`
(`AIE2CompositeFormats.td`) icinden cikarildi:

| Dusuk bitler | Paket | Bayt |
| --- | --- | --- |
| `0` | instr128 | 16 |
| `101` | instr48 | 6 |
| `0001` | instr16 | 2 |
| `1001` | instr32 | 4 |
| `0011` | instr64 | 8 |
| `1011` | instr80 | 10 |
| `0111` | instr96 | 12 |
| `1111` | instr112 | 14 |

Tam bir onek kodu; bosluk yok. `xdna_aie2_packet_size()` disariya acik ve
`tests/test_core.c` tum bit desenlerini tek tek dogruluyor.

**Bu tablo tahmin degil, olculdu.** `llvm-aie` derlenip `aie2` hedefli
disassembler'i **altin standart** olarak kullanildi: her dusuk-nibble
deseni icin gercek bir paket bulundu ve ayni paket 3 kez tekrarlanip
llvm'in TAM OLARAK 3 instruction uretmesi dogrulandi. Boyut yanlis
olsaydi llvm farkli sayida instruction uretir veya gecersiz kodlama
hatasi verirdi. **16 desenin 16'si da uyustu.**

Ornek cikti:

```
0x01 0x00                    -> nop                          (2 bayt)
0x03 x8                      -> nops ; vadd.8 x0, x0, x0     (8 bayt)
0x0b 0xc8 0xcb ... (10 bayt) -> lda r31, [sp, #6212]         (10 bayt)
0x00 x16                     -> nopb ; nopa ; nops ; nopxm ;
                                vmac cm0, cm0, x0, x0, r0    (16 bayt)
```

Son satir 128 bitlik bundle'in **bes slot** tasidigini gosteriyor
(b / a / s / xm / vektor).

Uretilen altin vektorler `tests/aie2_vectors.h` icinde ve test paketi
bunlara karsi kosuyor; uretici betik `tools/gen-aie2-vectors.py`.

### 7b. Bundle slot cozucusu

AIE2 bir VLIW: her paket birkac islev biriminin ("slot") alanlarini yan
yana tasiyor. Hangi slotlarin bulundugunu ve bit araliklarini
**composite format** belirliyor; formati de paketin icindeki sabit bitler
ayirt ediyor. `llvm-aie`'nin `AIE2CompositeFormats.td` dosyasi bu
formatlarin **tamamini** TableGen ile tanimliyor:

| Boyut | Format sayisi |
| --- | --- |
| 2 bayt | 1 |
| 4 bayt | 6 |
| 6 bayt | 9 |
| 8 bayt | 11 |
| 10 bayt | 21 |
| 12 bayt | 20 |
| 14 bayt | 8 |
| 16 bayt | 2 |
| **toplam** | **78** |

Slot genislikleri sabit: `ldb` 16, `lda` 21, `st` 21, `alu` 20, `mv` 22,
`vec` 26, `lng` 42, `nop` 1 bit.

`tools/gen-aie2-formats.py` bu TableGen hiyerarsisini cozup duz bir C
tablosuna ceviriyor (`src/aie2_formats.h`, 78 satir); `xdna_aie2_decode()`
paketi bu tabloya gore formatina ve slotlarina ayiriyor. Uretici, **ayni
boyuttaki formatlarin sabit bitlerinin birbirinden ayirt edilebildigini**
dogruluyor -- yani cozum belirsiz degil, en fazla bir format eslesiyor.

Ornek: 16 baytlik bundle iki formattan biri, ayirt edici bit 27:

```
bit 27 = 0 : ldb[127:112] lda[111:91] st[90:70] lng[69:28]         vec[26:1]
bit 27 = 1 : ldb[127:112] lda[111:91] st[90:70] alu[69:50] mv[49:28] vec[26:1]
```

**Bu yerlesim de tahmin degil, olculdu.** `llvm-mc -triple=aie2`
disassembler'ina karsi differential test kosuldu:

1. Her format icin llvm'in tam olarak 1 instruction olarak cozdugu
   gecerli bir paket bulundu -- **78 formatin 78'i icin bulundu**.
2. llvm'in `;` ile ayirarak yazdigi slot sayisi bizim tablomuzdakiyle
   karsilastirildi -- **78 / 78 uyustu**.
3. Her slotun bit araligindan bir bit cevrilip llvm ciktisinda yalnizca
   bir slotun degistigi ve **hangi** slotun degistigi olculdu --
   **229 slotun 228'i dogrulandi, 0 yanlis**. Ayirt edilemeyen tek slot
   `instr16`'nin tek bitlik `nop` alani; iki degeri de `nop` bastigi icin
   ayrilamiyor (o formatta zaten tek slot var).

Olculen kayitlar `tests/aie2_slot_vectors.h` icinde ve `tests/test_core.c`
bizim cozucumuzun **ayni** slotu degistirdigini dogruluyor -- yani test
kendi tablomuza degil, llvm'in ciktisina bakiyor.

Negatif taraf da sinaniyor: `0x20000019` deseni instr32 etiketini tasiyor
ama hicbir formatin sabit bitlerine uymuyor; `xdna_aie2_decode()` reddediyor,
`llvm-mc` de "invalid instruction encoding" diyor.

`CORE_CONTROL` enable artik gercekten calisiyor: emulator program
bellegini getiriyor, paketi siniriyor, **formatini bulup slotlarina
ayiriyor** ve yurutemedigi icin **ERROR_HALT** ile duruyor. `CORE_PC`,
`CORE_SP`, `CORE_LR` ve `CORE_STATUS` gercek register offsetlerinde
okunabiliyor; ayrica surucuye `AIE_ERROR_INSTRUCTION` (core modulu,
olay 59) asenkron hatasi bildiriliyor. Gunluk ornegi:

```
core(0,2): PC 0x0, 4 baytlik AIE2 paketi AIE2__instr32__vec formatinda
           1 slota cozuldu ama instruction semantikleri uygulanmadi
           -- ERROR_HALT
```

**Kalan: semantik.** Slotlarin *icindeki* opcode/operand kodlamalari
`llvm-aie` TableGen'inde var (`AIE2GenInstrInfo`, binlerce instruction
sinifi) ve `llvm-mc` altin standart olarak elimizde -- yani bu is de
differential test ile ilerletilebilir. Ama net olmak gerekirse: binlerce
instruction sinifinin kodlamasini cikarmak ve ustune tam AIE2 vektor
semantigini (saturasyon, yuvarlama modlari, akumulator genisligi, permute
agi) dogrulamak hala **aylar mertebesinde** bir is. Bu asamada cozme
katmani ve onu dogrulayan altyapi kuruldu; yurutme kurulmadi.

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
