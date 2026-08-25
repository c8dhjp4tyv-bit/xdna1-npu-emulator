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
| 7 | AIE instruction interpreter | iskelet + paket cozucu var; ISA yok |
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

**Durum:** yurutme iskeleti ve **paket uzunlugu cozucusu** yazildi;
instruction seti henuz yok.

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
(b / a / s / xm / vektor), yani slot cozumunun hedefi bu.

Uretilen altin vektorler `tests/aie2_vectors.h` icinde ve test paketi
bunlara karsi kosuyor; uretici betik `tools/gen-aie2-vectors.py`.

`CORE_CONTROL` enable artik gercekten calisiyor: emulator program
bellegini getiriyor, paketi dogru siniriyor ve slot'lari
calistiramadigi icin **ERROR_HALT** ile duruyor. `CORE_PC`, `CORE_SP`,
`CORE_LR` ve `CORE_STATUS` gercek register offsetlerinde okunabiliyor;
ayrica surucuye `AIE_ERROR_INSTRUCTION` (core modulu, olay 59) asenkron
hatasi bildiriliyor.

Yani "core enable edildi ama hicbir sey olmadi" durumu bitti: hangi
PC'de, hangi boyutta bir paketle karsilastigimiz raporlaniyor.

**Kalan:** slot cozumu ve semantik. VLIW paketi icindeki alu / lda / st /
mv slot'larinin kodlamalari `llvm-aie` TableGen'inde tam olarak var
(`AIE2GenInstrFormats.td`, her instruction sinifi icin bit atamalari).

Artik elimizde calisan bir altin standart var (`llvm-mc -triple=aie2`),
yani slot cozucusu **differential test ile** gelistirilebilir: bizim
cozumumuzu llvm'in ciktisiyla karsilastir. Bu, ISA alt kumesini
dogrulanabilir sekilde implemente etmenin saglam yolu.

Ama net olmak gerekirse: tam AIE2 vektor semantigi (saturasyon,
yuvarlama modlari, akumulator genisligi, permute agi) hala aylar
mertebesinde bir is. Bu asamada altyapisi kuruldu, kendisi degil.

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
