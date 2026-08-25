# xdna1-npu-emulator

AMD XDNA1 (Phoenix / Hawk Point) NPU'nun **fonksiyonel donanim emulatoru**.

Hedef: host makinede fiziksel NPU olmadan, guest VM icinde **degistirilmemis
stock `amdxdna` surucusu ve stock XRT** ile gercek XDNA1 workload'larini
calistirmak. Bu bir vGPU degil; QEMU'nun baska bir CPU mimarisini emule
etmesine benzer bir is.

Ayrintili hedef ve kapsam: [`docs/00-hedef-ve-kapsam.md`](docs/00-hedef-ve-kapsam.md).

## Yapi

```
include/xdna/     libxdna genel API'si ve dogrulanmis donanim sabitleri
src/              QEMU'dan bagimsiz emulator cekirdegi
  xdna_device.c     MMIO yonlendirmesi, reset, aygit govdesi
  xdna_psp.c        PSP firmware yukleme durum makinesi
  xdna_smu.c        SMU guc / saat durum makinesi
  xdna_fw.c         firmware boot el sikismasi
  xdna_mailbox.c    mailbox ring buffer protokolu (cihaz tarafi)
  xdna_mert.c       yonetim firmware'i (MERT) mesaj isleyicisi
  xdna_array.c      XDNA array: tile'lar, lock'lar, BD'ler, DMA, stream switch
  xdna_txn.c        ctrlcode (XAie transaction) yorumlayicisi
  xdna_core.c       AIE2 VLIW paket + bundle slot cozucusu
  aie2_formats.h    uretilmis AIE2 composite format tablosu (78 format)
tests/            surucu davranisini taklit eden kosumlar
                    aie2_vectors.h       paket uzunlugu altin vektorleri
                    aie2_slot_vectors.h  slot yerlesimi differential vektorleri
                    txn_vectors.h        derleyici encoder'iyla uretilmis ctrlcode
tools/            gen-aie2-vectors.py -- altin vektor ureticisi
                  gen-aie2-formats.py -- format tablosu + slot vektorleri
                  gen-txn-vectors.cpp -- ctrlcode vektorleri (derleyicinin
                                         kendi encoder'i ile)
qemu/             QEMU PCI aygiti sarmalayicisi
docs/             hedef, dogrulanmis donanim arayuzu, yol haritasi, acik sorular
```

Cekirdek kasitli olarak QEMU'dan bagimsiz: ayni kod hem QEMU aygitindan hem
de testlerden surulebiliyor, boylece her degisiklik QEMU/VM kurmadan
dogrulanabiliyor.

## Derleme ve test

```sh
make          # build/libxdna.a
make test     # surucu boot dizisi kosumu
```

`make test` uc kosum calistirir.

**test_boot** stock `amdxdna` surucusunun boot dizisini birebir taklit eder --
SMU guc dizisi, PSP firmware yukleme, firmware el sikismasi, mailbox kanali,
runtime config, PASID atama, suspend/resume, surum sorgulari, context
olusturma/yok etme, ring buffer sarmalanmasi ve hata yollari.

**test_exec** gercek bicimde bir ctrlcode uretir, `EXEC_DPU` ile gonderir ve
verinin host -> shim DMA -> memory tile -> shim DMA -> host yolundan birebir
gectigini dogrular; lock semantigi, BLOCKWRITE, komut zinciri, `SYNC_BO`,
cihaz adresi cevirisi ve hata yollari da test edilir. Ayrica
**derleyicinin kendi encoder'iyla** (mlir-aie `TxnEncoding.h`) uretilmis
ctrlcode'u kosturur -- yani yorumlayici gercek derleyici ciktisina karsi
dogrulaniyor.

**test_core** AIE2 paket uzunlugu ve bundle slot cozucusunu `llvm-aie`
disassembler'iyla uretilmis differential vektorlere karsi dogrular, sonra
program bellegine yazip core'u calistirir ve `ERROR_HALT` ile durdugunu
yalnizca MMIO uzerinden okur.

test_boot ve test_exec kodu emulatorun ic yapilarina bakmaz; sadece MMIO
okur/yazar.

## Durum

| Asama | Durum |
| --- | --- |
| 1. PCI kabugu | **QEMU'da derlendi ve dogrulandi** |
| 2. Surucu boot'u | **gercek guest'te dogrulandi** |
| 3. Guest IOMMU (SVA/PASID) | SVA yok; **carveout ile asildi** |
| 4. Yonetim firmware'i (MERT) | **gercek guest'te dogrulandi** |
| 5. Bellek modeli / DMA | tile bellegi + DMA tamam, testli |
| 6. XDNA array modeli | tamam, stream switch dahil |
| 7. AIE instruction interpreter | **cozme katmani tamam**; semantik yok |
| 8. ctrlcode motoru | tamam; **derleyici ciktisina karsi testli** |
| 9. Uctan uca workload | **gercek guest'te veri yolu calisti**; compute eksik |
| 10. Uyumluluk + performans | baslanmadi |

Tam liste ve kabul kriterleri: [`docs/02-yol-haritasi.md`](docs/02-yol-haritasi.md).

## Gercek guest'te dogrulama

Emulator, gercek bir Linux guest'inde **degistirilmemis stock `amdxdna`
surucusuyle** kosturuldu:

