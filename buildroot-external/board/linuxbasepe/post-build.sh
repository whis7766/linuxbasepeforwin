#!/usr/bin/env bash
# post-build: finalize rootfs before initramfs is packed into the kernel.
set -euo pipefail
TARGET_DIR="$1"

# Ensure the GUI autostart script is executable.
chmod 0755 "${TARGET_DIR}/etc/init.d/S99lbpe" 2>/dev/null || true

# glibc gconv modules: Buildroot only installs shared libs, but hivex/NTFS
# and the GUI need UTF-16LE<->UTF-8 conversion for Windows key names.
STAGING_DIR="${TARGET_DIR%/*}/staging"
if [ -d "${STAGING_DIR}/usr/lib/gconv" ]; then
    mkdir -p "${TARGET_DIR}/usr/lib/gconv"
    cp -a "${STAGING_DIR}/usr/lib/gconv/." "${TARGET_DIR}/usr/lib/gconv/"
fi

# Convenience: mount points used by the maintenance tools.
mkdir -p "${TARGET_DIR}/mnt" "${TARGET_DIR}/run" "${TARGET_DIR}/tmp"

# fstab for pseudo filesystems (busybox init mounts most, keep explicit).
cat > "${TARGET_DIR}/etc/fstab" <<'EOF'
proc     /proc    proc     defaults      0 0
sysfs    /sys     sysfs    defaults      0 0
devtmpfs /dev     devtmpfs defaults      0 0
tmpfs    /tmp     tmpfs    mode=1777     0 0
efivarfs /sys/firmware/efi/efivars efivarfs defaults 0 0
EOF