#!/bin/bash
set -euo pipefail

REPO_OWNER="rushikeshsakharleofficial"
REPO_NAME="ps_mem-c"
BASE_URL="https://${REPO_OWNER}.github.io/${REPO_NAME}"
GH_BASE="https://github.com/${REPO_OWNER}/${REPO_NAME}"

# Fetch latest tag
LATEST=$(curl -fsSL "https://api.github.com/repos/${REPO_OWNER}/${REPO_NAME}/releases/latest" \
  | grep '"tag_name"' | cut -d'"' -f4)
VER="${LATEST#v}"

ARCH=$(uname -m)
OS=$(uname -s)

echo "Installing ps_mem ${LATEST} (${OS}/${ARCH})..."

case "$OS" in
  Linux)
    case "$ARCH" in
      x86_64)  DEB_ARCH="amd64"; RPM_ARCH="x86_64"  ;;
      aarch64) DEB_ARCH="arm64"; RPM_ARCH="aarch64"  ;;
      *)       DEB_ARCH="$ARCH"; RPM_ARCH="$ARCH"    ;;
    esac

    if [ -f /etc/debian_version ] || grep -qi debian /etc/os-release 2>/dev/null; then
      # Debian / Ubuntu / Raspbian / Kali / Pop!_OS / Linux Mint
      curl -fsSL "${BASE_URL}/public.gpg" | sudo gpg --dearmor -o /usr/share/keyrings/ps-mem.gpg
      echo "deb [signed-by=/usr/share/keyrings/ps-mem.gpg] ${BASE_URL}/apt /" \
        | sudo tee /etc/apt/sources.list.d/ps-mem.list > /dev/null
      sudo apt-get update -qq && sudo apt-get install -y ps-mem

    elif [ -f /etc/redhat-release ] || [ -f /etc/rocky-release ] || [ -f /etc/fedora-release ] || [ -f /etc/centos-release ] || [ -f /etc/almalinux-release ]; then
      # RHEL / Rocky / Fedora / CentOS / AlmaLinux / Oracle Linux
      sudo rpm --import "${BASE_URL}/public.gpg"
      sudo tee /etc/yum.repos.d/ps-mem.repo > /dev/null <<EOF
[ps-mem]
name=ps_mem
baseurl=${BASE_URL}/rpm/\$basearch/
enabled=1
gpgcheck=1
gpgkey=${BASE_URL}/public.gpg
EOF
      sudo dnf install -y ps_mem 2>/dev/null || sudo yum install -y ps_mem

    elif [ -f /etc/arch-release ] || [ -f /etc/manjaro-release ]; then
      # Arch Linux / Manjaro / EndeavourOS
      if command -v yay &>/dev/null; then
        yay -S --noconfirm ps_mem
      elif command -v paru &>/dev/null; then
        paru -S --noconfirm ps_mem
      else
        TMPDIR=$(mktemp -d)
        curl -fsSL "${GH_BASE}/raw/main/packaging/PKGBUILD" -o "${TMPDIR}/PKGBUILD"
        cd "$TMPDIR" && makepkg -si --noconfirm
      fi

    elif [ -f /etc/alpine-release ]; then
      # Alpine Linux
      MUSL_URL="${GH_BASE}/releases/download/${LATEST}/ps_mem_${VER}_linux_x86_64_musl.tar.gz"
      TMP=$(mktemp -d)
      curl -fsSL "$MUSL_URL" | tar xz -C "$TMP"
      sudo install -m755 "$TMP/ps_mem" /usr/bin/ps_mem

    elif [ -f /etc/SuSE-release ] || [ -f /etc/opensuse-release ] || grep -qi suse /etc/os-release 2>/dev/null; then
      # openSUSE / SUSE
      sudo zypper --non-interactive install ps_mem 2>/dev/null || \
        sudo rpm -Uvh "${GH_BASE}/releases/download/${LATEST}/ps_mem-${VER}-1.el9.${RPM_ARCH}.rpm"

    elif [ -f /etc/void-release ] || grep -qi void /etc/os-release 2>/dev/null; then
      # Void Linux
      TMP=$(mktemp -d)
      curl -fsSL "${GH_BASE}/releases/download/${LATEST}/ps_mem_${VER}_linux_x86_64_musl.tar.gz" | tar xz -C "$TMP"
      sudo install -m755 "$TMP/ps_mem" /usr/bin/ps_mem

    elif command -v nix-env &>/dev/null; then
      # NixOS / Nix
      nix-env -iA nixpkgs.ps_mem 2>/dev/null || {
        TMP=$(mktemp -d)
        curl -fsSL "${GH_BASE}/releases/download/${LATEST}/ps_mem_${VER}_linux_${ARCH}_musl.tar.gz" | tar xz -C "$TMP" 2>/dev/null || \
          curl -fsSL "${GH_BASE}/releases/download/${LATEST}/ps_mem_${VER}_linux_x86_64_musl.tar.gz" | tar xz -C "$TMP"
        sudo install -m755 "$TMP/ps_mem" /usr/local/bin/ps_mem
      }

    elif command -v snap &>/dev/null; then
      # Snap fallback
      snap install ps-mem 2>/dev/null || {
        echo "Falling back to binary install..."
        TMP=$(mktemp -d)
        curl -fsSL "${GH_BASE}/releases/download/${LATEST}/ps_mem_${VER}_linux_x86_64_musl.tar.gz" | tar xz -C "$TMP"
        sudo install -m755 "$TMP/ps_mem" /usr/bin/ps_mem
      }

    else
      # Generic Linux — download musl static binary
      echo "Unknown distro — installing static binary..."
      TMP=$(mktemp -d)
      curl -fsSL "${GH_BASE}/releases/download/${LATEST}/ps_mem_${VER}_linux_x86_64_musl.tar.gz" | tar xz -C "$TMP"
      sudo install -m755 "$TMP/ps_mem" /usr/bin/ps_mem
    fi
    ;;

  Darwin)
    # macOS
    case "$ARCH" in
      arm64)  MAC_ARCH="arm64"  ;;
      x86_64) MAC_ARCH="x86_64" ;;
      *)      MAC_ARCH="arm64"  ;;
    esac
    if command -v brew &>/dev/null; then
      brew tap "${REPO_OWNER}/ps_mem" "${GH_BASE}" 2>/dev/null || true
      brew install ps_mem 2>/dev/null || {
        TMP=$(mktemp -d)
        curl -fsSL "${GH_BASE}/releases/download/${LATEST}/ps_mem_${VER}_darwin_${MAC_ARCH}.tar.gz" | tar xz -C "$TMP"
        sudo install -m755 "$TMP/ps_mem" /usr/local/bin/ps_mem
      }
    else
      TMP=$(mktemp -d)
      curl -fsSL "${GH_BASE}/releases/download/${LATEST}/ps_mem_${VER}_darwin_${MAC_ARCH}.tar.gz" | tar xz -C "$TMP"
      sudo install -m755 "$TMP/ps_mem" /usr/local/bin/ps_mem
    fi
    echo "Note: macOS requires sudo to read process memory."
    ;;

  *)
    echo "Unsupported OS: $OS. Build from source: gcc -O2 -o ps_mem ps_mem.c"
    exit 1
    ;;
esac

echo "Done. Run: sudo ps_mem"
