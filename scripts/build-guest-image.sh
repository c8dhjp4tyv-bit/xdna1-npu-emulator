#!/usr/bin/env bash
# Build a disposable Linux guest image for stock amdxdna/XRT testing.
# SPDX-License-Identifier: GPL-2.0-only

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

# Fedora is used as the reproducible userspace base.  The kernel, signed
# amdxdna module, firmware and XRT are copied from the host explicitly so the
# resulting guest can exercise the exact stack selected by the caller.
BASE_IMAGE=${XDNA_GUEST_BASE_IMAGE:-quay.io/fedora/fedora@sha256:20f8c46b8323eb06f921935ce28dab50d5ac3673694d9f2ac866bab6a04b3706}
OUTPUT_DIR=${XDNA_GUEST_OUTPUT:-$REPO_ROOT/build/guest}
IMAGE_PATH=${XDNA_GUEST_IMAGE:-}
IMAGE_SIZE=${XDNA_GUEST_IMAGE_SIZE:-6G}
KVER=${XDNA_GUEST_KERNEL_VERSION:-$(uname -r)}
KERNEL_PATH=${XDNA_GUEST_KERNEL:-/boot/vmlinuz-$KVER}
INITRD_PATH=${XDNA_GUEST_INITRD:-/boot/initramfs-$KVER.img}
XRT_ROOT=${XDNA_GUEST_XRT_ROOT:-/opt/xilinx/xrt}
MODULE_ROOT=${XDNA_GUEST_MODULE_ROOT:-/lib/modules/$KVER}
FIRMWARE_ROOT=${XDNA_GUEST_FIRMWARE_ROOT:-/lib/firmware}
UAPI_ROOT=${XDNA_GUEST_UAPI_ROOT:-}
SSH_KEY=${XDNA_GUEST_SSH_KEY:-}
ENGINE=${XDNA_CONTAINER_ENGINE:-}
PULL=1
KEEP_IMAGE=0

usage() {
    cat <<'EOF'
Usage: scripts/build-guest-image.sh [options]

Build a disposable raw Fedora guest image with the host's matching kernel
modules, amdxdna firmware, XRT userspace and DRM UAPI headers.  The image is
created through Podman or Docker and is never committed to the repository.
The generated guest.env can be sourced by scripts/guest-integration.sh.

Options:
  --output-dir PATH       artifact directory (default: build/guest)
  --image PATH            output raw image (default: OUTPUT/xdna-fedora.raw)
  --image-size SIZE       sparse image size (default: 6G)
  --base-image IMAGE      pinned container base (default: Fedora digest)
  --kernel-version KVER   module/kernel release (default: uname -r)
  --kernel PATH           guest kernel (default: /boot/vmlinuz-KVER)
  --initrd PATH            guest initramfs (default: /boot/initramfs-KVER.img)
  --module-root PATH      matching /lib/modules/KVER tree
  --firmware-root PATH    firmware tree containing amdnpu/
  --xrt-root PATH         XRT installation (default: /opt/xilinx/xrt)
  --uapi-root PATH        directory containing amdxdna_accel.h and drm.h
  --ssh-key PATH          private ed25519 key (generated when omitted)
  --engine PATH           podman or docker executable
  --no-pull               do not pull the base image before building
  --keep-image            keep the temporary container builder image
  -h, --help              show this help

Environment variables use the XDNA_GUEST_* names corresponding to the
options.  The image uses -snapshot at runtime, so every probe run is
disposable.  No guest driver or kernel source is modified.
EOF
}

die() {
    printf 'build-guest-image.sh: %s\n' "$*" >&2
    exit 1
}

