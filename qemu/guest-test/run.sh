#!/bin/sh
# Emulatoru gercek bir guest kernel + stock amdxdna surucusu ile calistirir.
#
# Gerekenler (bkz. README.md):
#   $QEMU        derlenmis qemu-system-x86_64 (xdna-npu aygiti dahil)
#   $BZIMAGE     CONFIG_DRM_ACCEL_AMDXDNA=y ve gomulu initramfs ile kernel
set -eu

QEMU=${QEMU:-./qemu-system-x86_64}
BZIMAGE=${BZIMAGE:-./bzImage}
IOMMU=${IOMMU:-intel}

case "$IOMMU" in
intel)
    # VARSAYILAN. Uctan uca workload YALNIZCA burada calisiyor: QEMU'nun
    # intel-iommu'su aygit DMA'sini gercekten ceviriyor, amd-iommu'su
    # cevirmiyor (bkz. docs/03-acik-sorular.md). BO'lar IOVA ile
    # adreslendigi icin ceviri sart. SVA yine de baglanmiyor.
    IOMMU_DEV="-device intel-iommu,scalable-mode=on,svm=on,fsts=on,pasid-bits=16,device-iotlb=on,intremap=on"
    CMDLINE="console=ttyS0 panic=1 intel_iommu=on,sm_on memmap=128M\$0x60000000"
    ;;
amd)
    # Probe ve sorgular geciyor, /dev/accel/accel0 olusuyor; ama aygit
    # DMA'si cevrilmedigi icin workload yolu calismiyor.
    IOMMU_DEV="-device amd-iommu"
    CMDLINE="console=ttyS0 panic=1 memmap=128M\$0x60000000"
    ;;
*)
    echo "IOMMU=amd|intel" >&2
    exit 1
    ;;
esac

exec "$QEMU" \
    -machine q35,kernel-irqchip=split -m 2G -smp 2 \
    -display none -nodefaults \
    $IOMMU_DEV \
    -kernel "$BZIMAGE" \
    -append "$CMDLINE" \
    -device xdna-npu,addr=0x3 \
    -serial stdio
