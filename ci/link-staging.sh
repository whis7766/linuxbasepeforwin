#!/bin/bash
# Buildroot's output/staging is a symlink to output/host/<triple>/sysroot.
# Recreate it after restoring the archived host tree so per-package staging
# file lists and package builds resolve the sysroot.
set -euo pipefail

OUT="$GITHUB_WORKSPACE/output"
sysroot=$(find "$OUT/host" -mindepth 2 -maxdepth 2 -type d -name sysroot | head -n1)
if [ -z "$sysroot" ]; then
  echo "ERROR: Buildroot sysroot not found under output/host" >&2
  exit 1
fi

ln -snf "$sysroot" "$OUT/staging"
