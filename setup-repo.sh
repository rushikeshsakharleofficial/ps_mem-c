#!/bin/bash
set -euo pipefail

REPO_OWNER="rushikeshsakharleofficial"
REPO_NAME="ps_mem-c"
BASE_URL="https://${REPO_OWNER}.github.io/${REPO_NAME}"

echo "Adding ps_mem package repository..."

if [ -f /etc/debian_version ]; then
  # Debian / Ubuntu
  curl -fsSL "${BASE_URL}/public.gpg" | sudo gpg --dearmor -o /usr/share/keyrings/ps-mem.gpg
  echo "deb [signed-by=/usr/share/keyrings/ps-mem.gpg] ${BASE_URL}/apt /" \
    | sudo tee /etc/apt/sources.list.d/ps-mem.list > /dev/null
  sudo apt-get update -qq
  sudo apt-get install -y ps-mem
  echo "Done. Run: sudo ps_mem"

elif [ -f /etc/redhat-release ] || [ -f /etc/rocky-release ] || [ -f /etc/fedora-release ]; then
  # RHEL / Rocky / Fedora
  sudo rpm --import "${BASE_URL}/public.gpg"
  sudo tee /etc/yum.repos.d/ps-mem.repo > /dev/null <<EOF
[ps-mem]
name=ps_mem - C port with smaps_rollup optimization
baseurl=${BASE_URL}/rpm/\$basearch/
enabled=1
gpgcheck=1
gpgkey=${BASE_URL}/public.gpg
EOF
  sudo dnf install -y ps_mem
  echo "Done. Run: sudo ps_mem"

else
  echo "Unsupported distro. Build from source:"
  echo "  gcc -O2 -o ps_mem ps_mem.c && sudo cp ps_mem /usr/bin/ps_mem"
  exit 1
fi
