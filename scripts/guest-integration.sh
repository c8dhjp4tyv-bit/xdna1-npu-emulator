#!/usr/bin/env bash
# Boot a real guest with the integrated QEMU device and collect probe/XRT evidence.
# SPDX-License-Identifier: GPL-2.0-only

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

env_or() {
    local value
    value=$(printenv "$1" 2>/dev/null || true)
    if [[ -n "$value" ]]; then printf '%s' "$value"; else printf '%s' "$2"; fi
}

QEMU_BIN=$(env_or XDNA_QEMU_BINARY "$REPO_ROOT/build/qemu-v11.1.1/qemu-system-x86_64")
QEMU_FIRMWARE_DIR=$(env_or XDNA_QEMU_FIRMWARE_DIR "")
QEMU_BIOS=$(env_or XDNA_QEMU_BIOS "")
DISK_IMAGE=$(env_or XDNA_GUEST_DISK "")
DISK_FORMAT=$(env_or XDNA_GUEST_DISK_FORMAT "")
KERNEL_IMAGE=$(env_or XDNA_GUEST_KERNEL "")
INITRD_IMAGE=$(env_or XDNA_GUEST_INITRD "")
KERNEL_APPEND=$(env_or XDNA_GUEST_APPEND "")
SSH_TARGET=$(env_or XDNA_GUEST_SSH "root@127.0.0.1")
SSH_KEY=$(env_or XDNA_GUEST_SSH_KEY "")
SSH_HOST_FINGERPRINT=$(env_or XDNA_GUEST_SSH_HOST_FINGERPRINT "")
SSH_PORT=$(env_or XDNA_GUEST_SSH_PORT "0")
MEMORY=$(env_or XDNA_GUEST_MEMORY "4G")
SMP=$(env_or XDNA_GUEST_SMP "2")
BOOT_TIMEOUT=$(env_or XDNA_GUEST_BOOT_TIMEOUT "180")
REPEAT_OPENS=$(env_or XDNA_GUEST_REPEAT_OPENS "3")
EVIDENCE_ROOT=$(env_or XDNA_GUEST_EVIDENCE "$REPO_ROOT/evidence/guest")
MODE=$(env_or XDNA_GUEST_MODE "both")
IOMMU=$(env_or XDNA_GUEST_IOMMU "intel")
PCI_DEVICE=$(env_or XDNA_GUEST_PCI_DEVICE "0000:00:02.0")
HELPER_SOURCE=$(env_or XDNA_XRT_HELPER_SOURCE "$REPO_ROOT/tests/xrt_context_probe.c")
HELPER_REMOTE=$(env_or XDNA_XRT_HELPER "")
ATTEMPT_SUSPEND=0
DEBUG_DRIVER=$(env_or XDNA_GUEST_DEBUG_DRIVER "0")
QEMU_EXTRA=()

usage() {
    cat <<'EOF'
Usage: scripts/guest-integration.sh --disk IMAGE [options]

Boot a guest with -device xdna-npu and collect raw driver/XRT/QEMU evidence.
The disk is opened with QEMU -snapshot. amdxdna is loaded from the guest;
guest driver and kernel files are never modified. The default runs both modes;
force_iova reloads the stock module and therefore changes disposable runtime
driver state.

  --qemu PATH          qemu-system-x86_64 (default: build/qemu-v11.1.1/...)
  --firmware-dir PATH  QEMU BIOS/firmware directory (auto-detected when possible)
  --bios PATH          explicit BIOS image (auto-detected when possible)
  --disk PATH          guest disk image (required)
  --disk-format FMT    optional QEMU disk format
  --kernel PATH        boot this guest kernel directly (optional)
  --initrd PATH        initramfs for --kernel
  --append STRING      guest kernel command line for --kernel
  --ssh USER@HOST      SSH target (default: root@127.0.0.1)
  --ssh-key PATH       SSH private key
  --ssh-host-fingerprint SHA256:... expected guest host-key fingerprint
  --ssh-port PORT      host forwarding port (0 chooses a free port)
  --mode MODE          normal, force_iova, or both (default: both)
  --memory SIZE        guest RAM (default: 4G)
  --smp N              guest vCPU count (default: 2)
  --boot-timeout SEC   SSH timeout (default: 180)
  --repeat-opens N     XRT open/close iterations (default: 3)
  --evidence-dir PATH  evidence root (default: evidence/guest)
  --iommu MODE         none, intel, amd, or virtio (default: intel)
  --pci-device BDF     XDNA PCI address for xrt-smi (default: 0000:00:02.0)
  --xrt-helper PATH    already-built helper path inside the guest
  --helper-source PATH upload/build helper (default: tests/xrt_context_probe.c)
  --debug-driver       enable amdxdna dynamic-debug messages in the guest
  --suspend-resume     attempt system suspend/resume (optional and disruptive)
  --qemu-arg ARG       append one extra QEMU argument (repeatable)
  -h, --help           show this help

Exit 0 means all requested modes passed PCI identity, amdxdna probe,
/dev/accel/accel0, xrt-smi examine, XRT handle lifetime, and context
create/destroy. Exit 77 means prerequisites were unavailable; exit 1 means
a run was attempted and failed. No unsupported execution operation is marked
successful.
EOF
}

die() { printf 'guest-integration.sh: %s\n' "$*" >&2; exit 2; }

