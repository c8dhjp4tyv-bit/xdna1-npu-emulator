# xdna1-npu-emulator

`xdna1-npu-emulator` is a functional AMD XDNA1 (Phoenix / Hawk Point) NPU
emulator for QEMU, targeting unmodified stock `amdxdna` + XRT guests. The
QEMU-independent `libxdna` core models the boot, management-firmware, mailbox,
reset, interrupt, and guest-integration paths; reproducible QEMU 11.1.1 builds
and disposable real-guest validation are included.

The goal is to run real XDNA1 workloads inside a guest VM on a host with no
physical NPU, using the **unmodified stock `amdxdna` driver and stock XRT**.
This is not a vGPU; the intended model is closer to QEMU functionally emulating
a processor architecture.

Detailed goals and scope: [`docs/00-hedef-ve-kapsam.md`](docs/00-hedef-ve-kapsam.md).

> [!WARNING]
> **Current limitations:** This emulator is not yet capable of executing
> arbitrary XDNA workloads. The Stage 5 memory model is not yet implemented.
> Array- and execution-dependent opcodes continue to fail explicitly rather
> than report fabricated success.

No firmware blobs, kernel modules, or host-generated guest images are vendored
or committed. The disposable guest-image workflow copies the selected signed
host artifacts at build time and keeps them under ignored local output paths.

## Structure

```text
include/xdna/     libxdna public API and verified hardware constants
src/              QEMU-independent emulator core
  xdna_device.c     MMIO routing, reset, device body
  xdna_psp.c        PSP firmware-load state machine
  xdna_smu.c        SMU power/clock state machine
  xdna_fw.c         firmware boot handshake
  xdna_mailbox.c    mailbox ring-buffer protocol, device side
  xdna_mert.c       management-firmware (MERT) message handler
tests/            tests that reproduce stock-driver behavior
qemu/             QEMU PCI-device wrapper
docs/             scope, verified hardware interface, roadmap, open questions
```

The core is intentionally independent from QEMU: the same code can be driven
from both the QEMU device and the test suite, so most changes can be validated
without booting a VM.

## Build and test

```sh
make          # build/libxdna.a
make test     # run the stock-driver boot-sequence test
make qemu     # clean QEMU 11.1.1 integration/build, out of tree
```

`make test` reproduces the stock `amdxdna` driver's boot sequence through the
observable MMIO interface: the SMU power sequence, PSP firmware load, firmware
handshake, mailbox channel, runtime configuration, PASID assignment,
suspend/resume, version queries, context creation/destruction, ring-buffer
wraparound, and error paths. The test code does not inspect emulator internals;
it only performs MMIO reads and writes.

## Status

| Stage | Status |
| --- | --- |
| 1. PCI shell | **complete** — real-guest Acceptance A passes and the device builds in a clean QEMU 11.1.1 tree with `-Werror` |
| 2. Driver boot | **complete on a real guest** — stock `amdxdna` probe, firmware boot, and `/dev/accel/accel0` all pass |
| 3. Guest IOMMU (SVA/PASID) | **probe/context path observed** — both normal SVA/PASID and `force_iova=1` pass in a real guest; PASID-tagged execution DMA is not implemented yet |
| 4. Management firmware (MERT) | basic management messages and XRT context lifecycle complete and tested |
| 5–10. Memory model, array, ISA, ctrlcode, end-to-end execution | **not started** |

Full roadmap and acceptance criteria:
[`docs/02-yol-haritasi.md`](docs/02-yol-haritasi.md).

## Real guest validation

For real-guest validation, first complete the `scripts/build-qemu.sh` step from
[`qemu/README.md`](qemu/README.md). A reproducible disposable guest image can
then be built from the host-selected kernel/module/firmware/XRT stack:

```sh
scripts/build-guest-image.sh
set -a; . build/guest/guest.env; set +a
XDNA_QEMU_BINARY=/tmp/xdna-qemu/qemu-system-x86_64 \
  scripts/guest-integration.sh --mode both --debug-driver
```

The image uses a pinned Fedora container userspace. The stock kernel,
`amdxdna.ko`, firmware, and XRT are copied from the host during image creation.
The runner boots each mode in a separate `-snapshot` VM and writes
`summary.json`, a human-readable report, and raw `lspci`, `dmesg`, `xrt-smi`,
QEMU trace, and version evidence under `evidence/<timestamp>/` in the selected
local `build/guest/...` output directory. It does not modify guest kernel or
driver files; force-IOVA mode only reloads `amdxdna` with its stock module
parameter.

Latest validated evidence:
[`docs/evidence/guest-20260908T225000Z.md`](docs/evidence/guest-20260908T225000Z.md).
Raw artifacts remain under the selected local `build/guest/...` output tree,
with separate `normal/` and `force_iova/` directories.

Execution opcodes (`CONFIG_CU`, `EXECUTE_BUFFER_CF`, `EXEC_DPU`,
`CHAIN_EXEC_*`, and `SYNC_BO`) currently **return explicit errors by design**.
Reporting success before the XDNA memory/array execution model exists would
produce incorrect results.

## Verified hardware interface

Every value in `include/xdna/xdna_regs.h` is derived from the publicly
available upstream `amdxdna` driver source. Values that still require hardware
verification are marked with `TODO(verify)`.

Reference document:
[`docs/01-donanim-arayuzu.md`](docs/01-donanim-arayuzu.md).

One critical protocol detail is that each mailbox response size must match the
corresponding `struct <name>_resp` size **exactly**, including error responses.
The stock driver (`xdna_msg_cb`) compares the received size and returns
`-EINVAL` on a mismatch.

## License

GPL-2.0-only. The complete license text is available in [`LICENSE`](LICENSE).
Hardware-interface constants are derived from the publicly available
GPL-2.0-licensed `amdxdna` driver source.
