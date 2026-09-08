#!/usr/bin/env bash
# Build a clean, out-of-tree QEMU with the XDNA1 device integrated.
# SPDX-License-Identifier: GPL-2.0-only

set -Eeuo pipefail

SCRIPT_DIR=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
REPO_ROOT=$(cd -- "$SCRIPT_DIR/.." && pwd)

QEMU_URL=${XDNA_QEMU_URL:-https://gitlab.com/qemu-project/qemu.git}
QEMU_REF=${XDNA_QEMU_REF:-v11.1.1}
QEMU_SOURCE_ARG=${XDNA_QEMU_SOURCE:-}
QEMU_BUILD_ARG=${XDNA_QEMU_BUILD:-}
QEMU_CACHE_ROOT=${XDNA_QEMU_CACHE:-${XDG_CACHE_HOME:-$HOME/.cache}/xdna1-npu-emulator}
QEMU_JOBS=${XDNA_QEMU_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || printf '2')}
QEMU_WERROR=1
QEMU_KEEP_SOURCE=0
QEMU_CLEAN_BUILD=0
QEMU_INTEGRATE=1
QEMU_CONFIGURE_ARGS=()

# v11.1.1 resolves to this commit in the upstream QEMU repository. Keeping
# the commit alongside the tag makes the primary integration target explicit.
QEMU_11_1_1_COMMIT=c3d48b7d1e89604920e5b81b91140c2ad39a1943

usage() {
    cat <<'EOF'
Usage: scripts/build-qemu.sh [options]

Build qemu-system-x86_64 from a clean QEMU checkout with xdna-npu enabled.
The default target is upstream QEMU v11.1.1 (pinned by commit).

Options:
  --qemu-src PATH       use an existing clean QEMU source checkout
  --qemu-ref REF        checkout REF (default: v11.1.1; master is useful for CI)
  --qemu-url URL        clone URL (default: upstream QEMU GitLab repository)
  --build-dir PATH      persistent QEMU build output directory
  --cache-dir PATH      source clone cache directory
  --jobs N              Ninja parallelism
  --configure-arg ARG   pass one additional argument to QEMU configure
  --no-werror           keep QEMU warnings enabled without promoting them to errors
  --baseline-only       build QEMU without applying the XDNA integration (upstream smoke check)
  --keep-source         keep the temporary integrated QEMU worktree for inspection
  --clean-build         remove a previous build directory created by this script
  -h, --help            show this help

Examples:
  scripts/build-qemu.sh
  scripts/build-qemu.sh --qemu-src /work/qemu-11.1.1 --build-dir /tmp/xdna-qemu
  scripts/build-qemu.sh --qemu-ref master --no-werror
EOF
}

die() {
    printf 'build-qemu.sh: %s\n' "$*" >&2
    exit 1
}

