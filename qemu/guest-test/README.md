# Guest testi -- stock amdxdna surucusu ile

Bu dizin, emulatoru **gercek bir Linux guest'i icinde degistirilmemis
`amdxdna` surucusuyle** kosturmak icin gereken en kucuk kurulumu iceriyor.

## Sonuc (2026-08, QEMU master + Linux master)

```
########## XDNA EMULATOR GUEST TESTI ##########
  /dev/accel: accel0
  /sys/class/accel: accel0
  /sys/class/accel/accel0/device/vbnv = RyzenAI-npu1
  /sys/class/accel/accel0/device/fw_version = 5.7.0.0
  config space okundu: 4096 bayt
  ext cap @0x100 id=0x000f (ATS)
  ext cap @0x140 id=0x0013 (PRI)
  ext cap @0x180 id=0x001b (PASID)
  /dev/accel/accel0 ACILAMADI (SVA yok)
  carveout ayarlandi: 0x4000000@0x60000000
  /sys/kernel/debug/accel/0000:00:03.0/carveout = 0x4000000@0x60000000
  /dev/accel/accel0 ACILDI (carveout ile, fd=3)
  --- DRM ioctl'leri (stock UAPI) ---
  QUERY_AIE_VERSION      = 2.0
  QUERY_FIRMWARE_VERSION = 5.7.0.0
  QUERY_AIE_METADATA     = 5 sutun, sutun boyu 8192
    core: 4 satir @2, mem: 1 satir @1, shim: 1 satir @0
  CREATE_BO(DEV_HEAP)    = handle 1, 67108864 bayt
  GET_BO_INFO            = xdna_addr 0x4000000, map_offset 0x100000000
  heap mmap              = 0x7febab18c000
  CREATE_HWCTX           = handle 1, syncobj 1
  DESTROY_HWCTX          = tamam
########## TEST BITTI ##########
```

Yani stock surucu:

- aygiti `1022:1502` olarak buldu ve baglandi,
- SMU guc dizisini ve PSP firmware yuklemeyi gecti,
- firmware el sikismasini (FW_ALIVE + mgmt kanal bilgisi) tamamladi,
- mailbox uzerinden surum sorgularini yapti (`fw_version = 5.7.0.0`
  bizim emule ettigimiz firmware surumu),
- cihazi `RyzenAI-npu1` olarak tanidi,
- `/dev/accel/accel0` olusturdu,
- **`/dev/accel/accel0` acildi** ve DRM ioctl'leri emulatordan gercek
  degerler dondurdu,
- **donanim context'i olusturuldu ve yok edildi**.

### SVA engeli ve surucunun kendi yedek yolu: carveout

Surucu client acildiginda `iommu_sva_bind_device()` cagiriyor; QEMU
vIOMMU'sunda SVA baglanmiyor (analiz: `docs/03-acik-sorular.md`). Uzun
sure `/dev/accel/accel0` bu yuzden acilamiyordu.

Ama `amdxdna_drm_open` PASID alinamadiginda **carveout** yapilandirilmissa
open()'i basarili sayiyor (`amdxdna_pci_drv.c`). Carveout, surucunun kendi
debugfs arayuzunden ayarlanan, fiziksel olarak surekli bir bellek blogu:

```
/sys/kernel/debug/accel/<pci-adresi>/carveout  <-  "<boyut>@<adres>"
```

Bu **stock surucunun kendi ozelligi**; guest tarafinda hicbir sey
degistirilmiyor. Tek gereken, o fiziksel bolgenin kernel tarafindan
kullanilmiyor olmasi -- `run.sh` cekirdek komut satirina
`memmap=64M$0x60000000` ekliyor, `init.c` de `0x4000000@0x60000000`
yaziyor.

Sonrasinda BO'lar bu blogtan ayriliyor ve `CREATE_HWCTX` calisiyor.
Cihaz heap'inin **mmap edilmesi sart**: surucu context olustururken
heap'in user VA'sini firmware'e bildiriyor, mmap edilmemis heap ile
`CREATE_HWCTX` "Heap 0 is not mapped" diyor.

SVA yine de dogru cozum; carveout onu gerektirmeyen bir yol aciyor.

## Kurulum

### 1. QEMU

`qemu/README.md`'deki adimlarla aygiti agaca ekleyip derleyin.

### 2. Kernel

```sh
git clone --depth 1 https://github.com/torvalds/linux.git
cd linux
make defconfig
./scripts/config \
  --enable DRM --enable DRM_ACCEL --enable DRM_ACCEL_AMDXDNA \
  --enable AMD_IOMMU --enable IOMMU_SUPPORT \
  --enable BLK_DEV_INITRD --enable DEVTMPFS --enable DEVTMPFS_MOUNT \
  --enable DEBUG_FS \
  --set-str INITRAMFS_SOURCE "/path/to/initramfs" \
  --enable SERIAL_8250 --enable SERIAL_8250_CONSOLE \
  --enable DEBUG_INFO_NONE
make olddefconfig
make -j$(nproc) bzImage
```

`CONFIG_DRM_ACCEL_AMDXDNA=y` oldugunu dogrulayin (modul degil, gomulu).

### 3. initramfs

```sh
mkdir -p initramfs/{proc,sys,dev,lib/firmware/amdnpu/1502_00}
# UAPI basliklari kernel kaynagindan geliyor (DRM ioctl'leri icin).
gcc -static -O2 -Wno-cpp -I/path/to/linux/include/uapi \
    -o initramfs/init init.c
# Surucu firmware dosyasinin VAR OLMASINI bekliyor; icerigi onemli degil,
# emulator yalnizca okunabilirligini dogruluyor.
head -c 262144 /dev/urandom > initramfs/lib/firmware/amdnpu/1502_00/npu.sbin
```

### 4. Kosum

```sh
QEMU=/path/to/qemu-system-x86_64 \
BZIMAGE=/path/to/bzImage \
IOMMU=amd ./run.sh
```

`IOMMU=intel` PASID destekli vIOMMU ile dener (SVA hala baglanmiyor).

## Bilinen kisitlar

- **IOMMU sart.** `aie2_init` icinde `if (!xdna->group)` kontrolu var:
  IOMMU grubu yoksa surucu `-EINVAL` ile cikiyor.
- **Hipervizor tespiti.** Ayni fonksiyonda
  `if (!hypervisor_is_type(X86_HYPER_NATIVE))` kontrolu var; surucu
  bilinen bir hipervizor altinda calismayi reddediyor. QEMU'yu **TCG ile**
  (yani `-accel kvm` OLMADAN) kosturunca kernel `X86_HYPER_NATIVE`
  goruyor ve kontrol geciyor. KVM ile bu kontrol basarisiz olur.
- **PSP yolu IOMMU'dan gecmez.** Surucu firmware tamponunun adresini
  `virt_to_phys()` ile veriyor, yani DMA API'sini atliyor. Emulator bunu
  ayri bir `phys_read` callback'i ile modelliyor; aksi halde IOMMU ceviri
  modundayken firmware yukleme sayfa hatasi veriyor.
- **SVA baglanmiyor.** `/dev/accel/accel0` acmak icin carveout yolu
  kullaniliyor (yukariya bakin). Bu, `CONFIG_DEBUG_FS=y` ve debugfs'in
  bagli olmasini gerektiriyor.
