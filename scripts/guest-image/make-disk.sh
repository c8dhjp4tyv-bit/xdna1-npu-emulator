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
mkdir -p "$STAGING"/{dev,proc,sys,run,tmp,mnt,media}
chmod 1777 "$STAGING/tmp"

mkdir -p -- "$(dirname -- "$OUTPUT")"
truncate -s "$SIZE" "$OUTPUT"
mkfs.ext4 -q -F -L XDNA_GUEST -d "$STAGING" "$OUTPUT"
sync
printf 'guest image: %s\n' "$OUTPUT"
