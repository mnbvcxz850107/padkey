#!/bin/bash
# Make the ESP32 toolchain runnable natively on Android (outside proot).
#  1. ELF interpreter rewritten in place to /data/data/com.termux/ld (a
#     symlink to Debian's glibc loader; valid both natively and in proot)
#  2. /data/data/com.termux/gl holds only versioned glibc libs, used as
#     LD_LIBRARY_PATH for native builds (no clash with Termux's bionic libs)
#  3. proot's emulated hard links (symlinks into rootfs/.l2s) are turned into
#     real files so gcc can find cc1plus/as/ld relative to itself
set -e
R=/data/data/com.termux/files/usr/var/lib/proot-distro/containers/debian/rootfs
T=$HOME/.arduino15/packages/esp32/tools/esp-x32
C=$HOME/.arduino15/packages/builtin/tools/ctags
python3 "$(dirname "$0")/interp_patch.py" /lib/ld-linux-aarch64.so.1 /data/data/com.termux/ld $T $C
ssh -i ~/.ssh/padkey_build -p 8022 -o BatchMode=yes localhost 'bash -s' <<EOS
set -e
L=$R/usr/lib/aarch64-linux-gnu
ln -sfn \$L/ld-linux-aarch64.so.1 /data/data/com.termux/ld
mkdir -p /data/data/com.termux/gl
for l in libc.so.6 libdl.so.2 libgcc_s.so.1 libm.so.6 libpthread.so.0 libstdc++.so.6 librt.so.1; do ln -sfn \$L/\$l /data/data/com.termux/gl/\$l; done
cd $R/home/user/.arduino15/packages
n=0
while IFS= read -r f; do cp -p --remove-destination "\$(readlink "\$f")" "\$f" && n=\$((n+1)); done < <(find esp32/tools/esp-x32 builtin -type l -lname '*/.l2s/*')
echo "materialized \$n proot hard links"
EOS
# Inside proot, /data/data/com.termux is a plain rootfs dir: give it its own loader link.
ln -sfn /usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1 /data/data/com.termux/ld
