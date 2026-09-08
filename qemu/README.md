# QEMU integration

`qemu/hw/misc/xdna_npu.c` is a thin PCI wrapper around `libxdna`.  The
emulator core remains independent of QEMU; only PCI, MMIO, MSI-X and DMA
callbacks live in this directory.

## Reproducible QEMU 11.1.1 build

Run the integration script from the repository root:

```sh
scripts/build-qemu.sh
```

The script clones (or reuses) upstream QEMU `v11.1.1`, pinned to commit
`c3d48b7d1e89604920e5b81b91140c2ad39a1943`, creates a temporary worktree,
copies the core and wrapper into that worktree, applies
`qemu/integration/qemu-11.1.1.patch`, configures the x86_64 system target and
builds `qemu-system-x86_64` with warnings enabled and `-Werror`.  Slirp is
enabled for the guest runner's SSH forwarding; when a system libslirp
development package is unavailable, QEMU's pinned subproject is fetched.

The full QEMU source is never committed to this repository.  Use an existing
clean checkout when desired:

```sh
scripts/build-qemu.sh \
  --qemu-src /work/qemu-11.1.1 \
  --build-dir /tmp/xdna-qemu \
  --jobs 8
```

`--qemu-ref master --no-werror --baseline-only` is useful for the informational
upstream build check.  It compiles a moving upstream checkout without applying
the version-pinned integration patch; the primary script refuses a dirty Git
source tree and verifies the 11.1.1 commit before integrating it.

The generated `xdna-build.env` records the source, commit, build directory and
binary path.  `-device xdna-npu` is checked after linking.

## Device smoke test

The built binary can be started without a guest to verify QOM realization and
PCI identity:

```sh
qemu-system-x86_64 \
  -machine q35,accel=tcg -nodefaults -display none -S \
  -device xdna-npu
```

The device exposes AMD vendor `1022`, device `1502`, revision `00`, 64-bit BAR
0/2/4 and eight MSI-X vectors.  QEMU trace events are named
`xdna_npu_*`; enable them with `-d trace:xdna_npu_* -D xdna-qemu.log`.

The PCIe wrapper publishes the ATS, PASID and PRI extended capabilities used by
the stock amdxdna discovery/context path.  This is not a claim that the
emulator has functional PASID-tagged execution DMA: QEMU 11.1.1's generic PCI
DMA API supplies a no-PASID bus-master address space, `MemTxAttrs.pid` is only
8 bits, and the wrapper has no active per-context PASID at its DMA callback.
Array/execution operations therefore remain explicitly unsupported until the
memory/array model adds a context-aware DMA path.  The QEMU 11.1.1 integration
patch adds the Intel VT-d `SMPWC` ECAP only when `intel-iommu,svm=on` is
requested; QEMU's emulated RAM is coherent, so this is the compatibility bit
required by Linux SVM.  It is an integration fix in QEMU, not a guest-driver
change.

The wrapper's lifetime rules are deliberate: each BAR callback context is
embedded in the QOM object (no per-BAR heap leak), the core is freed on every
realize failure and exactly once from `exit`, and `msix_initialized` guards
both reset and uninitialization.  QEMU `MemTxResult` failures are logged and
returned to libxdna as DMA errors; reset only resets device state and IRQ
assertion.

## Guest integration

`scripts/build-guest-image.sh` builds a disposable raw Fedora image from a
pinned userspace base and copies in the selected host kernel, signed stock
`amdxdna.ko`, firmware, XRT userspace and DRM UAPI headers.  This avoids a
manually maintained VM disk; the runner opens the image with `-snapshot`.

```sh
scripts/build-guest-image.sh
set -a; . build/guest/guest.env; set +a
XDNA_QEMU_BINARY=/tmp/xdna-qemu/qemu-system-x86_64 \
  scripts/guest-integration.sh --mode both --debug-driver
```

Alternatively, `scripts/guest-integration.sh` boots a user-supplied guest
disk over QEMU user networking and collects evidence.  It does not modify
guest kernel or driver files, but `--mode both` unloads and reloads the stock
`amdxdna` module for the `force_iova=1` run and therefore changes driver state
inside that disposable snapshot.  A typical invocation is:

```sh
scripts/guest-integration.sh \
  --qemu /tmp/xdna-qemu/qemu-system-x86_64 \
  --disk /images/fedora-npu.qcow2 \
  --ssh root@127.0.0.1 --ssh-key ~/.ssh/id_ed25519 \
  --ssh-host-fingerprint SHA256:EXPECTED_GUEST_KEY
```

The runner requires the expected host-key fingerprint; the disposable image
builder writes it to `build/guest/guest.env`, so sourcing that file is the
usual path.  This prevents a different local process from supplying probe
results through the forwarded SSH port.

For an uninstalled QEMU build, the runner auto-detects the usual Seabios
directory; pass `--firmware-dir /path/to/qemu/share` when firmware is stored
elsewhere.

Use `--mode force_iova` (or the default `--mode both`) for the supported
`amdxdna.force_iova=1` compatibility path.  The runner unloads and reloads
the stock module with `modprobe amdxdna force_iova=1`; it never edits driver
source or module files.  For direct kernel boot, pass `--kernel`, optionally `--initrd`,
and `--append`; the runner adds `amdxdna.force_iova=1` to the force-IOVA
mode's command line automatically.  `--iommu intel` (the default) exposes
the SVA-capable Intel VT-d configuration; `--iommu none`, `amd` and `virtio`
are available for compatibility experiments.  `--pci-device` selects the
BDF used by the detailed PCI and XRT captures.
The guest must provide `lspci`, `dmesg`, stock `amdxdna`, and `xrt-smi`; the
script exits with status 77 when required guest inputs are absent.

Each run writes a timestamped evidence directory containing:

* `lspci.txt` and the exact PCI identity check;
* early `dmesg-amdxdna.txt` plus post-XRT `dmesg-amdxdna-final.txt`;
* `xrt-smi.txt`, open/context and repeated open/close results;
* QEMU stderr plus `xdna-qemu.log` trace output;
* `qemu-version.txt`, guest `versions.txt` and machine-readable `summary.json`;
* a human-readable `report.txt`.

The test records normal and force-IOVA modes separately.  It only reports
Stage 1/2 as complete after the guest probe and XRT context checks actually
pass.  The validated run is summarized in
[`docs/evidence/guest-20260908T225000Z.md`](../docs/evidence/guest-20260908T225000Z.md),
with raw artifacts under the selected local `build/guest/...` output directory.  No
execution opcode is faked: array-dependent operations continue to return an
explicit unsupported status until the memory/array model exists.
