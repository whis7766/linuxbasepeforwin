#!/usr/bin/env bash
# post-image: 把内核与外置 initramfs 打成 BIOS+UEFI 双启动 ISO。
# rootfs 走 rootfs.cpio.gz（grub initrd），不在内核里内嵌，
# 这样包集合变化时 final 阶段无需重编内核。
set -euo pipefail
BOARD_DIR="$(dirname "$0")"
BINARIES_DIR="$1"

ISO_ROOT="$(mktemp -d)"
trap 'rm -rf "${ISO_ROOT}"' EXIT
mkdir -p "${ISO_ROOT}/boot/grub"

cp "${BINARIES_DIR}/bzImage" "${ISO_ROOT}/boot/bzImage"
cp "${BINARIES_DIR}/rootfs.cpio.gz" "${ISO_ROOT}/boot/rootfs.cpio.gz"
cp "${BOARD_DIR}/grub.cfg" "${ISO_ROOT}/boot/grub/grub.cfg"

grub-mkrescue -o "${BINARIES_DIR}/linuxbasepe.iso" "${ISO_ROOT}" \
  -- -volid LINUXBASEPE

echo "ISO written: ${BINARIES_DIR}/linuxbasepe.iso"
