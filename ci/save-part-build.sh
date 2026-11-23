#!/bin/bash
# Archive Buildroot package stamps and per-package file lists so the final job
# treats these packages as already built. Extracted sources and objects are
# deliberately excluded: they are large and the installed files already travel
# in part-overlay.tar.gz.
set -euo pipefail

OUT="$GITHUB_WORKSPACE/output"
STAGE="$GITHUB_WORKSPACE/part-build"
rm -rf "$STAGE"
mkdir -p "$STAGE"

for pkg in "$@"; do
  while IFS= read -r dir; do
    [ -d "$dir" ] || continue
    base=$(basename "$dir")
    mkdir -p "$STAGE/$base"
    find "$dir" -maxdepth 1 -type f \
      \( -name '.stamp_*' -o -name '.files-list*' -o -name '.config' \) \
      -exec cp -a {} "$STAGE/$base/" \;
  done < <(find "$OUT/build" -maxdepth 1 -type d -name "${pkg}-*" | sort)
done

tar -czf "$GITHUB_WORKSPACE/part-build.tar.gz" -C "$STAGE" .
