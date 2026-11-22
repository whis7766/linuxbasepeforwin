#!/usr/bin/env bash
# post-image: wrap the kernel (with embedded initramfs) into a hybrid BIOS+UEFI
# bootable ISO using the host grub-mkrescue (provided by the CI runner).
set -euo pipefail
BOARD_DIR="$(dirname "$0")"
BINARIES_DIR="$1"

ISO_ROOT="$(mktemp -d)"
trap 'rm -rf "${ISO_ROOT}"' EXIT
mkdir -p "${ISO_ROOT}/boot/grub"

cp "${BINARIES_DIR}/bzImage" "${ISO_ROOT}/boot/bzImage"
cp "${BOARD_DIR}/grub.cfg" "${ISO_ROOT}/boot/grub/grub.cfg"

grub-mkrescue -o "${BINARIES_DIR}/linuxbasepe.iso" "${ISO_ROOT}" \
  -- -volid LINUXBASEPE

echo "ISO written: ${BINARIES_DIR}/linuxbasepe.iso"