while (($#)); do
    case "$1" in
        --qemu-src)
            (($# >= 2)) || die "--qemu-src needs a path"
            QEMU_SOURCE_ARG=$2
            shift 2
            ;;
        --qemu-ref)
            (($# >= 2)) || die "--qemu-ref needs a ref"
            QEMU_REF=$2
            shift 2
            ;;
        --qemu-url)
            (($# >= 2)) || die "--qemu-url needs a URL"
            QEMU_URL=$2
            shift 2
            ;;
        --build-dir)
            (($# >= 2)) || die "--build-dir needs a path"
            QEMU_BUILD_ARG=$2
            shift 2
            ;;
        --cache-dir)
            (($# >= 2)) || die "--cache-dir needs a path"
            QEMU_CACHE_ROOT=$2
            shift 2
            ;;
        --jobs)
            (($# >= 2)) || die "--jobs needs a number"
            QEMU_JOBS=$2
            shift 2
            ;;
        --configure-arg)
            (($# >= 2)) || die "--configure-arg needs an argument"
            QEMU_CONFIGURE_ARGS+=("$2")
            shift 2
            ;;
        --no-werror)
            QEMU_WERROR=0
            shift
            ;;
        --baseline-only)
            QEMU_INTEGRATE=0
            shift
            ;;
        --keep-source)
            QEMU_KEEP_SOURCE=1
            shift
            ;;
        --clean-build)
            QEMU_CLEAN_BUILD=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1 (use --help)"
            ;;
    esac
done

command -v git >/dev/null || die "git is required"
command -v ninja >/dev/null || die "ninja is required"

if [[ -z "$QEMU_BUILD_ARG" ]]; then
    build_label=${QEMU_REF//\//-}
    QEMU_BUILD_ARG="$REPO_ROOT/build/qemu-$build_label"
fi
mkdir -p -- "$(dirname -- "$QEMU_BUILD_ARG")"
QEMU_BUILD_ARG=$(cd -- "$(dirname -- "$QEMU_BUILD_ARG")" && pwd)/$(basename -- "$QEMU_BUILD_ARG")

if [[ -e "$QEMU_BUILD_ARG" ]]; then
    if [[ "$QEMU_CLEAN_BUILD" == 1 ]]; then
        rm -rf -- "$QEMU_BUILD_ARG"
    elif [[ -n "$(find "$QEMU_BUILD_ARG" -mindepth 1 -maxdepth 1 -print -quit 2>/dev/null)" ]]; then
        [[ -f "$QEMU_BUILD_ARG/.xdna-qemu-build" ]] ||
            die "build directory exists and was not created by this script: $QEMU_BUILD_ARG (use another path or --clean-build)"
    fi
fi
mkdir -p -- "$QEMU_BUILD_ARG"
touch -- "$QEMU_BUILD_ARG/.xdna-qemu-build"

resolve_source() {
    local candidate

    if [[ -n "$QEMU_SOURCE_ARG" ]]; then
        [[ -d "$QEMU_SOURCE_ARG" ]] || die "QEMU source directory not found: $QEMU_SOURCE_ARG"
        candidate=$(cd -- "$QEMU_SOURCE_ARG" && pwd)
    else
        candidate="$QEMU_CACHE_ROOT/qemu-${QEMU_REF//\//-}"
        if [[ ! -d "$candidate/.git" ]]; then
            mkdir -p -- "$QEMU_CACHE_ROOT"
            git clone --depth 1 --branch "$QEMU_REF" "$QEMU_URL" "$candidate"
        fi
    fi

    [[ -f "$candidate/configure" && -d "$candidate/hw/misc" ]] ||
        die "not a QEMU source tree: $candidate"

    if git -C "$candidate" rev-parse --git-dir >/dev/null 2>&1; then
        [[ -z "$(git -C "$candidate" status --porcelain)" ]] ||
            die "QEMU source tree is not clean: $candidate"

        local actual
        actual=$(git -C "$candidate" rev-parse "${QEMU_REF}^{commit}" 2>/dev/null || true)
        [[ -n "$actual" ]] || die "QEMU ref not found in source tree: $QEMU_REF"
        if [[ "$QEMU_REF" == v11.1.1 && "$actual" != "$QEMU_11_1_1_COMMIT" ]]; then
            die "QEMU v11.1.1 is not the pinned commit (got $actual, expected $QEMU_11_1_1_COMMIT)"
        fi
        QEMU_SOURCE_COMMIT=$actual
        QEMU_SOURCE_IS_GIT=1
    else
        # An extracted source archive is accepted, but cannot be cryptographically
        # pinned. The configured QEMU version remains visible in the build output.
        QEMU_SOURCE_COMMIT=unverified
        QEMU_SOURCE_IS_GIT=0
    fi
    QEMU_SOURCE_ROOT=$candidate
}

resolve_source

QEMU_WORKTREE_PARENT=$(mktemp -d "${TMPDIR:-/tmp}/xdna-qemu-worktree.XXXXXX")
QEMU_WORKTREE="$QEMU_WORKTREE_PARENT/source"

QEMU_WORKTREE_IS_GIT=0
cleanup() {
    local status=$?
    if [[ "$QEMU_KEEP_SOURCE" != 1 && -e "$QEMU_WORKTREE" ]]; then
        if [[ "$QEMU_WORKTREE_IS_GIT" == 1 ]]; then
            git -C "$QEMU_SOURCE_ROOT" worktree remove --force "$QEMU_WORKTREE" >/dev/null 2>&1 || true
        else
            rm -rf -- "$QEMU_WORKTREE"
        fi
    fi
    if [[ "$QEMU_KEEP_SOURCE" != 1 && -e "$QEMU_WORKTREE_PARENT" ]]; then
        rm -rf -- "$QEMU_WORKTREE_PARENT"
    fi
    exit "$status"
}
trap cleanup EXIT

if [[ "$QEMU_SOURCE_IS_GIT" == 1 ]]; then
    git -C "$QEMU_SOURCE_ROOT" worktree add --detach "$QEMU_WORKTREE" "$QEMU_SOURCE_COMMIT"
    QEMU_WORKTREE_IS_GIT=1
else
    mkdir -p -- "$QEMU_WORKTREE"
    cp -a -- "$QEMU_SOURCE_ROOT"/. "$QEMU_WORKTREE"/
fi

if [[ "$QEMU_INTEGRATE" == 1 ]]; then
    mkdir -p -- "$QEMU_WORKTREE/hw/misc/xdna" "$QEMU_WORKTREE/include/xdna"
    cp -- "$REPO_ROOT"/src/*.c "$QEMU_WORKTREE/hw/misc/xdna/"
    cp -- "$REPO_ROOT"/src/*.h "$QEMU_WORKTREE/hw/misc/xdna/"
    cp -- "$REPO_ROOT"/include/xdna/*.h "$QEMU_WORKTREE/include/xdna/"
    cp -- "$REPO_ROOT"/qemu/hw/misc/xdna_npu.c "$QEMU_WORKTREE/hw/misc/xdna_npu.c"

    if git -C "$QEMU_WORKTREE" rev-parse --git-dir >/dev/null 2>&1; then
        git -C "$QEMU_WORKTREE" apply --whitespace=nowarn \
            "$REPO_ROOT/qemu/integration/qemu-11.1.1.patch"
    else
        command -v patch >/dev/null || die "patch is required for an archive source tree"
        patch --batch --forward -p1 -d "$QEMU_WORKTREE" < \
            "$REPO_ROOT/qemu/integration/qemu-11.1.1.patch"
    fi
else
    printf 'QEMU baseline build: skipping XDNA integration patch\n'
fi

configure_args=(
    "--target-list=x86_64-softmmu"
    "--disable-docs"
    "--disable-tools"
    "--disable-guest-agent"
    # The guest runner uses QEMU's user networking for SSH/evidence capture.
    # QEMU falls back to its pinned libslirp subproject when no system
    # libslirp development package is available.
    "--enable-slirp"
)
if [[ "$QEMU_WERROR" == 1 ]]; then
    configure_args+=(--enable-werror)
else
    configure_args+=(--disable-werror)
fi
configure_args+=("${QEMU_CONFIGURE_ARGS[@]}")

printf 'QEMU source: %s (%s)\n' "$QEMU_SOURCE_ROOT" "$QEMU_SOURCE_COMMIT"
printf 'QEMU worktree: %s\n' "$QEMU_WORKTREE"
printf 'QEMU build: %s\n' "$QEMU_BUILD_ARG"
printf 'configure: %q' "$QEMU_WORKTREE/configure"
printf ' %q' "${configure_args[@]}"
printf '\n'

(
    cd -- "$QEMU_BUILD_ARG"
    "$QEMU_WORKTREE/configure" "${configure_args[@]}"
)
ninja -C "$QEMU_BUILD_ARG" -j"$QEMU_JOBS" qemu-system-x86_64

QEMU_BINARY="$QEMU_BUILD_ARG/qemu-system-x86_64"
[[ -x "$QEMU_BINARY" ]] || die "QEMU build did not produce qemu-system-x86_64"
if [[ "$QEMU_INTEGRATE" == 1 ]]; then
    "$QEMU_BINARY" -device help | grep -qE '^name "xdna-npu"' ||
        die "built QEMU does not register xdna-npu"
fi

printf '%s\n' \
    "XDNA_QEMU_SOURCE=$QEMU_SOURCE_ROOT" \
    "XDNA_QEMU_COMMIT=$QEMU_SOURCE_COMMIT" \
    "XDNA_QEMU_REF=$QEMU_REF" \
    "XDNA_QEMU_BUILD=$QEMU_BUILD_ARG" \
    "XDNA_QEMU_BINARY=$QEMU_BINARY" \
    "XDNA_QEMU_WERROR=$QEMU_WERROR" \
    "XDNA_QEMU_INTEGRATED=$QEMU_INTEGRATE" \
    > "$QEMU_BUILD_ARG/xdna-build.env"

printf 'Built QEMU: %s\n' "$QEMU_BINARY"
