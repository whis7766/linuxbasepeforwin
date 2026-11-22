#!/bin/bash
set -euo pipefail

BR2_DIR="$HOME/buildroot"
mkdir -p "$HOME/.buildroot-dl"

if [ ! -f "$BR2_DIR/Makefile" ]; then
  wget -q "https://buildroot.org/downloads/buildroot-${BUILDROOT_VERSION}.tar.gz" -O /tmp/buildroot.tar.gz
  mkdir -p "$BR2_DIR"
  tar -xzf /tmp/buildroot.tar.gz -C "$BR2_DIR" --strip-components=1
fi

make -C "$BR2_DIR" \
  BR2_EXTERNAL="$GITHUB_WORKSPACE/buildroot-external" \
  BR2_DL_DIR="$HOME/.buildroot-dl" \
  O="$GITHUB_WORKSPACE/output" \
  linuxbasepe_x86_64_defconfig
