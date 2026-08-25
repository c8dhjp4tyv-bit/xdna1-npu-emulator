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
  /dev/accel/accel0 ACILAMADI
########## TEST BITTI ##########
```

Yani stock surucu:

- aygiti `1022:1502` olarak buldu ve baglandi,
- SMU guc dizisini ve PSP firmware yuklemeyi gecti,
- firmware el sikismasini (FW_ALIVE + mgmt kanal bilgisi) tamamladi,
- mailbox uzerinden surum sorgularini yapti (`fw_version = 5.7.0.0`
  bizim emule ettigimiz firmware surumu),
- cihazi `RyzenAI-npu1` olarak tanidi,
- `/dev/accel/accel0` olusturdu.

**Acilamayan tek sey `/dev/accel/accel0` open()**: surucu client
acildiginda `iommu_sva_bind_device()` cagiriyor ve QEMU'nun vIOMMU'sunda
SVA baglanmiyor. Ayrintili analiz: `docs/03-acik-sorular.md`.

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
gcc -static -O2 -o initramfs/init init.c
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
