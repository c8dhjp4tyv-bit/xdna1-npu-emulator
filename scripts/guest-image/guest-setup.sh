#!/usr/bin/env bash
# Configure the disposable guest image.  The kernel and amdxdna module are
# supplied by the host and must have the same KVER.
# SPDX-License-Identifier: GPL-2.0-only

set -Eeuo pipefail

KVER=${1:?kernel version is required}
MODULE_DIR="/usr/lib/modules/$KVER"

[[ -d "$MODULE_DIR" ]] || { printf 'missing module directory: %s\n' "$MODULE_DIR" >&2; exit 1; }
[[ -f /etc/xdna-guest/authorized_keys ]] || { printf 'missing SSH key\n' >&2; exit 1; }

install -d -m 0700 /root/.ssh
install -m 0600 /etc/xdna-guest/authorized_keys /root/.ssh/authorized_keys
install -d -m 0755 /etc/sudoers.d
printf '%s\n' 'root ALL=(ALL) NOPASSWD: ALL' >/etc/sudoers.d/xdna-guest
chmod 0440 /etc/sudoers.d/xdna-guest

install -d -m 0755 /etc/ssh/sshd_config.d
cat >/etc/ssh/sshd_config.d/99-xdna-guest.conf <<'EOF'
PermitRootLogin yes
PubkeyAuthentication yes
PasswordAuthentication no
UsePAM yes
EOF
ssh-keygen -A

install -d -m 0755 /etc/NetworkManager/system-connections
cat >/etc/NetworkManager/system-connections/xdna-dhcp.nmconnection <<'EOF'
[connection]
id=xdna-dhcp
type=ethernet
autoconnect=true

[ipv4]
method=auto

[ipv6]
method=ignore
EOF
chmod 0600 /etc/NetworkManager/system-connections/xdna-dhcp.nmconnection

printf '%s\n' xdna-guest >/etc/hostname
# Avoid systemd-firstboot's interactive serial prompt.  This image is
# disposable and always booted with -snapshot, so a fixed identity is safe.
printf '%s\n' 8f6b6c7a8a4d4b9e9f6d1d2c3b4a5968 >/etc/machine-id

cat >/etc/profile.d/xrt.sh <<'EOF'
export XILINX_XRT=/opt/xilinx/xrt
export PATH="$XILINX_XRT/bin:$PATH"
export LD_LIBRARY_PATH="$XILINX_XRT/lib64${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
EOF
printf '%s\n' /opt/xilinx/xrt/lib64 >/etc/ld.so.conf.d/xrt.conf
ln -sf /opt/xilinx/xrt/bin/xrt-smi /usr/local/bin/xrt-smi
ldconfig

systemctl enable sshd.service
systemctl enable NetworkManager.service
depmod -a "$KVER"
