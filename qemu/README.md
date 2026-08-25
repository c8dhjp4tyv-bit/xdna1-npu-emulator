# QEMU aygiti

`hw/misc/xdna_npu.c` ince bir sarmalayicidir: tum davranis `libxdna`
icindedir. Buradaki tek is QEMU'nun PCI/MMIO/MSI-X/DMA arayuzlerini
`XdnaHostOps` callback'lerine baglamak.

> **Durum: derlendi ve dogrulandi.** QEMU master agacinda (2026-08) derlenip
> calistirildi; aygit `-device xdna-npu` ile ornekleniyor ve PCI kimligi,
> sinif kodu ve uc BAR bekledigimiz gibi gorunuyor:
>
> ```
> class Class 1200, addr 00:01.0, pci id 1022:1502
> bar 0: mem [0x7fffe]   bar 2: mem [0x3fffe]   bar 4: mem [0xfffe]
> ```
>
> Derleme sirasinda uc API uyusmazligi duzeltildi: `qdev-properties.h`
> artik `hw/core/` altinda, `class_init` imzasi `const void *data` aldi,
> `LOG_TRACE` yerine `LOG_UNIMP` kullaniliyor. Eski QEMU surumlerinde
> `ResettableClass` faz imzasi farkli olabilir.

## Kurulum

```sh
git clone --depth 1 https://gitlab.com/qemu-project/qemu.git
cd qemu

# Emulator cekirdegini ve aygiti agaca bagla.
# DIKKAT: basliklar hw/misc/xdna/xdna/ altinda olmali; kaynaklardaki
# #include "xdna/..." satirlari dosyanin kendi dizinine gore cozuluyor.
R=/path/to/xdna1-npu-emulator
mkdir -p hw/misc/xdna/xdna
cp $R/src/*.c $R/src/*.h            hw/misc/xdna/
cp $R/include/xdna/*.h              hw/misc/xdna/xdna/
cp $R/qemu/hw/misc/xdna_npu.c       hw/misc/xdna/
```

`hw/misc/meson.build` icine:

```meson
system_ss.add(when: 'CONFIG_XDNA_NPU', if_true: files(
  'xdna/xdna_npu.c',
  'xdna/xdna_device.c',
  'xdna/xdna_psp.c',
  'xdna/xdna_smu.c',
  'xdna/xdna_fw.c',
  'xdna/xdna_mailbox.c',
  'xdna/xdna_mert.c',
  'xdna/xdna_array.c',
  'xdna/xdna_txn.c',
  'xdna/xdna_error.c',
  'xdna/xdna_core.c',
))
```

`hw/misc/Kconfig` icine:

```
config XDNA_NPU
    bool
    default y if PCI_DEVICES
    depends on PCI && MSI_NONBROKEN
```

Ayri bir include yolu gerekmiyor: yerlesim dogru oldugunda
`#include "xdna/..."` satirlari kendiliginden cozuluyor.

Derleme:

```sh
./configure --target-list=x86_64-softmmu --disable-docs --disable-tools
ninja -C build
build/qemu-system-x86_64 -device help | grep xdna
```

## Calistirma

```sh
qemu-system-x86_64 \
  -machine q35,accel=kvm \
  -cpu host -m 8G \
  -device xdna-npu \
  ...
```

Guest icinde:

```sh
lspci -nn | grep 1502          # 1022:1502 gorunmeli
modprobe amdxdna               # veya: modprobe amdxdna force_iova=1
dmesg | grep -i xdna
ls /dev/accel/
```

Firmware guest icinde `/lib/firmware/amdnpu/1502_00/npu.sbin` yolunda
bulunmali -- surucu imaji host bellegine yukleyip PSP'ye adresini veriyor,
emulator de o adresi DMA ile okuyor. Imaj gercekten yurutulmuyor, ama
okunabiliyor olmasi gerekiyor.

`force_iova=1` icin gerekce: `docs/03-acik-sorular.md`.

## Bilinecek noktalar

- **BAR'lar 64-bit.** Surucu BAR 0/2/4 bekliyor; 64-bit BAR'lar 0-1, 2-3,
  4-5 yuvalarini tuketiyor, bu da gercek donanimin da 64-bit BAR
  kullandigini gosteriyor.
- **MSI-X tablosu BAR0'in ust bolgesinde** (`0x40000` / `0x50000`).
  Ayri BAR yuvasi kalmadigi icin; registerlar `0x11000`'in altinda.
- **Interrupt tetigi senkron.** Emulator, guest'in mailbox tail
  register'ina yazmasi sirasinda cevabi uretip MSI-X tetikliyor. Gercek
  donanimda bu asenkron. Fonksiyonel olarak fark yok; zamanlamaya duyarli
  bir test yazilirsa bu nokta ayrilmali (is parcaciklarina tasinmali).
