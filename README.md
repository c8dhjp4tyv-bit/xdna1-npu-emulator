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

`make test` stock `amdxdna` surucusunun boot dizisini birebir taklit eder --
SMU guc dizisi, PSP firmware yukleme, firmware el sikismasi, mailbox kanali,
runtime config, PASID atama, suspend/resume, surum sorgulari, context
olusturma/yok etme, ring buffer sarmalanmasi ve hata yollari. Test kodu
emulatorun ic yapilarina bakmaz; sadece MMIO okur/yazar.

## Durum

| Asama | Durum |
| --- | --- |
| 1. PCI kabugu | QEMU aygiti yazildi, **derlenmedi** (bu depoda QEMU agaci yok) |
| 2. Surucu boot'u | cekirdek tamam, testli |
| 3. Guest IOMMU (SVA/PASID) | plan var, bkz. acik sorular |
| 4. Yonetim firmware'i (MERT) | temel mesajlar tamam, testli |
| 5-10. Bellek modeli, array, ISA, ctrlcode, uctan uca | baslanmadi |

Tam liste ve kabul kriterleri: [`docs/02-yol-haritasi.md`](docs/02-yol-haritasi.md).

Yurutme opcode'lari (`CONFIG_CU`, `EXECUTE_BUFFER_CF`, `EXEC_DPU`,
`CHAIN_EXEC_*`, `SYNC_BO`) su an bilerek **acikca hata donduruyor**. XDNA
array'i olmadan sessizce "basarili" demek yanlis sonuc uretirdi.

## Dogrulanmis donanim arayuzu

`include/xdna/xdna_regs.h` icindeki her deger upstream `amdxdna` surucusunun
kaynagindan cikarilmistir; tahmin edilenler `TODO(dogrula)` ile isaretlidir.
Referans belge: [`docs/01-donanim-arayuzu.md`](docs/01-donanim-arayuzu.md).

En kritik ayrinti: mailbox cevaplarinin boyutu, o opcode'un
`struct <name>_resp` boyutuna **birebir** esit olmali -- hata durumunda bile.
Surucu (`xdna_msg_cb`) boyutu karsilastirip farkliysa `-EINVAL` donduruyor.

## Lisans

GPL-2.0-only. Donanim arayuz sabitleri, GPL-2.0 lisansli `amdxdna`
surucusunun herkese acik kaynagindan cikarilmistir.