while (($#)); do
    case "$1" in
        --output-dir) (($# >= 2)) || die "--output-dir needs a path"; OUTPUT_DIR=$2; shift 2 ;;
        --image) (($# >= 2)) || die "--image needs a path"; IMAGE_PATH=$2; shift 2 ;;
        --image-size) (($# >= 2)) || die "--image-size needs a size"; IMAGE_SIZE=$2; shift 2 ;;
        --base-image) (($# >= 2)) || die "--base-image needs an image"; BASE_IMAGE=$2; shift 2 ;;
        --kernel-version) (($# >= 2)) || die "--kernel-version needs KVER"; KVER=$2; shift 2 ;;
        --kernel) (($# >= 2)) || die "--kernel needs a path"; KERNEL_PATH=$2; shift 2 ;;
        --initrd) (($# >= 2)) || die "--initrd needs a path"; INITRD_PATH=$2; shift 2 ;;
        --module-root) (($# >= 2)) || die "--module-root needs a path"; MODULE_ROOT=$2; shift 2 ;;
        --firmware-root) (($# >= 2)) || die "--firmware-root needs a path"; FIRMWARE_ROOT=$2; shift 2 ;;
        --xrt-root) (($# >= 2)) || die "--xrt-root needs a path"; XRT_ROOT=$2; shift 2 ;;
        --uapi-root) (($# >= 2)) || die "--uapi-root needs a path"; UAPI_ROOT=$2; shift 2 ;;
        --ssh-key) (($# >= 2)) || die "--ssh-key needs a path"; SSH_KEY=$2; shift 2 ;;
        --engine) (($# >= 2)) || die "--engine needs a path"; ENGINE=$2; shift 2 ;;
        --no-pull) PULL=0; shift ;;
        --keep-image) KEEP_IMAGE=1; shift ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1 (use --help)" ;;
    esac
done

command -v python3 >/dev/null || die "python3 is required"
command -v ssh-keygen >/dev/null || die "ssh-keygen is required"
command -v realpath >/dev/null || die "realpath is required"
command -v mktemp >/dev/null || die "mktemp is required"

if [[ -z "$ENGINE" ]]; then
    if command -v podman >/dev/null 2>&1; then
        ENGINE=$(command -v podman)
    elif command -v docker >/dev/null 2>&1; then
        ENGINE=$(command -v docker)
    else
        die "podman or docker is required"
    fi
elif [[ "$ENGINE" != */* ]]; then
    ENGINE=$(command -v "$ENGINE" 2>/dev/null || true)
fi
[[ -x "$ENGINE" ]] || die "container engine is not executable: $ENGINE"

[[ -f "$KERNEL_PATH" ]] || die "guest kernel not found: $KERNEL_PATH"
if [[ -n "$INITRD_PATH" && ! -r "$INITRD_PATH" ]]; then
    printf 'Warning: guest initramfs is not readable (%s); recording direct-kernel boot without an initrd.\n' \
        "$INITRD_PATH" >&2
    INITRD_PATH=
fi
[[ -d "$MODULE_ROOT" ]] || die "module tree not found: $MODULE_ROOT"
[[ -d "$FIRMWARE_ROOT/amdnpu" ]] || die "amdnpu firmware directory not found: $FIRMWARE_ROOT/amdnpu"
[[ -f "$XRT_ROOT/version.json" ]] || die "XRT version.json not found: $XRT_ROOT"
[[ -f "$XRT_ROOT/include/xrt/xrt_device.h" ]] || die "XRT headers not found: $XRT_ROOT/include"
[[ -x "$XRT_ROOT/bin/xrt-smi" ]] || die "xrt-smi not found: $XRT_ROOT/bin/xrt-smi"
if ! find "$XRT_ROOT/lib64" -maxdepth 1 -name 'libxrt_coreutil.so*' -print -quit 2>/dev/null | grep -q .; then
    die "XRT core library not found under $XRT_ROOT/lib64"
fi
AMDXDNA_MODULE=$(find "$MODULE_ROOT" -type f \
    \( -name 'amdxdna.ko' -o -name 'amdxdna.ko.xz' -o -name 'amdxdna.ko.zst' -o -name 'amdxdna.ko.gz' \) \
    -print -quit)
[[ -n "$AMDXDNA_MODULE" ]] || die "amdxdna kernel module not found under $MODULE_ROOT"

if [[ -z "$UAPI_ROOT" ]]; then
    for candidate in /usr/include/drm "$REPO_ROOT/../src/xdna-driver/src/include/uapi/drm"; do
        if [[ -f "$candidate/amdxdna_accel.h" && -f "$candidate/drm.h" ]]; then
            UAPI_ROOT=$candidate
            break
        fi
    done
fi
[[ -f "$UAPI_ROOT/amdxdna_accel.h" ]] || die "amdxdna_accel.h not found; pass --uapi-root"
[[ -f "$UAPI_ROOT/drm.h" ]] || die "drm.h not found; pass --uapi-root"

mkdir -p -- "$OUTPUT_DIR"
OUTPUT_DIR=$(cd -- "$OUTPUT_DIR" && pwd)
if [[ -z "$IMAGE_PATH" ]]; then
    IMAGE_PATH=$OUTPUT_DIR/xdna-fedora.raw
elif [[ "$IMAGE_PATH" != /* ]]; then
    IMAGE_PATH=$OUTPUT_DIR/$IMAGE_PATH
fi
IMAGE_PATH=$(realpath -m -- "$IMAGE_PATH")
IMAGE_DIR=$(dirname -- "$IMAGE_PATH")
mkdir -p -- "$IMAGE_DIR"

if [[ -z "$SSH_KEY" ]]; then
    SSH_KEY=$OUTPUT_DIR/ssh/id_ed25519
elif [[ "$SSH_KEY" != /* ]]; then
    SSH_KEY=$(realpath -m -- "$SSH_KEY")
fi
mkdir -p -- "$(dirname -- "$SSH_KEY")"
if [[ ! -f "$SSH_KEY" ]]; then
    ssh-keygen -q -t ed25519 -N '' -C xdna-guest -f "$SSH_KEY"
fi
[[ -f "$SSH_KEY" ]] || die "SSH private key was not created: $SSH_KEY"
if [[ ! -f "$SSH_KEY.pub" ]]; then
    ssh-keygen -y -f "$SSH_KEY" >"$SSH_KEY.pub"
    chmod 0644 "$SSH_KEY.pub"
fi
SSH_PUBLIC=$(<"$SSH_KEY.pub")
[[ -n "$SSH_PUBLIC" ]] || die "SSH public key is empty: $SSH_KEY.pub"

if ((PULL == 1)); then
    printf 'Pulling base image: %s\n' "$BASE_IMAGE"
    "$ENGINE" pull "$BASE_IMAGE"
fi
BASE_DIGEST=$("$ENGINE" image inspect "$BASE_IMAGE" --format '{{.Digest}}' 2>/dev/null || true)
[[ -n "$BASE_DIGEST" ]] || BASE_DIGEST=unknown
BASE_ID=$("$ENGINE" image inspect "$BASE_IMAGE" --format '{{.Id}}' 2>/dev/null || true)
[[ -n "$BASE_ID" ]] || BASE_ID=unknown

CONTEXT=$(mktemp -d "${TMPDIR:-/tmp}/xdna-guest-context.XXXXXX")
IMAGE_TAG=localhost/xdna-guest-builder:$BASHPID
cleanup() {
    local status=$?
    if ((KEEP_IMAGE == 0)); then
        "$ENGINE" image rm "$IMAGE_TAG" >/dev/null 2>&1 || true
    fi
    rm -rf -- "$CONTEXT"
    exit "$status"
}
trap cleanup EXIT

mkdir -p -- "$CONTEXT/modules" "$CONTEXT/firmware" \
    "$CONTEXT/xrt" "$CONTEXT/uapi" "$CONTEXT/ssh"
cp -a -- "$MODULE_ROOT"/. "$CONTEXT/modules/"
cp -a -- "$FIRMWARE_ROOT/amdnpu" "$CONTEXT/firmware/"
cp -a -- "$XRT_ROOT"/. "$CONTEXT/xrt/"
cp -a -- "$UAPI_ROOT/amdxdna_accel.h" "$CONTEXT/uapi/"
cp -a -- "$UAPI_ROOT/drm.h" "$CONTEXT/uapi/"
[[ -f "$UAPI_ROOT/drm_mode.h" ]] && cp -a -- "$UAPI_ROOT/drm_mode.h" "$CONTEXT/uapi/" || true
printf '%s\n' "$SSH_PUBLIC" >"$CONTEXT/ssh/authorized_keys"
cp -- "$SCRIPT_DIR/guest-image/Containerfile" "$CONTEXT/Containerfile"
cp -- "$SCRIPT_DIR/guest-image/guest-setup.sh" "$CONTEXT/guest-setup.sh"
cp -- "$SCRIPT_DIR/guest-image/make-disk.sh" "$CONTEXT/make-disk.sh"

printf 'Building disposable guest builder image: %s\n' "$IMAGE_TAG"
BUILD_PULL_ARGS=()
case "$(basename -- "$ENGINE")" in
    podman|podman-remote) BUILD_PULL_ARGS=(--pull=never) ;;
    docker)
        # Docker's --pull flag is boolean.  Keep --no-pull offline when the
        # caller requested it; after an explicit pull, false also prevents a
        # second mutable base-image lookup during the build.
        BUILD_PULL_ARGS=(--pull=false)
        ;;
    *) die "unsupported container engine (use Docker or Podman): $ENGINE" ;;
esac
"$ENGINE" build "${BUILD_PULL_ARGS[@]}" \
    --build-arg "BASE_IMAGE=$BASE_IMAGE" \
    --build-arg "KVER=$KVER" \
    --tag "$IMAGE_TAG" --file "$CONTEXT/Containerfile" "$CONTEXT"

printf 'Creating raw guest disk: %s (%s)\n' "$IMAGE_PATH" "$IMAGE_SIZE"
"$ENGINE" run --rm --network=none --user 0 \
    --volume "$IMAGE_DIR:/output:Z" "$IMAGE_TAG" \
    /usr/local/sbin/xdna-make-disk "/output/$(basename -- "$IMAGE_PATH")" "$IMAGE_SIZE"
[[ -s "$IMAGE_PATH" ]] || die "guest image was not created: $IMAGE_PATH"

# The image builder generated the SSH host key, so record its fingerprint as
# an expected identity for the runner.  The runner still uses a per-run
# known_hosts file, but it rejects a key that is not the key baked into this
# disposable image (rather than accepting an arbitrary process on the port).
SSH_HOST_FINGERPRINT=$("$ENGINE" run --rm --network=none "$IMAGE_TAG" \
    ssh-keygen -lf /etc/ssh/ssh_host_ed25519_key.pub -E sha256 2>/dev/null |
    awk 'NR == 1 { print $2; exit }')
[[ "$SSH_HOST_FINGERPRINT" =~ ^SHA256:[A-Za-z0-9+/=]+$ ]] ||
    die "could not determine the generated guest SSH host-key fingerprint"

XRT_VERSION=$(python3 - "$XRT_ROOT/version.json" <<'PY'
import json
import sys
try:
    with open(sys.argv[1], encoding="utf-8") as stream:
        data = json.load(stream)
except (OSError, json.JSONDecodeError):
    print("unknown")
else:
    print(data.get("BUILD_VERSION", "unknown"))
PY
)
DRIVER_VERSION=$(modinfo -F version "$AMDXDNA_MODULE" 2>/dev/null || true)
if [[ -z "$DRIVER_VERSION" ]]; then
    DRIVER_VERSION=$(modinfo -F srcversion "$AMDXDNA_MODULE" 2>/dev/null || true)
fi
[[ -n "$DRIVER_VERSION" ]] || DRIVER_VERSION=unknown
modinfo "$AMDXDNA_MODULE" >"$OUTPUT_DIR/amdxdna-modinfo.txt" 2>&1 || true

ENV_PATH=$OUTPUT_DIR/guest.env
GUEST_APPEND=${XDNA_GUEST_APPEND:-'root=/dev/vda rootfstype=ext4 rw console=ttyS0,115200 rd.luks=0 rd.md=0 rd.dm=0 rd.fips=0 ip=dhcp intel_iommu=on,sm_on iommu=on'}
{
    printf 'XDNA_GUEST_DISK=%q\n' "$IMAGE_PATH"
    printf 'XDNA_GUEST_DISK_FORMAT=raw\n'
    printf 'XDNA_GUEST_SSH=root@127.0.0.1\n'
    printf 'XDNA_GUEST_SSH_KEY=%q\n' "$SSH_KEY"
    printf 'XDNA_GUEST_SSH_HOST_FINGERPRINT=%q\n' "$SSH_HOST_FINGERPRINT"
    printf 'XDNA_GUEST_KERNEL=%q\n' "$KERNEL_PATH"
    printf 'XDNA_GUEST_INITRD=%q\n' "$INITRD_PATH"
    printf 'XDNA_GUEST_APPEND=%q\n' "$GUEST_APPEND"
    printf 'XDNA_GUEST_KERNEL_VERSION=%q\n' "$KVER"
    printf 'XDNA_GUEST_AMDXDNA_VERSION=%q\n' "$DRIVER_VERSION"
    printf 'XDNA_GUEST_XRT_VERSION=%q\n' "$XRT_VERSION"
    printf 'XDNA_GUEST_BASE_IMAGE=%q\n' "$BASE_IMAGE"
    printf 'XDNA_GUEST_BASE_DIGEST=%q\n' "$BASE_DIGEST"
    printf 'XDNA_GUEST_BASE_ID=%q\n' "$BASE_ID"
    printf 'XDNA_GUEST_IMAGE=%q\n' "$IMAGE_PATH"
    printf 'XDNA_GUEST_EVIDENCE=%q\n' "$OUTPUT_DIR/evidence"
} >"$ENV_PATH"
chmod 0644 "$ENV_PATH"

REPORT_PATH=$OUTPUT_DIR/build-report.txt
{
    printf 'XDNA disposable guest image build\n================================\n\n'
    printf 'Image: %s\nImage size: %s\n' "$IMAGE_PATH" "$IMAGE_SIZE"
    printf 'Base image: %s\nBase digest: %s\nBase ID: %s\n' "$BASE_IMAGE" "$BASE_DIGEST" "$BASE_ID"
    if [[ -n "$INITRD_PATH" ]]; then
        printf 'Kernel: %s\nInitrd: %s\nKernel release: %s\n' "$KERNEL_PATH" "$INITRD_PATH" "$KVER"
    else
        printf 'Kernel: %s\nInitrd: unavailable (direct-kernel boot)\nKernel release: %s\n' "$KERNEL_PATH" "$KVER"
    fi
    printf 'amdxdna module: %s\namdxdna version/srcversion: %s\n' "$AMDXDNA_MODULE" "$DRIVER_VERSION"
    printf 'Firmware root: %s\nXRT root: %s\nXRT version: %s\nUAPI root: %s\n' \
        "$FIRMWARE_ROOT" "$XRT_ROOT" "$XRT_VERSION" "$UAPI_ROOT"
    printf 'SSH key: %s\nSSH host-key fingerprint: %s\n\n' "$SSH_KEY" "$SSH_HOST_FINGERPRINT"
    printf 'Source environment before running the guest test:\n  source %q\n\n' "$ENV_PATH"
    printf 'Guest kernel append: %s\n\n' "$GUEST_APPEND"
    printf 'Guest test (normal and force_iova):\n  XDNA_QEMU_BINARY=/path/to/qemu-system-x86_64 %q --mode both\n' \
        "$REPO_ROOT/scripts/guest-integration.sh"
} >"$REPORT_PATH"
chmod 0644 "$REPORT_PATH"

printf 'Guest image: %s\nMetadata: %s\nReport: %s\n' "$IMAGE_PATH" "$ENV_PATH" "$REPORT_PATH"
