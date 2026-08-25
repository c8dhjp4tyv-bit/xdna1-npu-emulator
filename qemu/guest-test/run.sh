#!/bin/sh
# Emulatoru gercek bir guest kernel + stock amdxdna surucusu ile calistirir.
#
# Gerekenler (bkz. README.md):
#   $QEMU        derlenmis qemu-system-x86_64 (xdna-npu aygiti dahil)
#   $BZIMAGE     CONFIG_DRM_ACCEL_AMDXDNA=y ve gomulu initramfs ile kernel
set -eu

QEMU=${QEMU:-./qemu-system-x86_64}
BZIMAGE=${BZIMAGE:-./bzImage}
IOMMU=${IOMMU:-amd}

case "$IOMMU" in
amd)
    # Calisan yapilandirma: probe geciyor, /dev/accel/accel0 olusuyor.
    # SVA yok (QEMU'nun amd-iommu'sunda PASID destegi yok).
    IOMMU_DEV="-device amd-iommu"
    CMDLINE="console=ttyS0 panic=1"
    ;;
intel)
    # PASID destekli vIOMMU; SVA hala baglanmiyor (bkz. docs/03).
    IOMMU_DEV="-device intel-iommu,scalable-mode=on,svm=on,fsts=on,pasid-bits=16,device-iotlb=on,intremap=on"
    CMDLINE="console=ttyS0 panic=1 intel_iommu=on,sm_on"
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
