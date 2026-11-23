#!/usr/bin/env bash
# post-build: finalize rootfs before it is packed into rootfs.cpio.gz.
set -euo pipefail
TARGET_DIR="$1"

# Ensure the GUI autostart script is executable.
chmod 0755 "${TARGET_DIR}/etc/init.d/S99lbpe" 2>/dev/null || true

# glibc gconv modules: Buildroot only installs shared libs, but hivex/NTFS
# and the GUI need UTF-16LE<->UTF-8 conversion for Windows key names.
# Buildroot staging is output/host/<gnu-triple>/sysroot, not output/staging.
STAGING_ROOT="$(dirname "${TARGET_DIR}")/host"
# Buildroot x86_64 glibc installs gconv under /usr/lib64/gconv; older layouts
# use /usr/lib/gconv. Copy whichever exists, preserving the matching path so
# libhivex's iconv finds UTF-16LE/LATIN1 converters at runtime.
for sysroot in "${STAGING_ROOT}"/*/sysroot; do
    [ -d "${sysroot}" ] || continue
    for g in usr/lib64/gconv usr/lib/gconv; do
        if [ -d "${sysroot}/${g}" ]; then
            mkdir -p "${TARGET_DIR}/${g}"
            cp -a "${sysroot}/${g}/." "${TARGET_DIR}/${g}/"
        fi
    done
done

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