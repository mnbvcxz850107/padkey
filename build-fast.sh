#!/bin/bash
# Build the firmware natively on the Android host (much faster than proot).
set -e
cd "$(dirname "$0")"
python3 tools/gen_assets.py
R=/data/data/com.termux/files/usr/var/lib/proot-distro/containers/debian/rootfs
ssh -i ~/.ssh/padkey_build -p 8022 -o BatchMode=yes localhost "bash $R$PWD/native_build.sh $*"
