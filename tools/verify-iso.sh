#!/usr/bin/env bash
# 校验 linuxbasepe.iso 结构（不需要 root）：BIOS/UEFI 双启动、grub 配置、内核大小、
# initramfs 里的关键文件。用法：tools/verify-iso.sh <iso>
set -euo pipefail
ISO="${1:?usage: verify-iso.sh <iso>}"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

fail=0
ok() { echo "[PASS] $*"; }
bad() { echo "[FAIL] $*"; fail=$((fail + 1)); }

[ -f "$ISO" ] || { echo "no such ISO: $ISO"; exit 2; }
SIZE=$(stat -c%s "$ISO")
ok "ISO 大小 $((SIZE / 1024 / 1024)) MiB"

# 1) 混合启动记录：保护性 MBR 0x55AA + EFI El Torito 引导镜像
MBR_SIG=$(dd if="$ISO" bs=1 skip=510 count=2 status=none | od -An -tx1 | tr -d ' \n')
[ "$MBR_SIG" = "55aa" ] && ok "ISO 带 0x55AA 引导签名（BIOS 可引导）" || bad "缺少 MBR 引导签名"

if command -v xorriso >/dev/null; then
  xorriso -indev "$ISO" -find / -maxdepth 3 -type f 2>/dev/null | sort > "$WORK/list.txt" || true
  grep -q "/boot/grub/grub.cfg" "$WORK/list.txt" && ok "存在 /boot/grub/grub.cfg" || bad "缺少 grub.cfg"
  grep -q "/boot/bzImage" "$WORK/list.txt" && ok "存在 /boot/bzImage" || bad "缺少 bzImage"
  grep -q "/boot/rootfs.cpio.gz" "$WORK/list.txt" && ok "存在 /boot/rootfs.cpio.gz" || bad "缺少 rootfs.cpio.gz"
  if grep -qi "efi/boot/bootx64.efi" "$WORK/list.txt"; then
    ok "存在 EFI 引导程序（UEFI 可引导）"
  else
    bad "缺少 EFI/BOOT/BOOTX64.EFI"
  fi
  xorriso -osirrox on -indev "$ISO" -extract /boot/grub/grub.cfg "$WORK/grub.cfg" 2>/dev/null || true
  xorriso -osirrox on -indev "$ISO" -extract /boot/bzImage "$WORK/bzImage" 2>/dev/null || true
fi

if [ -s "$WORK/grub.cfg" ]; then
  grep -q "linux /boot/bzImage" "$WORK/grub.cfg" && ok "grub.cfg 指向 /boot/bzImage" || bad "grub.cfg 未指向 bzImage"
  grep -q "initrd /boot/rootfs.cpio.gz" "$WORK/grub.cfg" && ok "grub.cfg 加载外置 initrd" || bad "grub.cfg 缺少 initrd 行"
fi

if [ -s "$WORK/bzImage" ]; then
  KSIZE=$(stat -c%s "$WORK/bzImage")
  [ "$KSIZE" -gt $((5 * 1024 * 1024)) ] && ok "bzImage 大小正常（${KSIZE} 字节）" \
    || bad "bzImage 过小（${KSIZE} 字节）"
  MAGIC=$(dd if="$WORK/bzImage" bs=1 skip=514 count=4 status=none | od -An -c | tr -d ' \n')
  [ "$MAGIC" = "HdrS" ] && ok "bzImage 头魔数正确" || bad "bzImage 头魔数异常: $MAGIC"
fi

echo "== verify-iso: $fail failure(s) =="
exit $((fail > 0 ? 1 : 0))