```
/dev/accel: accel0
/sys/class/accel/accel0/device/vbnv = RyzenAI-npu1
/sys/class/accel/accel0/device/fw_version = 5.7.0.0
/dev/accel/accel0 ACILDI (carveout ile, fd=3)
QUERY_AIE_VERSION      = 2.0
QUERY_FIRMWARE_VERSION = 5.7.0.0
QUERY_AIE_METADATA     = 5 sutun, sutun boyu 8192
  core: 4 satir @2, mem: 1 satir @1, shim: 1 satir @0
CREATE_BO(DEV_HEAP)    = handle 1, 67108864 bayt
GET_BO_INFO            = xdna_addr 0x4000000, map_offset 0x100000000
CREATE_HWCTX           = handle 1, syncobj 1
--- uctan uca workload ---
BO'lar                 = inst 0x4020000, in 0x4030000, out 0x4038000
CONFIG_HWCTX(CU)       = tamam
ctrlcode               = 720 bayt
EXEC_CMD               = seq 0
SYNCOBJ_TIMELINE_WAIT  = tamam (nokta 0)
komut durumu           = 4 (COMPLETED)
SONUC                  = cikis girisle BIREBIR AYNI (256 bayt)
DESTROY_HWCTX          = tamam
```

Surucu aygiti buldu, SMU guc dizisini ve PSP firmware yuklemesini gecti,
firmware el sikismasini tamamladi, mailbox uzerinden surum sorgularini
yapti, `/dev/accel/accel0` olusturdu; guest **stock DRM UAPI'siyle**
aygiti acti, bilgi sorgularini yapti, cihaz heap'i BO'su olusturup mmap
etti, donanim context'i olusturdu ve **gercek bir workload'i uctan uca
kosturup dogru cikti aldi**:

```
guest userspace -> DRM EXEC_CMD (ERT_START_NPU) -> stock amdxdna
  -> mailbox CHAIN_EXEC_DPU -> emulator MERT -> ctrlcode yorumlayicisi
  -> XDNA array: shim MM2S -> memory tile (lock) -> shim S2MM
  -> cikis BO'su
```

Guest tarafinda degistirilmis hicbir sey yok; emulator tarafinda da
kisayol yok -- veri gercekten modellenmis DMA, lock ve stream switch
uzerinden geciyor. Yurutulen ctrlcode, `tests/test_exec.c`'nin
dogruladigi ureticinin (`tests/ctrlcode.h`) ta kendisi.

`iommu_sva_bind_device()` QEMU vIOMMU'sunda hala baglanmiyor. Aygiti acmak
icin **surucunun kendi carveout yolu** kullaniliyor: stock surucu, PASID
alinamadiginda debugfs'ten ayarlanmis bir carveout bellek blogu varsa
`open()`'i basarili sayiyor. Guest tarafinda hicbir sey degistirilmiyor.
Ayrica uctan uca yol icin QEMU'da `intel-iommu` gerekiyor; `amd-iommu`
aygit DMA'sini cevirmiyor. Kurulum, tekrar uretim ve tam analiz:
[`qemu/guest-test/`](qemu/guest-test/) ve
[`docs/03-acik-sorular.md`](docs/03-acik-sorular.md).

`EXEC_DPU` ve `CHAIN_EXEC_DPU` gercek ctrlcode'u yurutuyor: tampon
adresleri cihaz bellegi penceresinden context heap'ine cevriliyor, XAie
transaction'lari yorumlaniyor ve array uzerinde BD/lock/DMA islemleri
gerceklesiyor. `tests/test_exec.c` bunu uctan uca dogruluyor
(host -> memory tile -> host), gercek guest de ayni yolu kosturuyor.

Compute tile programlari **yurutulmuyor**: AIE2 instruction semantikleri
(asama 7) henuz yok. Ama **cozme katmani tamam** ve `CORE_CONTROL` enable
gercekten calisiyor: emulator program bellegini getiriyor, AIE2 VLIW
paketini siniriyor, 78 composite formattan hangisi oldugunu bulup
`ldb/lda/st/alu/mv/lng/vec` slotlarina ayiriyor, sonra yurutemedigi icin
**ERROR_HALT** ile duruyor -- `CORE_PC` ve `CORE_STATUS` gercek
registerlarda okunabiliyor, surucuye de `AIE_ERROR_INSTRUCTION` asenkron
hatasi bildiriliyor.

Hem paket uzunlugu hem slot yerlesimi `llvm-aie` TableGen'inden cikarildi
ve `llvm-mc -triple=aie2` disassembler'ina karsi **differential test ile
olculdu**: 78 formatin 78'inde slot sayisi uyustu, 229 slot->bit
eslemesinin 228'i bit-flip ile dogrulandi, 0 yanlis. Ayrinti:
[`docs/02-yol-haritasi.md`](docs/02-yol-haritasi.md) asama 7.

`EXECUTE_BUFFER_CF`, `CHAIN_EXEC_BUFFER_CF` ve `CHAIN_EXEC_NPU` acikca hata
donduruyor.

## Dogrulanmis donanim arayuzu

`include/xdna/xdna_regs.h` icindeki her deger upstream `amdxdna` surucusunun
kaynagindan cikarilmistir; tahmin edilenler `TODO(dogrula)` ile isaretlidir.
Referans belgeler:
[`docs/01-donanim-arayuzu.md`](docs/01-donanim-arayuzu.md) (host arayuzu),
[`docs/04-array-ve-ctrlcode.md`](docs/04-array-ve-ctrlcode.md) (array ve
ctrlcode -- degerler `aie-rt` kaynagindan dogrulandi).

En kritik ayrinti: mailbox cevaplarinin boyutu, o opcode'un
`struct <name>_resp` boyutuna **birebir** esit olmali -- hata durumunda bile.
Surucu (`xdna_msg_cb`) boyutu karsilastirip farkliysa `-EINVAL` donduruyor.

## Lisans

GPL-2.0-only. Host arayuz sabitleri GPL-2.0 lisansli `amdxdna` surucusunun,
array ve ctrlcode sabitleri MIT lisansli `aie-rt` surucusunun herkese acik
kaynagindan cikarilmistir.
