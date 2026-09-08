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
make qemu     # clean QEMU 11.1.1 integration/build (out-of-tree)
```

`make test` stock `amdxdna` surucusunun boot dizisini birebir taklit eder --
SMU guc dizisi, PSP firmware yukleme, firmware el sikismasi, mailbox kanali,
runtime config, PASID atama, suspend/resume, surum sorgulari, context
olusturma/yok etme, ring buffer sarmalanmasi ve hata yollari. Test kodu
emulatorun ic yapilarina bakmaz; sadece MMIO okur/yazar.

## Durum

| Asama | Durum |
| --- | --- |
| 1. PCI kabugu | **tamamlandi** -- temiz QEMU 11.1.1 agacinda `-Werror` ve gercek guest A kabul testi yesil |
| 2. Surucu boot'u | **tamamlandi (gercek guest)** -- stock `amdxdna` probe'u, firmware boot'u ve `/dev/accel/accel0` yesil |
| 3. Guest IOMMU (SVA/PASID) | **probe/context gozlemlendi** -- normal SVA/PASID ve `force_iova=1` yollarinin ikisi de gercek guest'te yesil; PASID-tag'li execution DMA henuz yok |
| 4. Yonetim firmware'i (MERT) | temel mesajlar tamam, testli |
| 5-10. Bellek modeli, array, ISA, ctrlcode, uctan uca | **baslanmadi** |

Tam liste ve kabul kriterleri: [`docs/02-yol-haritasi.md`](docs/02-yol-haritasi.md).

Gercek guest calistirmasi icin once [`qemu/README.md`](qemu/README.md)'deki
`scripts/build-qemu.sh` adimini tamamlayin. Tekrarlanabilir, disposable bir
guest image'i host'un sectigi cekirdek/modul/firmware/XRT yiginindan su sekilde
olusturabilirsiniz:

```sh
scripts/build-guest-image.sh
set -a; . build/guest/guest.env; set +a
XDNA_QEMU_BINARY=/tmp/xdna-qemu/qemu-system-x86_64 \
  scripts/guest-integration.sh --mode both --debug-driver
```

Image, sabitlenmis Fedora container userspace'ini kullanir; stock kernel,
`amdxdna.ko`, firmware ve XRT host'tan kopyalanir. Runner her modu ayri
`-snapshot` boot'unda deneyip `build/guest/evidence/<timestamp>/` altinda
`summary.json`, insan-okunur rapor ve ham `lspci`, `dmesg`, `xrt-smi`, QEMU
trace ve surum kanitlarini birakir. Guest kernel'i veya driver'i degistirmez.

Son dogrulama kaniti: [`docs/evidence/guest-20260908T204620Z.md`](docs/evidence/guest-20260908T204620Z.md)
(ham artifact'lar `build/guest/audit/evidence/20260908T220931Z/` altinda,
`normal/` ve `force_iova/` alt dizinlerinde).

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
