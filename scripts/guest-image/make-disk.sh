#!/usr/bin/env bash
# Convert the configured container root into a sparse ext4 guest disk.
# SPDX-License-Identifier: GPL-2.0-only

set -Eeuo pipefail

OUTPUT=${1:?output image path is required}
SIZE=${2:-6G}
STAGING=$(mktemp -d /tmp/xdna-guest-root.XXXXXX)

cleanup() {
    local status=$?
    rm -rf -- "$STAGING"
    exit "$status"
}
trap cleanup EXIT

mkdir -p -- "$STAGING"
set +e
tar --numeric-owner --one-file-system --warning=no-file-changed \
    --exclude=./dev --exclude=./proc --exclude=./sys --exclude=./run \
    --exclude=./tmp --exclude=./mnt --exclude=./media --exclude=./output \
    -cpf - -C / . | tar --numeric-owner -xpf - -C "$STAGING"
tar_statuses=( "${PIPESTATUS[@]}" )
set -e
if (( tar_statuses[0] > 1 || tar_statuses[1] != 0 )); then
    printf 'root filesystem copy failed (tar=%s unpack=%s)\n' \
        "${tar_statuses[0]}" "${tar_statuses[1]}" >&2
    exit 1
fi

# Container runtimes bind-mount these paths.  GNU tar can skip a mounted file
# under --one-file-system, so recreate deterministic guest copies after the
# root filesystem is unpacked.  guest-setup.sh establishes xdna-guest; retain
# that value when the source file was omitted by the runtime mount.
guest_hostname=xdna-guest
if [[ -r "$STAGING/etc/hostname" ]]; then
    IFS= read -r guest_hostname <"$STAGING/etc/hostname" || true
    guest_hostname=${guest_hostname//$'\r'/}
fi
[[ "$guest_hostname" =~ ^[A-Za-z0-9][A-Za-z0-9.-]*$ ]] || guest_hostname=xdna-guest
mkdir -p -- "$STAGING/etc"
rm -f -- "$STAGING/etc/hostname" "$STAGING/etc/hosts" "$STAGING/etc/resolv.conf"
printf '%s\n' "$guest_hostname" >"$STAGING/etc/hostname"
printf '127.0.0.1 localhost localhost.localdomain\n::1 localhost localhost.localdomain\n127.0.1.1 %s\n' \
    "$guest_hostname" >"$STAGING/etc/hosts"
printf 'nameserver 10.0.2.3\n' >"$STAGING/etc/resolv.conf"

mkdir -p "$STAGING"/{dev,proc,sys,run,tmp,mnt,media}
chmod 1777 "$STAGING/tmp"

mkdir -p -- "$(dirname -- "$OUTPUT")"
truncate -s "$SIZE" "$OUTPUT"
mkfs.ext4 -q -F -L XDNA_GUEST -d "$STAGING" "$OUTPUT"
sync
printf 'guest image: %s\n' "$OUTPUT"