while (($#)); do
    case "$1" in
        --qemu) (($# >= 2)) || die "--qemu needs a path"; QEMU_BIN=$2; shift 2 ;;
        --firmware-dir) (($# >= 2)) || die "--firmware-dir needs a path"; QEMU_FIRMWARE_DIR=$2; shift 2 ;;
        --bios) (($# >= 2)) || die "--bios needs a path"; QEMU_BIOS=$2; shift 2 ;;
        --disk) (($# >= 2)) || die "--disk needs a path"; DISK_IMAGE=$2; shift 2 ;;
        --disk-format) (($# >= 2)) || die "--disk-format needs a format"; DISK_FORMAT=$2; shift 2 ;;
        --kernel) (($# >= 2)) || die "--kernel needs a path"; KERNEL_IMAGE=$2; shift 2 ;;
        --initrd) (($# >= 2)) || die "--initrd needs a path"; INITRD_IMAGE=$2; shift 2 ;;
        --append) (($# >= 2)) || die "--append needs a command line"; KERNEL_APPEND=$2; shift 2 ;;
        --ssh) (($# >= 2)) || die "--ssh needs USER@HOST"; SSH_TARGET=$2; shift 2 ;;
        --ssh-key) (($# >= 2)) || die "--ssh-key needs a path"; SSH_KEY=$2; shift 2 ;;
        --ssh-host-fingerprint) (($# >= 2)) || die "--ssh-host-fingerprint needs a SHA256 fingerprint"; SSH_HOST_FINGERPRINT=$2; shift 2 ;;
        --ssh-port) (($# >= 2)) || die "--ssh-port needs a port"; SSH_PORT=$2; shift 2 ;;
        --mode) (($# >= 2)) || die "--mode needs a value"; MODE=$2; shift 2 ;;
        --memory) (($# >= 2)) || die "--memory needs a size"; MEMORY=$2; shift 2 ;;
        --smp) (($# >= 2)) || die "--smp needs a count"; SMP=$2; shift 2 ;;
        --boot-timeout) (($# >= 2)) || die "--boot-timeout needs seconds"; BOOT_TIMEOUT=$2; shift 2 ;;
        --repeat-opens) (($# >= 2)) || die "--repeat-opens needs a count"; REPEAT_OPENS=$2; shift 2 ;;
        --evidence-dir) (($# >= 2)) || die "--evidence-dir needs a path"; EVIDENCE_ROOT=$2; shift 2 ;;
        --iommu) (($# >= 2)) || die "--iommu needs a mode"; IOMMU=$2; shift 2 ;;
        --pci-device) (($# >= 2)) || die "--pci-device needs a BDF"; PCI_DEVICE=$2; shift 2 ;;
        --xrt-helper) (($# >= 2)) || die "--xrt-helper needs a guest path"; HELPER_REMOTE=$2; shift 2 ;;
        --helper-source) (($# >= 2)) || die "--helper-source needs a local path"; HELPER_SOURCE=$2; shift 2 ;;
        --debug-driver) DEBUG_DRIVER=1; shift ;;
        --suspend-resume) ATTEMPT_SUSPEND=1; shift ;;
        --qemu-arg) (($# >= 2)) || die "--qemu-arg needs an argument"; QEMU_EXTRA+=("$2"); shift 2 ;;
        -h|--help) usage; exit 0 ;;
        *) die "unknown option: $1 (use --help)" ;;
    esac
done

case "$MODE" in
    normal) MODES=(normal) ;;
    force_iova) MODES=(force_iova) ;;
    both) MODES=(normal force_iova) ;;
    *) die "--mode must be normal, force_iova, or both" ;;
esac

case "$IOMMU" in
    none|intel|amd|virtio) ;;
    *) die "--iommu must be none, intel, amd, or virtio" ;;
esac

[[ "$REPEAT_OPENS" != *$'\n'* && "$REPEAT_OPENS" =~ ^[1-9][0-9]*$ ]] ||
    die "--repeat-opens must be a positive integer"
[[ "$PCI_DEVICE" =~ ^[0-9A-Fa-f]{4}:[0-9A-Fa-f]{2}:[0-9A-Fa-f]{2}\.[0-7]$ ]] ||
    die "--pci-device must be a DDDD:BB:DD.F address"
# Linux exposes PCI BDFs in lower case; normalize a syntactically valid
# upper-case argument before it is used in guest sysfs/lspci commands.
PCI_DEVICE=${PCI_DEVICE,,}
[[ "$SSH_PORT" =~ ^[0-9]+$ ]] && ((SSH_PORT <= 65535)) ||
    die "--ssh-port must be an integer from 0 to 65535"
if [[ -n "$SSH_HOST_FINGERPRINT" &&
      ! "$SSH_HOST_FINGERPRINT" =~ ^SHA256:[A-Za-z0-9+/=]+$ ]]; then
    die "--ssh-host-fingerprint must be a SHA256:... fingerprint"
fi
[[ -n "$SSH_HOST_FINGERPRINT" ]] ||
    die "an expected SSH host-key fingerprint is required (source guest.env)"

command -v python3 >/dev/null || die "python3 is required"
command -v ssh >/dev/null || die "ssh is required"
command -v ssh-keyscan >/dev/null || die "ssh-keyscan is required"
if [[ -z "$HELPER_REMOTE" ]]; then
    command -v scp >/dev/null || die "scp is required to upload the context helper"
fi

RUN_ID=$(date -u +%Y%m%dT%H%M%SZ)
RUN_DIR="$EVIDENCE_ROOT/$RUN_ID"
mkdir -p -- "$RUN_DIR"

skip_preflight() {
    local reason=$1
    cat >"$RUN_DIR/status.env" <<EOF
mode=preflight
overall=skip
reason=$reason
EOF
    python3 - "$RUN_DIR/status.env" "$RUN_DIR/summary.json" <<'PY'
import json
import sys
values = {}
with open(sys.argv[1], encoding="utf-8") as stream:
    for line in stream:
        if "=" in line:
            key, value = line.rstrip("\n").split("=", 1)
            values[key] = value
with open(sys.argv[2], "w", encoding="utf-8") as stream:
    json.dump({"overall": "skip", "status": values}, stream, indent=2)
    stream.write("\n")
PY
    printf 'Guest integration skipped: %s\nEvidence: %s\n' "$reason" "$RUN_DIR"
    printf 'Guest integration skipped\n\nReason: %s\nEvidence: %s\n' "$reason" "$RUN_DIR" >"$RUN_DIR/report.txt"
    exit 77
}

[[ -x "$QEMU_BIN" ]] || skip_preflight "QEMU binary not found: $QEMU_BIN"
[[ -n "$DISK_IMAGE" ]] || skip_preflight "--disk was not supplied"
[[ -f "$DISK_IMAGE" ]] || skip_preflight "guest disk not found: $DISK_IMAGE"
[[ -z "$KERNEL_IMAGE" || -f "$KERNEL_IMAGE" ]] || skip_preflight "guest kernel not found: $KERNEL_IMAGE"
[[ -z "$INITRD_IMAGE" || -f "$INITRD_IMAGE" ]] || skip_preflight "guest initrd not found: $INITRD_IMAGE"
if [[ -z "$HELPER_REMOTE" ]]; then
    [[ -f "$HELPER_SOURCE" ]] || skip_preflight "XRT helper source not found: $HELPER_SOURCE"
fi
[[ -z "$SSH_KEY" || -f "$SSH_KEY" ]] || skip_preflight "SSH key not found: $SSH_KEY"
"$QEMU_BIN" -device help 2>/dev/null | grep -qE '^name "xdna-npu"' || \
    skip_preflight "QEMU binary does not register xdna-npu: $QEMU_BIN"
"$QEMU_BIN" -netdev help 2>/dev/null | grep -qx 'user' || \
    skip_preflight "QEMU binary has no user networking backend (rebuild with --enable-slirp)"

if [[ -n "$QEMU_FIRMWARE_DIR" ]]; then
    [[ -d "$QEMU_FIRMWARE_DIR" ]] || skip_preflight "QEMU firmware directory not found: $QEMU_FIRMWARE_DIR"
else
    for candidate in \
        "$(dirname -- "$QEMU_BIN")/qemu-bundle/usr/local/share/qemu" \
        "$(dirname -- "$QEMU_BIN")/../share/qemu" \
        /usr/share/qemu /usr/share/seabios; do
        if [[ -f "$candidate/kvmvapic.bin" || -f "$candidate/bios-256k.bin" || -f "$candidate/bios.bin" ]]; then
            QEMU_FIRMWARE_DIR=$candidate
            break
        fi
    done
fi
if [[ -z "$QEMU_BIOS" ]]; then
    for candidate in "$QEMU_FIRMWARE_DIR/bios-256k.bin" \
        "$QEMU_FIRMWARE_DIR/bios.bin" /usr/share/seabios/bios-256k.bin; do
        if [[ -f "$candidate" ]]; then
            QEMU_BIOS=$candidate
            break
        fi
    done
fi

find_free_port() {
    python3 - <<'PY'
import socket
s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
s.bind(("127.0.0.1", 0))
print(s.getsockname()[1])
s.close()
PY
}

qemu_running() {
    local pid=$1 state
    kill -0 "$pid" 2>/dev/null || return 1
    # kill -0 also succeeds for a zombie.  A guest result is only accepted
    # while the QEMU process still has a runnable process state.
    if [[ -r "/proc/$pid/stat" ]]; then
        state=$(awk '{print $3}' "/proc/$pid/stat" 2>/dev/null || true)
        [[ "$state" != Z ]]
    fi
}

mode_summary() {
    local directory=$1
    python3 - "$directory/status.env" "$directory/summary.json" <<'PY'
import json
import sys
values = {}
with open(sys.argv[1], encoding="utf-8") as stream:
    for line in stream:
        if "=" in line:
            key, value = line.rstrip("\n").split("=", 1)
            values[key] = value
summary = {
    "mode": values.get("mode"),
    "overall": values.get("overall", "fail"),
    "acceptance": {
        "A_pci_identity": values.get("acceptance_a", "fail"),
        "B_amdxdna_probe": values.get("acceptance_b", "fail"),
        "C_accel_node": values.get("acceptance_c", "fail"),
    },
    "xrt": {
        "examine": values.get("xrt_examine", "fail"),
        "device_open_close": values.get("xrt_open_close", "skip"),
        "context_create_destroy": values.get("xrt_context", "skip"),
        "repetitions": int(values.get("repeat_opens", "0")),
    },
    "force_iova": values.get("force_iova", "not-requested"),
    "qemu_pid_alive": values.get("qemu_pid_alive", "fail"),
    "suspend_resume": values.get("suspend_resume", "not-requested"),
    "stage_1": values.get("stage_1", "pending"),
    "stage_2": values.get("stage_2", "pending"),
    "stage_3_iova_pasid": values.get("stage_3_iova_pasid", "see iova-pasid.txt"),
    "evidence": {
        "lspci": "lspci.txt", "pci_identity": "pci-identity.txt",
        "dmesg_early": "dmesg-amdxdna.txt",
        "dmesg_final": "dmesg-amdxdna-final.txt",
        "xrt_smi": "xrt-smi.txt", "qemu_trace": "qemu.log",
        "qemu_stderr": "qemu-stderr.txt", "qemu_version": "qemu-version.txt",
        "versions": "versions.txt",
        "report": "report.txt",
    },
}
if "reason" in values:
    summary["reason"] = values["reason"]
with open(sys.argv[2], "w", encoding="utf-8") as stream:
    json.dump(summary, stream, indent=2)
    stream.write("\n")
PY
}

mode_report() {
    local directory=$1
    python3 - "$directory/summary.json" "$directory/report.txt" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    summary = json.load(stream)
acceptance = summary.get("acceptance", {})
xrt = summary.get("xrt", {})
with open(sys.argv[2], "w", encoding="utf-8") as stream:
    stream.write("XDNA guest integration report\n=============================\n\n")
    stream.write("Mode: {}\nOverall: {}\nQEMU PID alive: {}\n\n".format(
        summary.get("mode", "unknown"), summary.get("overall", "fail"),
        summary.get("qemu_pid_alive", "fail")))
    stream.write("Acceptance A (PCI 1022:1502 rev 00): {}\n".format(
        acceptance.get("A_pci_identity", "fail")))
    stream.write("Acceptance B (amdxdna entered probe): {}\n".format(
        acceptance.get("B_amdxdna_probe", "fail")))
    stream.write("Acceptance C (/dev/accel/accel0): {}\n\n".format(
        acceptance.get("C_accel_node", "fail")))
    stream.write("xrt-smi examine: {}\nXRT device open/close: {} ({} iterations)\n".format(
        xrt.get("examine", "fail"), xrt.get("device_open_close", "skip"),
        xrt.get("repetitions", 0)))
    stream.write("force_iova path: {}\n".format(summary.get("force_iova", "not-requested")))
    stream.write("XDNA context create/destroy: {}\nSuspend/resume: {}\n\n".format(
        xrt.get("context_create_destroy", "skip"),
        summary.get("suspend_resume", "not-requested")))
    stream.write("Roadmap Stage 1: {}\nRoadmap Stage 2: {}\n".format(
        summary.get("stage_1", "pending"), summary.get("stage_2", "pending")))
    stream.write("Stage 3 IOVA/PASID: {}\n\n".format(
        summary.get("stage_3_iova_pasid", "see iova-pasid.txt")))
    stream.write("Raw command outputs are kept beside this report. The guest driver was not patched.\n")
PY
}

ACTIVE_QEMU_PID=
cleanup_qemu() {
    if [[ -n "$ACTIVE_QEMU_PID" ]] && kill -0 "$ACTIVE_QEMU_PID" 2>/dev/null; then
        kill "$ACTIVE_QEMU_PID" 2>/dev/null || true
        wait "$ACTIVE_QEMU_PID" 2>/dev/null || true
    fi
}
trap cleanup_qemu EXIT INT TERM

run_mode() {
    local mode=$1
    local index=$2
    local directory="$RUN_DIR/$mode"
    local port qemu_pid connected=0 guest_uid=0
    local qemu_pid_alive=fail
    local known_hosts="$directory/ssh-known-hosts"
    local ssh_host=${SSH_TARGET#*@}
    local scan_fingerprint=
    local acceptance_a=fail acceptance_b=fail acceptance_c=fail
    local xrt_examine=fail xrt_open_close=skip xrt_context=skip
    local force_iova_status=not-requested
    local suspend_resume=not-requested stage_1=pending stage_2=pending
    local stage_3_iova_pasid=unobserved helper_build_rc=1 helper_rc=1
    mkdir -p -- "$directory"
    if [[ "$SSH_PORT" == 0 ]]; then port=$(find_free_port); else port=$((SSH_PORT + index)); fi

    local -a ssh_cmd=(ssh -o BatchMode=yes -o ConnectTimeout=3
        -o ConnectionAttempts=1 -o StrictHostKeyChecking=yes
        -o "UserKnownHostsFile=$known_hosts"
        -o GlobalKnownHostsFile=/dev/null -p "$port")
    [[ -z "$SSH_KEY" ]] || ssh_cmd+=(-i "$SSH_KEY")
    guest_exec() { "${ssh_cmd[@]}" "$SSH_TARGET" "$1"; }
    guest_root_exec() {
        local command=$1
        if [[ "$guest_uid" == 0 ]]; then guest_exec "$command"; else
            guest_exec "sudo -n sh -c $(printf '%q' "$command")"
        fi
    }
    capture_guest() {
        local output=$1 root=$2 command=$3 rc
        set +e
        if [[ "$root" == 1 ]]; then guest_root_exec "$command" >"$output" 2>&1; else
            guest_exec "$command" >"$output" 2>&1
        fi
        rc=$?
        set -e
        printf '%s\n' "$rc" >"$output.rc"
    }

    local -a qemu_args=(
        -machine q35,accel=tcg -nodefaults -display none -no-reboot
        -m "$MEMORY" -smp "$SMP" -serial "file:$directory/qemu-serial.log"
        -D "$directory/qemu.log" -d 'guest_errors,trace:xdna_npu_*' -snapshot
    )
    case "$IOMMU" in
        intel)
            # amdxdna needs a guest IOMMU domain and PASID/SVA-capable
            # translation.  Scalable VT-d exposes that path to stock Linux.
            qemu_args+=(-device
                'intel-iommu,intremap=on,device-iotlb=on,caching-mode=on,svm=on,pasid-bits=20,scalable-mode=on,fsts=on')
            ;;
        amd) qemu_args+=(-device amd-iommu,intremap=on) ;;
        virtio) qemu_args+=(-device virtio-iommu-pci) ;;
        none) ;;
    esac
    [[ -z "$QEMU_FIRMWARE_DIR" ]] || qemu_args=(-L "$QEMU_FIRMWARE_DIR" "${qemu_args[@]}")
    [[ -z "$QEMU_BIOS" ]] || qemu_args=(-bios "$QEMU_BIOS" "${qemu_args[@]}")
    if [[ -n "$KERNEL_IMAGE" ]]; then
        qemu_args+=(-kernel "$KERNEL_IMAGE")
        [[ -z "$INITRD_IMAGE" ]] || qemu_args+=(-initrd "$INITRD_IMAGE")
        local kernel_append=$KERNEL_APPEND
        if [[ "$IOMMU" == intel && "$kernel_append" != *intel_iommu=* ]]; then
            [[ -z "$kernel_append" ]] || kernel_append+=" "
            kernel_append+="intel_iommu=on,sm_on"
        fi
        if [[ "$IOMMU" == intel && "$kernel_append" != iommu=* &&
              "$kernel_append" != *' iommu='* ]]; then
            # The validated QEMU 11.1.1 run uses translated Intel VT-d with
            # coherent SVM.  Keep that observed default unless the caller
            # explicitly selects another kernel IOMMU mode.
            [[ -z "$kernel_append" ]] || kernel_append+=" "
            kernel_append+="iommu=on"
        fi
        if [[ "$mode" == force_iova ]]; then
            if [[ -n "$kernel_append" ]]; then kernel_append+=" "; fi
            kernel_append+="amdxdna.force_iova=1"
        fi
        [[ -z "$kernel_append" ]] || qemu_args+=(-append "$kernel_append")
    fi
    if [[ -n "$DISK_FORMAT" ]]; then qemu_args+=(-drive "file=$DISK_IMAGE,if=virtio,format=$DISK_FORMAT")
    else qemu_args+=(-drive "file=$DISK_IMAGE,if=virtio"); fi
    qemu_args+=(
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$port-:22"
        -device virtio-net-pci,netdev=net0,romfile= -device xdna-npu "${QEMU_EXTRA[@]}"
    )
    printf '%q ' "$QEMU_BIN" "${qemu_args[@]}" >"$directory/qemu-command.txt"
    printf '\n' >>"$directory/qemu-command.txt"
    "$QEMU_BIN" --version >"$directory/qemu-version.txt" 2>&1 || true

    set +e
    "$QEMU_BIN" "${qemu_args[@]}" >"$directory/qemu-stderr.txt" 2>&1 &
    qemu_pid=$!
    set -e
    ACTIVE_QEMU_PID=$qemu_pid
    printf '%s\n%s\n' "$qemu_pid" "$port" >"$directory/qemu.pid-port.txt"

    local elapsed=0
    while ((elapsed < BOOT_TIMEOUT)); do
        if ! qemu_running "$qemu_pid"; then break; fi
        if [[ ! -s "$known_hosts" ]]; then
            set +e
            ssh-keyscan -T 3 -t ed25519 -p "$port" "$ssh_host" >"$known_hosts.tmp" \
                2>"$directory/ssh-keyscan.txt"
            local keyscan_rc=$?
            set -e
            if ((keyscan_rc == 0)) && [[ -s "$known_hosts.tmp" ]]; then
                scan_fingerprint=$(ssh-keygen -lf "$known_hosts.tmp" -E sha256 2>/dev/null |
                    awk 'NR == 1 { print $2; exit }')
                if [[ -n "$SSH_HOST_FINGERPRINT" &&
                      "$scan_fingerprint" != "$SSH_HOST_FINGERPRINT" ]]; then
                    printf 'SSH host-key fingerprint mismatch: expected %s, got %s\n' \
                        "$SSH_HOST_FINGERPRINT" "$scan_fingerprint" \
                        >"$directory/ssh-key-fingerprint-error.txt"
                    rm -f -- "$known_hosts.tmp"
                else
                    mv -- "$known_hosts.tmp" "$known_hosts"
                    chmod 0600 "$known_hosts"
                    printf 'expected=%s\nobserved=%s\n' \
                        "${SSH_HOST_FINGERPRINT:-session-pinned}" "$scan_fingerprint" \
                        >"$directory/ssh-host-fingerprint.txt"
                fi
            else
                rm -f -- "$known_hosts.tmp"
            fi
        fi
        if [[ -s "$known_hosts" ]] && guest_exec true >/dev/null 2>&1 &&
           qemu_running "$qemu_pid"; then
            connected=1
            qemu_pid_alive=pass
            break
        fi
        sleep 1
        elapsed=$((elapsed + 1))
    done
    if ((connected == 0)); then
        printf 'SSH did not become ready after %s seconds.\n' "$elapsed" >"$directory/connection-error.txt"
        cat >"$directory/status.env" <<EOF
mode=$mode
overall=fail
acceptance_a=fail
acceptance_b=fail
acceptance_c=fail
xrt_examine=fail
xrt_open_close=skip
xrt_context=skip
force_iova=not-reached
repeat_opens=$REPEAT_OPENS
qemu_pid_alive=fail
suspend_resume=not-reached
stage_1=pending
stage_2=pending
stage_3_iova_pasid=unobserved
EOF
        mode_summary "$directory"; mode_report "$directory"; cleanup_qemu; ACTIVE_QEMU_PID=; return 1
    fi

    set +e
    guest_uid=$(guest_exec id -u 2>/dev/null)
    local uid_rc=$?
    set -e
    if ((uid_rc != 0)) || [[ ! "$guest_uid" =~ ^[0-9]+$ ]]; then guest_uid=0; fi

    printf 'ssh_host=%s\n' "$ssh_host" >"$directory/ssh-endpoint.txt"
    printf 'expected_host_fingerprint=%s\n' "${SSH_HOST_FINGERPRINT:-session-pinned}" \
        >>"$directory/ssh-endpoint.txt"
    printf 'qemu_pid_alive=%s\n' "$qemu_pid_alive" >>"$directory/ssh-endpoint.txt"
    capture_guest "$directory/prerequisites.txt" 0 \
        'set -eu; if command -v lspci >/dev/null 2>&1; then printf "lspci: "; command -v lspci; else printf "lspci: missing\n"; exit 127; fi; printf "xrt-smi: "; if command -v xrt-smi >/dev/null 2>&1 || test -x /opt/xilinx/xrt/bin/xrt-smi; then command -v xrt-smi 2>/dev/null || printf "/opt/xilinx/xrt/bin/xrt-smi\n"; else echo missing; exit 127; fi; printf "amdxdna-module: "; if modinfo amdxdna >/dev/null 2>&1 || test -e /sys/module/amdxdna; then echo present; else echo missing; exit 127; fi'
    prereq_rc=$(<"$directory/prerequisites.txt.rc")
    if [[ "$prereq_rc" == 127 ]]; then
        printf 'Required guest prerequisites are unavailable; dmesg remains diagnostic-only.\n' \
            >"$directory/connection-error.txt"
        cat >"$directory/status.env" <<EOF
mode=$mode
overall=skip
acceptance_a=skip
acceptance_b=skip
acceptance_c=skip
xrt_examine=skip
xrt_open_close=skip
xrt_context=skip
force_iova=not-reached
repeat_opens=$REPEAT_OPENS
qemu_pid_alive=$qemu_pid_alive
suspend_resume=not-reached
stage_1=pending
stage_2=pending
stage_3_iova_pasid=unobserved
EOF
        mode_summary "$directory"; mode_report "$directory"; cleanup_qemu; ACTIVE_QEMU_PID=; return 77
    elif [[ "$prereq_rc" != 0 ]]; then
        printf 'Guest prerequisite command failed with status %s; dmesg is diagnostic-only.\n' \
            "$prereq_rc" >"$directory/connection-error.txt"
        cat >"$directory/status.env" <<EOF
mode=$mode
overall=fail
acceptance_a=fail
acceptance_b=fail
acceptance_c=fail
xrt_examine=fail
xrt_open_close=skip
xrt_context=skip
force_iova=not-reached
repeat_opens=$REPEAT_OPENS
qemu_pid_alive=$qemu_pid_alive
suspend_resume=not-reached
stage_1=pending
stage_2=pending
stage_3_iova_pasid=unobserved
EOF
        mode_summary "$directory"; mode_report "$directory"; cleanup_qemu; ACTIVE_QEMU_PID=; return 1
    fi

    capture_guest "$directory/versions.txt" 0 \
        'printf "uname: "; uname -a; printf "kernel-release: "; uname -r; printf "os-release:\n"; cat /etc/os-release 2>/dev/null || true; printf "amdxdna-modinfo:\n"; if command -v modinfo >/dev/null 2>&1; then modinfo amdxdna 2>&1 || true; else echo modinfo-unavailable; fi; printf "xrt-smi-version:\n"; if command -v xrt-smi >/dev/null 2>&1; then xrt-smi --version 2>&1 || true; elif [ -x /opt/xilinx/xrt/bin/xrt-smi ]; then /opt/xilinx/xrt/bin/xrt-smi --version 2>&1 || true; else echo xrt-smi-unavailable; fi'

    local module_arg=''
    [[ "$mode" != force_iova ]] || module_arg=' force_iova=1'
    capture_guest "$directory/amdxdna-load.txt" 1 "modprobe -r amdxdna 2>/dev/null || true; modprobe amdxdna$module_arg"
    if [[ "$mode" == force_iova ]]; then
        capture_guest "$directory/force-iova.txt" 1 \
            'printf "module-parameter: "; cat /sys/module/amdxdna/parameters/force_iova 2>&1 || true; printf "cmdline: "; cat /proc/cmdline'
        if grep -Eq 'module-parameter:[[:space:]]*1([[:space:]]|$)|amdxdna\.force_iova=1' "$directory/force-iova.txt"; then
            force_iova_status=pass
        else
            force_iova_status=fail
        fi
    else
        force_iova_status=normal
        printf 'module-parameter: normal (force_iova not requested)\n' >"$directory/force-iova.txt"
    fi

    capture_guest "$directory/lspci.txt" 0 \
        "lspci -Dnnk 2>&1; printf '\n--- verbose ---\n'; lspci -Dvmmnnk 2>&1 || true; printf '\n--- capabilities ---\n'; lspci -Dvvnnk -s '$PCI_DEVICE' 2>&1 || true; printf '\n--- config revision ---\n'; printf 'setpci-revision: '; setpci -s '$PCI_DEVICE' REVISION 2>&1 || true; printf 'sysfs-revision: '; od -An -tx1 -j8 -N1 /sys/bus/pci/devices/'$PCI_DEVICE'/config 2>&1 | tr -d ' \n'; printf '\n--- config space ---\n'; od -Ax -tx1 -v /sys/bus/pci/devices/'$PCI_DEVICE'/config 2>&1 || true"
    capture_guest "$directory/pci-identity.txt" 0 \
        "set -eu; printf 'bdf=%s\\n' '$PCI_DEVICE'; printf 'vendor='; cat /sys/bus/pci/devices/'$PCI_DEVICE'/vendor; printf 'device='; cat /sys/bus/pci/devices/'$PCI_DEVICE'/device; printf 'revision='; cat /sys/bus/pci/devices/'$PCI_DEVICE'/revision; printf '\\n--- selected lspci ---\\n'; lspci -Dnnk -s '$PCI_DEVICE' 2>&1; printf '\\n--- accel node target ---\\n'; printf 'accel0-bdf='; readlink -f /sys/class/accel/accel0/device 2>/dev/null | sed 's#^.*/##' || true"
    if awk -v bdf="$PCI_DEVICE" 'index(tolower($0), tolower(bdf)) == 1 && $0 ~ /\[1022:1502\]/ { found=1 } END { exit !found }' \
           "$directory/pci-identity.txt" &&
       grep -Eiq '^vendor=0x1022$' "$directory/pci-identity.txt" &&
       grep -Eiq '^device=0x1502$' "$directory/pci-identity.txt" &&
       grep -Eiq '^revision=0x00$' "$directory/pci-identity.txt"; then
        acceptance_a=pass; printf 'PASS: selected %s is 1022:1502 revision 00\n' "$PCI_DEVICE" >"$directory/lspci-check.txt"
    elif grep -Eiq '1022:1502' "$directory/pci-identity.txt"; then
        printf 'FAIL: device ID found but revision 00 was not observed\n' >"$directory/lspci-check.txt"
    else
        printf 'FAIL: AMD XDNA1 PCI identity 1022:1502 was not observed\n' >"$directory/lspci-check.txt"
    fi

    capture_guest "$directory/dmesg-amdxdna.txt" 1 'dmesg --color=never 2>&1 | grep -iE "amdxdna|xdna|aie" || true'
    if grep -Eiq 'Kernel driver in use:[[:space:]]*amdxdna' \
           "$directory/pci-identity.txt" ||
       grep -Eiq 'amdxdna.*(probe|firmware|hardware|device)|(probe|firmware|hardware).*(amdxdna|xdna)' \
           "$directory/dmesg-amdxdna.txt"; then
        acceptance_b=pass
    fi
    capture_guest "$directory/accel-node.txt" 1 \
        'if test -e /dev/accel/accel0; then stat /dev/accel/accel0; else echo /dev/accel/accel0-missing; exit 1; fi'
    if [[ "$(<"$directory/accel-node.txt.rc")" == 0 ]] &&
       ! grep -q 'missing' "$directory/accel-node.txt" &&
       [[ -s "$directory/accel-node.txt" ]] &&
       grep -Fxq "accel0-bdf=$PCI_DEVICE" "$directory/pci-identity.txt"; then
        acceptance_c=pass
    fi

    capture_guest "$directory/xrt-smi.txt" 0 \
        "if command -v xrt-smi >/dev/null 2>&1; then xrt-smi --batch examine --device '$PCI_DEVICE' --report all; elif [ -x /opt/xilinx/xrt/bin/xrt-smi ]; then /opt/xilinx/xrt/bin/xrt-smi --batch examine --device '$PCI_DEVICE' --report all; else echo xrt-smi-missing; exit 127; fi"
    [[ "$(<"$directory/xrt-smi.txt.rc")" == 0 ]] && xrt_examine=pass || true
    capture_guest "$directory/xrt-smi.json" 0 \
        "if command -v xrt-smi >/dev/null 2>&1; then xrt-smi --batch examine --device '$PCI_DEVICE' --report all -f JSON; elif [ -x /opt/xilinx/xrt/bin/xrt-smi ]; then /opt/xilinx/xrt/bin/xrt-smi --batch examine --device '$PCI_DEVICE' --report all -f JSON; else echo xrt-smi-missing; exit 127; fi"

    capture_guest "$directory/iova-pasid.txt" 1 \
        'printf "cmdline:\n"; cat /proc/cmdline; printf "force_iova:\n"; cat /sys/module/amdxdna/parameters/force_iova 2>&1 || true; printf "dmesg-iova-pasid:\n"; dmesg --color=never 2>&1 | grep -iE "iova|pasid|sva|iommu|force_iova" || true'
    if grep -Eiq 'pasid[[:space:]:=]+(0x)?[0-9a-f]+|iommu.*(attach|domain|enabled|success)|sva.*(bind|success)' \
           "$directory/iova-pasid.txt"; then
        stage_3_iova_pasid=observed-in-evidence
    elif [[ "$mode" == force_iova && "$force_iova_status" == pass ]]; then
        stage_3_iova_pasid=force_iova-selected
    fi

    local helper_src="/tmp/xdna-xrt-context-$RUN_ID-$mode.c"
    local helper_bin="/tmp/xdna-xrt-context-$RUN_ID-$mode"
    if [[ -n "$HELPER_REMOTE" ]]; then
        helper_build_rc=0; helper_bin=$HELPER_REMOTE
        printf 'Using guest helper: %s\n' "$HELPER_REMOTE" >"$directory/xrt-helper-build.txt"
    else
        local -a scp_opts=(-o BatchMode=yes -o ConnectTimeout=5
            -o StrictHostKeyChecking=yes -o "UserKnownHostsFile=$known_hosts"
            -o GlobalKnownHostsFile=/dev/null -P "$port")
        [[ -z "$SSH_KEY" ]] || scp_opts+=(-i "$SSH_KEY")
        set +e
        scp "${scp_opts[@]}" "$HELPER_SOURCE" "$SSH_TARGET:$helper_src" >"$directory/xrt-helper-build.txt" 2>&1
        helper_build_rc=$?
        set -e
        if ((helper_build_rc == 0)); then
            set +e
            guest_exec "cc -std=c11 -Wall -Wextra -Werror -I/opt/xilinx/xrt/include -I/usr/include '$helper_src' -L/opt/xilinx/xrt/lib64 -Wl,-rpath,/opt/xilinx/xrt/lib64 -lxrt_coreutil -o '$helper_bin'" >>"$directory/xrt-helper-build.txt" 2>&1
            helper_build_rc=$?
            if ((helper_build_rc != 0)); then
                guest_exec "cc -std=c11 -Wall -Wextra -Werror -I/opt/xilinx/xrt/include -I/usr/include '$helper_src' -L/usr/lib64 -L/usr/lib -L/opt/xilinx/xrt/lib64 -Wl,-rpath,/opt/xilinx/xrt/lib64 -lxrt_coreutil -o '$helper_bin'" >>"$directory/xrt-helper-build.txt" 2>&1
                helper_build_rc=$?
            fi
            set -e
        fi
    fi
    if ((helper_build_rc == 0)); then
        if [[ "$DEBUG_DRIVER" == 1 ]]; then
            capture_guest "$directory/amdxdna-dynamic-debug.txt" 1 \
                'if test -d /sys/kernel/debug; then mountpoint -q /sys/kernel/debug || mount -t debugfs debugfs /sys/kernel/debug 2>/dev/null || true; printf "module amdxdna +p\n" > /sys/kernel/debug/dynamic_debug/control 2>&1 || true; echo enabled; else echo debugfs-missing; fi'
        else
            printf 'not-requested (pass --debug-driver to enable amdxdna dynamic debug)\n' \
                >"$directory/amdxdna-dynamic-debug.txt"
            printf '0\n' >"$directory/amdxdna-dynamic-debug.txt.rc"
        fi
        capture_guest "$directory/xrt-open-close-context.txt" 1 "$helper_bin /dev/accel/accel0 $REPEAT_OPENS"
        helper_rc=$(<"$directory/xrt-open-close-context.txt.rc")
        if grep -q '^xrt_open_close=fail' "$directory/xrt-open-close-context.txt"; then xrt_open_close=fail
        elif grep -q '^xrt_open_close\[' "$directory/xrt-open-close-context.txt"; then xrt_open_close=pass
        else xrt_open_close=fail; fi
        grep -q '^context_lifetime=ok' "$directory/xrt-open-close-context.txt" && xrt_context=pass || xrt_context=fail
    else
        printf 'not-enabled because context helper did not build\n' >"$directory/amdxdna-dynamic-debug.txt"
        printf '0\n' >"$directory/amdxdna-dynamic-debug.txt.rc"
        printf 'Context helper unavailable (build rc %s); no success is inferred.\n' "$helper_build_rc" >"$directory/xrt-open-close-context.txt"
        printf '%s\n' "$helper_build_rc" >"$directory/xrt-open-close-context.txt.rc"
    fi

    # Capture the complete post-XRT kernel path as well as the early probe
    # snapshot above. Context creation errors are often emitted only after
    # the DRM ioctl returns, so the final log is authoritative post-XRT
    # diagnostic evidence; Acceptance B is intentionally decided from the
    # preserved early snapshot above.
    capture_guest "$directory/dmesg-amdxdna-final.txt" 1 \
        'dmesg --color=never 2>&1 | grep -iE "amdxdna|xdna|aie" || true'

    if ((ATTEMPT_SUSPEND == 1)); then
        capture_guest "$directory/suspend-resume.txt" 1 \
            'if command -v systemctl >/dev/null 2>&1 && systemctl is-system-running >/dev/null 2>&1; then echo suspend-requested; systemctl suspend; else echo suspend-not-supported; exit 77; fi'
        local suspend_rc=$(<"$directory/suspend-resume.txt.rc")
        if [[ "$suspend_rc" == 0 ]]; then suspend_resume=pass; elif [[ "$suspend_rc" == 77 ]]; then suspend_resume=skip; else suspend_resume=fail; fi
    else
        printf 'not-requested (pass --suspend-resume to attempt system suspend)\n' >"$directory/suspend-resume.txt"
        printf '0\n' >"$directory/suspend-resume.txt.rc"
    fi

    if ! qemu_running "$qemu_pid"; then
        qemu_pid_alive=fail
        printf 'QEMU PID %s exited before acceptance was finalized.\n' "$qemu_pid" \
            >"$directory/qemu-liveness-error.txt"
    else
        qemu_pid_alive=pass
    fi

    local overall=fail
    if [[ "$acceptance_a" == pass && "$acceptance_b" == pass && "$acceptance_c" == pass &&
          "$xrt_examine" == pass && "$xrt_open_close" == pass && "$xrt_context" == pass &&
          "$force_iova_status" != fail && "$qemu_pid_alive" == pass ]]; then
        overall=pass; stage_1=complete; stage_2=complete-on-real-guest
    fi
    cat >"$directory/status.env" <<EOF
mode=$mode
overall=$overall
acceptance_a=$acceptance_a
acceptance_b=$acceptance_b
acceptance_c=$acceptance_c
xrt_examine=$xrt_examine
xrt_open_close=$xrt_open_close
xrt_context=$xrt_context
force_iova=$force_iova_status
repeat_opens=$REPEAT_OPENS
qemu_pid_alive=$qemu_pid_alive
suspend_resume=$suspend_resume
stage_1=$stage_1
stage_2=$stage_2
stage_3_iova_pasid=$stage_3_iova_pasid
EOF
    mode_summary "$directory"; mode_report "$directory"; cleanup_qemu; ACTIVE_QEMU_PID=
    [[ "$overall" == pass ]]
}

MODE_DIRS=()
FAILURES=0
SKIPS=0
for index in "${!MODES[@]}"; do
    mode=${MODES[$index]}; MODE_DIRS+=("$RUN_DIR/$mode")
    if run_mode "$mode" "$index"; then
        :
    else
        mode_rc=$?
        if ((mode_rc == 77)); then SKIPS=$((SKIPS + 1)); else FAILURES=$((FAILURES + 1)); fi
    fi
done

python3 - "$RUN_DIR/summary.json" "${MODE_DIRS[@]}" <<'PY'
import json
import os
import sys
result = {"run": os.path.basename(os.path.dirname(sys.argv[1])), "modes": []}
for directory in sys.argv[2:]:
    with open(os.path.join(directory, "summary.json"), encoding="utf-8") as stream:
        result["modes"].append(json.load(stream))
if result["modes"] and all(item.get("overall") == "pass" for item in result["modes"]):
    result["overall"] = "pass"
elif result["modes"] and all(item.get("overall") == "skip" for item in result["modes"]):
    result["overall"] = "skip"
else:
    result["overall"] = "fail"
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(result, stream, indent=2)
    stream.write("\n")
PY

python3 - "$RUN_DIR/summary.json" "$RUN_DIR/report.txt" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    result = json.load(stream)
with open(sys.argv[2], "w", encoding="utf-8") as stream:
    stream.write("XDNA guest integration run {}\n================================\n\n".format(result["run"]))
    for item in result["modes"]:
        acceptance = item.get("acceptance", {})
        xrt = item.get("xrt", {})
        stream.write("{}: {} (A={} B={} C={}, xrt-smi={}, open={}, context={})\n".format(
            item.get("mode", "unknown"), item.get("overall", "fail"),
            acceptance.get("A_pci_identity", "fail"),
            acceptance.get("B_amdxdna_probe", "fail"),
            acceptance.get("C_accel_node", "fail"),
            xrt.get("examine", "fail"),
            xrt.get("device_open_close", "skip"),
            xrt.get("context_create_destroy", "skip")))
    stream.write("\nMachine-readable summary: summary.json\n"
                 "Per-mode reports: {normal,force_iova}/report.txt\n")
PY
printf 'Guest integration evidence: %s\n' "$RUN_DIR"
if ((FAILURES != 0)); then
    exit 1
elif ((SKIPS != 0)); then
    exit 77
fi
