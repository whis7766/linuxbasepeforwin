#!/bin/bash
# Archive Buildroot package build directories and stamps so the final job can
# skip rebuilding packages already produced on a parallel runner.
set -euo pipefail

OUT="$GITHUB_WORKSPACE/output"
mkdir -p "$GITHUB_WORKSPACE/part-build"

for pkg in "$@"; do
  find "$OUT/build" -maxdepth 1 -type d -name "${pkg}-*" -print0 \
    | while IFS= read -r -d '' dir; do
      base=$(basename "$dir")
      mkdir -p "$GITHUB_WORKSPACE/part-build/$base"
      cp -a "$dir/." "$GITHUB_WORKSPACE/part-build/$base/"
    done
done

tar -czf "$GITHUB_WORKSPACE/part-build.tar.gz" -C "$GITHUB_WORKSPACE/part-build" .
