#!/bin/bash
# Collect only the files installed by the given Buildroot packages into a
# portable overlay. Buildroot writes per-package file lists in its build dir:
#   .files-list-host.txt      -> output/host
#   .files-list-staging.txt   -> output/staging
#   .files-list.txt           -> output/target
set -euo pipefail

OUT="$GITHUB_WORKSPACE/output"
STAGE="$GITHUB_WORKSPACE/overlay"
rm -rf "$STAGE"
mkdir -p "$STAGE/host" "$STAGE/staging" "$STAGE/target" "$STAGE/images"

copy_list() {
  local pkg="$1"
  local build_dir="$2"
  local list="$3"
  local src_root="$4"
  local dst_root="$5"

  [ -f "$build_dir/$list" ] || return 0
  while IFS=, read -r _ pkg_path; do
    [ -n "$pkg_path" ] || continue
    local rel="${pkg_path#./}"
    mkdir -p "$STAGE/$dst_root/$(dirname "$rel")"
    cp -a "$OUT/$src_root/$rel" "$STAGE/$dst_root/$rel"
  done < "$build_dir/$list"
}

for pkg in "$@"; do
  mapfile -t dirs < <(find "$OUT/build" -maxdepth 1 -type d -name "${pkg}-*" | sort)
  if [ "${#dirs[@]}" -eq 0 ]; then
    echo "ERROR: no Buildroot build directory found for $pkg" >&2
    exit 1
  fi
  for build_dir in "${dirs[@]}"; do
    copy_list "$pkg" "$build_dir" ".files-list-host.txt" host host
    copy_list "$pkg" "$build_dir" ".files-list-staging.txt" staging staging
    copy_list "$pkg" "$build_dir" ".files-list.txt" target target
    copy_list "$pkg" "$build_dir" ".files-list-images.txt" images images
  done
done

tar -czf "$GITHUB_WORKSPACE/part-overlay.tar.gz" -C "$STAGE" .
