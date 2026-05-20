#!/bin/bash
set -euo pipefail

REPO="rushikeshsakharleofficial/ps_mem-c"
API="https://api.github.com/repos/${REPO}/releases/latest"

# Detect architecture
ARCH=$(uname -m)
case "$ARCH" in
  x86_64)  DEB_ARCH="amd64";  RPM_ARCH="x86_64"  ;;
  aarch64) DEB_ARCH="arm64";  RPM_ARCH="aarch64"  ;;
  *)       echo "Unsupported arch: $ARCH"; exit 1  ;;
esac

# Fetch latest release tag
LATEST=$(curl -fsSL "$API" | grep '"tag_name"' | cut -d'"' -f4)
VER="${LATEST#v}"

echo "Installing ps_mem-c ${LATEST} (${ARCH})..."

if [ -f /etc/debian_version ]; then
  URL="https://github.com/${REPO}/releases/download/${LATEST}/ps_mem-c_${VER}_${DEB_ARCH}.deb"
  TMP=$(mktemp /tmp/ps_mem-c.XXXXXX.deb)
  curl -fsSL "$URL" -o "$TMP"
  dpkg -i "$TMP"
  rm -f "$TMP"

elif [ -f /etc/redhat-release ] || [ -f /etc/rocky-release ] || [ -f /etc/fedora-release ]; then
  URL="https://github.com/${REPO}/releases/download/${LATEST}/ps_mem-c-${VER}-1.el9.${RPM_ARCH}.rpm"
  rpm -Uvh --force "$URL"

else
  echo "Unsupported distro. Build from source:"
  echo "  gcc -O2 -o ps_mem ps_mem.c && sudo cp ps_mem /usr/bin/ps_mem"
  exit 1
fi

echo "Done. Run: sudo ps_mem"
