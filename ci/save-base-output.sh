#!/bin/bash
# Archive the shared base for parallel part runners. Keep installed host
# (which contains the toolchain sysroot), target tree and kernel images; for
# dependency build directories only keep stamps and file lists, not extracted
# sources/objects.
set -euo pipefail

ROOT="$GITHUB_WORKSPACE/base-output"
rm -rf "$ROOT"
mkdir -p "$ROOT/host" "$ROOT/target" "$ROOT/build" "$ROOT/images"

cp -a "$GITHUB_WORKSPACE/output/host/." "$ROOT/host/"
cp -a "$GITHUB_WORKSPACE/output/target/." "$ROOT/target/"
[ -d "$GITHUB_WORKSPACE/output/images" ] && cp -a "$GITHUB_WORKSPACE/output/images/." "$ROOT/images/" || true

for dir in "$GITHUB_WORKSPACE/output/build"/*/; do
  [ -d "$dir" ] || continue
  name=$(basename "$dir")
  mkdir -p "$ROOT/build/$name"
  find "$dir" -maxdepth 1 -type f \
    \( -name '.stamp_*' -o -name '.files-list*' \) \
    -exec cp -a {} "$ROOT/build/$name/" \;
done

tar -czf "$GITHUB_WORKSPACE/base-output.tar.gz" -C "$ROOT" .
