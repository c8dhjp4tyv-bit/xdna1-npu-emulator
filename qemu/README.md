# QEMU aygiti

`hw/misc/xdna_npu.c` ince bir sarmalayicidir: tum davranis `libxdna`
icindedir. Buradaki tek is QEMU'nun PCI/MMIO/MSI-X/DMA arayuzlerini
`XdnaHostOps` callback'lerine baglamak.

> **Bu dosya bu depoda derlenmemistir.** Konteynerde QEMU kaynak agaci yok.
> Asagidaki adimlar denenmemis olabilir; ilk derlemede kucuk API
> uyusmazliklari cikmasi normal (ozellikle `ResettableClass` faz imzasi ve
> `pci_dma_read/write` donus tipi QEMU surumleri arasinda degisti).

## Kurulum

```sh
git clone https://gitlab.com/qemu-project/qemu.git
cd qemu

# Emulator cekirdegini ve aygiti agaca bagla
cp -r /path/to/xdna1-npu-emulator/src        hw/misc/xdna/
cp -r /path/to/xdna1-npu-emulator/include    hw/misc/xdna/include
cp /path/to/xdna1-npu-emulator/qemu/hw/misc/xdna_npu.c hw/misc/
```

`hw/misc/meson.build` icine:

```meson
system_ss.add(when: 'CONFIG_XDNA_NPU', if_true: files(
  'xdna_npu.c',
  'xdna/xdna_device.c',
  'xdna/xdna_psp.c',
  'xdna/xdna_smu.c',
  'xdna/xdna_fw.c',
  'xdna/xdna_mailbox.c',
  'xdna/xdna_mert.c',
))
```

`hw/misc/Kconfig` icine:

```
config XDNA_NPU
    bool
    default y if PCI_DEVICES
    depends on PCI
```

Include yolu icin `hw/misc/meson.build` basina:

```meson
xdna_inc = include_directories('xdna/include')
```

ve `system_ss.add(...)` cagrisina `include_directories: xdna_inc` ekleyin.

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
