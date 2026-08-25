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
tests/            surucu davranisini taklit eden kosumlar
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

`make test` iki kosum calistirir.

**test_boot** stock `amdxdna` surucusunun boot dizisini birebir taklit eder --
SMU guc dizisi, PSP firmware yukleme, firmware el sikismasi, mailbox kanali,
runtime config, PASID atama, suspend/resume, surum sorgulari, context
olusturma/yok etme, ring buffer sarmalanmasi ve hata yollari.

**test_exec** gercek bicimde bir ctrlcode uretir, `EXEC_DPU` ile gonderir ve
verinin host -> shim DMA -> memory tile -> shim DMA -> host yolundan birebir
gectigini dogrular; lock semantigi, BLOCKWRITE, komut zinciri, `SYNC_BO` ve
dort hata yolu da test edilir.

Her iki testin kodu da emulatorun ic yapilarina bakmaz; sadece MMIO okur/yazar.

## Durum

| Asama | Durum |
| --- | --- |
| 1. PCI kabugu | **QEMU'da derlendi ve dogrulandi** |
| 2. Surucu boot'u | **gercek guest'te dogrulandi** |
| 3. Guest IOMMU (SVA/PASID) | deneyle karakterize; vIOMMU engeli |
| 4. Yonetim firmware'i (MERT) | **gercek guest'te dogrulandi** |
| 5. Bellek modeli / DMA | tile bellegi + DMA tamam, testli |
| 6. XDNA array modeli | tamam, stream switch dahil |
| 7. AIE instruction interpreter | **baslanmadi -- kalan asil is** |
| 8. ctrlcode motoru | tamam, testli |
| 9. Uctan uca workload | veri hareketi calisiyor; compute eksik |
| 10. Uyumluluk + performans | baslanmadi |

Tam liste ve kabul kriterleri: [`docs/02-yol-haritasi.md`](docs/02-yol-haritasi.md).

## Gercek guest'te dogrulama

Emulator, gercek bir Linux guest'inde **degistirilmemis stock `amdxdna`
surucusuyle** kosturuldu:

```
/dev/accel: accel0
/sys/class/accel/accel0/device/vbnv = RyzenAI-npu1
/sys/class/accel/accel0/device/fw_version = 5.7.0.0
```

Surucu aygiti buldu, SMU guc dizisini ve PSP firmware yuklemesini gecti,
firmware el sikismasini tamamladi, mailbox uzerinden surum sorgularini
yapti ve `/dev/accel/accel0` olusturdu.

`/dev/accel/accel0` **open()** edilemiyor: surucu client acildiginda
`iommu_sva_bind_device()` cagiriyor ve QEMU vIOMMU'sunda SVA baglanmiyor.
Kurulum, tekrar uretim ve tam analiz: [`qemu/guest-test/`](qemu/guest-test/)
ve [`docs/03-acik-sorular.md`](docs/03-acik-sorular.md).

`EXEC_DPU` ve `CHAIN_EXEC_DPU` gercek ctrlcode'u yurutuyor: host bellegindin
DMA ile okunuyor, XAie transaction'lari yorumlaniyor ve array uzerinde
BD/lock/DMA islemleri gerceklesiyor. `tests/test_exec.c` bunu uctan uca
dogruluyor (host -> memory tile -> host).

Compute tile programlari **yurutulmuyor**: AIE instruction interpreter
(asama 7) henuz yok. `CORE_CONTROL` enable yazmasi kabul ediliyor ama her
seferinde UYARI loglaniyor -- sessizce basarili donmuyor.
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
