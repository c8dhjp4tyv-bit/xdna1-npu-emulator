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
SSH_PORT=$(env_or XDNA_GUEST_SSH_PORT "0")
MEMORY=$(env_or XDNA_GUEST_MEMORY "4G")
SMP=$(env_or XDNA_GUEST_SMP "2")
BOOT_TIMEOUT=$(env_or XDNA_GUEST_BOOT_TIMEOUT "180")
REPEAT_OPENS=$(env_or XDNA_GUEST_REPEAT_OPENS "3")
EVIDENCE_ROOT=$(env_or XDNA_GUEST_EVIDENCE "$REPO_ROOT/evidence/guest")
MODE=$(env_or XDNA_GUEST_MODE "both")
HELPER_SOURCE=$(env_or XDNA_XRT_HELPER_SOURCE "$REPO_ROOT/tests/xrt_context_probe.c")
HELPER_REMOTE=$(env_or XDNA_XRT_HELPER "")
ATTEMPT_SUSPEND=0
QEMU_EXTRA=()

usage() {
    cat <<'EOF'
Usage: scripts/guest-integration.sh --disk IMAGE [options]

Boot a guest with -device xdna-npu and collect raw driver/XRT/QEMU evidence.
The disk is opened with QEMU -snapshot. amdxdna is loaded from the guest;
the guest driver and kernel are never modified. The default runs both modes.

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
  --ssh-port PORT      host forwarding port (0 chooses a free port)
  --mode MODE          normal, force_iova, or both (default: both)
  --memory SIZE        guest RAM (default: 4G)
  --smp N              guest vCPU count (default: 2)
  --boot-timeout SEC   SSH timeout (default: 180)
  --repeat-opens N     XRT open/close iterations (default: 3)
  --evidence-dir PATH  evidence root (default: evidence/guest)
  --xrt-helper PATH    already-built helper path inside the guest
  --helper-source PATH upload/build helper (default: tests/xrt_context_probe.c)
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
        --ssh-port) (($# >= 2)) || die "--ssh-port needs a port"; SSH_PORT=$2; shift 2 ;;
        --mode) (($# >= 2)) || die "--mode needs a value"; MODE=$2; shift 2 ;;
        --memory) (($# >= 2)) || die "--memory needs a size"; MEMORY=$2; shift 2 ;;
        --smp) (($# >= 2)) || die "--smp needs a count"; SMP=$2; shift 2 ;;
        --boot-timeout) (($# >= 2)) || die "--boot-timeout needs seconds"; BOOT_TIMEOUT=$2; shift 2 ;;
        --repeat-opens) (($# >= 2)) || die "--repeat-opens needs a count"; REPEAT_OPENS=$2; shift 2 ;;
        --evidence-dir) (($# >= 2)) || die "--evidence-dir needs a path"; EVIDENCE_ROOT=$2; shift 2 ;;
        --xrt-helper) (($# >= 2)) || die "--xrt-helper needs a guest path"; HELPER_REMOTE=$2; shift 2 ;;
        --helper-source) (($# >= 2)) || die "--helper-source needs a local path"; HELPER_SOURCE=$2; shift 2 ;;
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

command -v python3 >/dev/null || die "python3 is required"
command -v ssh >/dev/null || die "ssh is required"
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
    "suspend_resume": values.get("suspend_resume", "not-requested"),
    "stage_1": values.get("stage_1", "pending"),
    "stage_2": values.get("stage_2", "pending"),
    "stage_3_iova_pasid": values.get("stage_3_iova_pasid", "see iova-pasid.txt"),
    "evidence": {
        "lspci": "lspci.txt", "dmesg": "dmesg-amdxdna.txt",
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
    # shellcheck disable=SC1090
    source "$directory/status.env"
    {
        printf 'XDNA guest integration report\n=============================\n\n'
        printf 'Mode: %s\nOverall: %s\n\n' "$mode" "$overall"
        printf 'Acceptance A (PCI 1022:1502 rev 00): %s\n' "$acceptance_a"
        printf 'Acceptance B (amdxdna entered probe): %s\n' "$acceptance_b"
        printf 'Acceptance C (/dev/accel/accel0): %s\n\n' "$acceptance_c"
        printf 'xrt-smi examine: %s\nXRT device open/close: %s (%s iterations)\n' \
            "$xrt_examine" "$xrt_open_close" "$repeat_opens"
        printf 'force_iova path: %s\n' "$force_iova"
        printf 'XDNA context create/destroy: %s\nSuspend/resume: %s\n\n' \
            "$xrt_context" "$suspend_resume"
        printf 'Roadmap Stage 1: %s\nRoadmap Stage 2: %s\n' "$stage_1" "$stage_2"
        printf 'Stage 3 IOVA/PASID: %s\n\n' "$stage_3_iova_pasid"
        printf 'Raw command outputs are kept beside this report. The guest driver was not patched.\n'
    } >"$directory/report.txt"
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
    local acceptance_a=fail acceptance_b=fail acceptance_c=fail
    local xrt_examine=fail xrt_open_close=skip xrt_context=skip
    local force_iova_status=not-requested
    local suspend_resume=not-requested stage_1=pending stage_2=pending
    local stage_3_iova_pasid=unobserved helper_build_rc=1 helper_rc=1
    mkdir -p -- "$directory"
    if [[ "$SSH_PORT" == 0 ]]; then port=$(find_free_port); else port=$((SSH_PORT + index)); fi

    local -a ssh_cmd=(ssh -o BatchMode=yes -o ConnectTimeout=3
        -o ConnectionAttempts=1 -o StrictHostKeyChecking=no
        -o UserKnownHostsFile=/dev/null -p "$port")
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
    [[ -z "$QEMU_FIRMWARE_DIR" ]] || qemu_args=(-L "$QEMU_FIRMWARE_DIR" "${qemu_args[@]}")
    [[ -z "$QEMU_BIOS" ]] || qemu_args=(-bios "$QEMU_BIOS" "${qemu_args[@]}")
    if [[ -n "$KERNEL_IMAGE" ]]; then
        qemu_args+=(-kernel "$KERNEL_IMAGE")
        [[ -z "$INITRD_IMAGE" ]] || qemu_args+=(-initrd "$INITRD_IMAGE")
        local kernel_append=$KERNEL_APPEND
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
        if guest_exec true >/dev/null 2>&1; then connected=1; break; fi
        if ! kill -0 "$qemu_pid" 2>/dev/null; then break; fi
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

    capture_guest "$directory/lspci.txt" 0 'lspci -Dnnk 2>&1; printf "\n--- verbose ---\n"; lspci -Dvmmnnk 2>&1 || true'
    if grep -Eiq '1022:1502.*\(rev[[:space:]]*00\)' "$directory/lspci.txt"; then
        acceptance_a=pass; printf 'PASS: 1022:1502 revision 00\n' >"$directory/lspci-check.txt"
    elif grep -Eiq '1022:1502' "$directory/lspci.txt"; then
        printf 'FAIL: device ID found but revision 00 was not observed\n' >"$directory/lspci-check.txt"
    else
        printf 'FAIL: AMD XDNA1 PCI identity 1022:1502 was not observed\n' >"$directory/lspci-check.txt"
    fi

    capture_guest "$directory/dmesg-amdxdna.txt" 1 'dmesg --color=never 2>&1 | grep -iE "amdxdna|xdna|aie" || true'
    if grep -Eiq 'Kernel driver in use:[[:space:]]*amdxdna' \
           "$directory/lspci.txt" ||
       grep -Eiq 'amdxdna.*(probe|firmware|hardware|device)|(probe|firmware|hardware).*(amdxdna|xdna)' \
           "$directory/dmesg-amdxdna.txt"; then
        acceptance_b=pass
    fi
    capture_guest "$directory/accel-node.txt" 1 \
        'if test -e /dev/accel/accel0; then stat /dev/accel/accel0; else echo /dev/accel/accel0-missing; exit 1; fi'
    if [[ "$(<"$directory/accel-node.txt.rc")" == 0 ]] &&
       ! grep -q 'missing' "$directory/accel-node.txt" &&
       [[ -s "$directory/accel-node.txt" ]]; then
        acceptance_c=pass
    fi

    capture_guest "$directory/xrt-smi.txt" 0 \
        'if command -v xrt-smi >/dev/null 2>&1; then xrt-smi --batch examine --report all; elif [ -x /opt/xilinx/xrt/bin/xrt-smi ]; then /opt/xilinx/xrt/bin/xrt-smi --batch examine --report all; else echo xrt-smi-missing; exit 127; fi'
    [[ "$(<"$directory/xrt-smi.txt.rc")" == 0 ]] && xrt_examine=pass || true
    capture_guest "$directory/xrt-smi.json" 0 \
        'if command -v xrt-smi >/dev/null 2>&1; then xrt-smi --batch examine --report all -f JSON; elif [ -x /opt/xilinx/xrt/bin/xrt-smi ]; then /opt/xilinx/xrt/bin/xrt-smi --batch examine --report all -f JSON; else echo xrt-smi-missing; exit 127; fi'

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
            -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -P "$port")
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
                guest_exec "cc -std=c11 -Wall -Wextra -Werror -I/usr/include '$helper_src' -L/usr/lib64 -L/usr/lib -lxrt_coreutil -o '$helper_bin'" >>"$directory/xrt-helper-build.txt" 2>&1
                helper_build_rc=$?
            fi
            set -e
        fi
    fi
    if ((helper_build_rc == 0)); then
        capture_guest "$directory/xrt-open-close-context.txt" 1 "$helper_bin /dev/accel/accel0 $REPEAT_OPENS"
        helper_rc=$(<"$directory/xrt-open-close-context.txt.rc")
        if grep -q '^xrt_open_close=fail' "$directory/xrt-open-close-context.txt"; then xrt_open_close=fail
        elif grep -q '^xrt_open_close\[' "$directory/xrt-open-close-context.txt"; then xrt_open_close=pass
        else xrt_open_close=fail; fi
        grep -q '^context_lifetime=ok' "$directory/xrt-open-close-context.txt" && xrt_context=pass || xrt_context=fail
    else
        printf 'Context helper unavailable (build rc %s); no success is inferred.\n' "$helper_build_rc" >"$directory/xrt-open-close-context.txt"
        printf '%s\n' "$helper_build_rc" >"$directory/xrt-open-close-context.txt.rc"
    fi

    if ((ATTEMPT_SUSPEND == 1)); then
        capture_guest "$directory/suspend-resume.txt" 1 \
            'if command -v systemctl >/dev/null 2>&1 && systemctl is-system-running >/dev/null 2>&1; then echo suspend-requested; systemctl suspend; else echo suspend-not-supported; exit 77; fi'
        local suspend_rc=$(<"$directory/suspend-resume.txt.rc")
        if [[ "$suspend_rc" == 0 ]]; then suspend_resume=pass; elif [[ "$suspend_rc" == 77 ]]; then suspend_resume=skip; else suspend_resume=fail; fi
    else
        printf 'not-requested (pass --suspend-resume to attempt system suspend)\n' >"$directory/suspend-resume.txt"
        printf '0\n' >"$directory/suspend-resume.txt.rc"
    fi

    local overall=fail
    if [[ "$acceptance_a" == pass && "$acceptance_b" == pass && "$acceptance_c" == pass &&
          "$xrt_examine" == pass && "$xrt_open_close" == pass && "$xrt_context" == pass &&
          "$force_iova_status" != fail ]]; then
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
for index in "${!MODES[@]}"; do
    mode=${MODES[$index]}; MODE_DIRS+=("$RUN_DIR/$mode")
    run_mode "$mode" "$index" || FAILURES=$((FAILURES + 1))
done

python3 - "$RUN_DIR/summary.json" "${MODE_DIRS[@]}" <<'PY'
import json
import os
import sys
result = {"run": os.path.basename(os.path.dirname(sys.argv[1])), "modes": []}
for directory in sys.argv[2:]:
    with open(os.path.join(directory, "summary.json"), encoding="utf-8") as stream:
        result["modes"].append(json.load(stream))
result["overall"] = "pass" if result["modes"] and all(item.get("overall") == "pass" for item in result["modes"]) else "fail"
with open(sys.argv[1], "w", encoding="utf-8") as stream:
    json.dump(result, stream, indent=2)
    stream.write("\n")
PY

{
    printf 'XDNA guest integration run %s\n================================\n\n' "$RUN_ID"
    for directory in "${MODE_DIRS[@]}"; do
        # shellcheck disable=SC1090
        source "$directory/status.env"
        printf '%s: %s (A=%s B=%s C=%s, xrt-smi=%s, open=%s, context=%s)\n' \
            "$mode" "$overall" "$acceptance_a" "$acceptance_b" "$acceptance_c" "$xrt_examine" "$xrt_open_close" "$xrt_context"
    done
    printf '\nMachine-readable summary: %s\nPer-mode reports: %s/{normal,force_iova}/report.txt\n' "$RUN_DIR/summary.json" "$RUN_DIR"
} >"$RUN_DIR/report.txt"
printf 'Guest integration evidence: %s\n' "$RUN_DIR"
((FAILURES == 0))
