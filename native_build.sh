#!/data/data/com.termux/files/usr/bin/bash
# Native (non-proot) firmware build. Run from Debian with: ./build-fast.sh
set -e
R=/data/data/com.termux/files/usr/var/lib/proot-distro/containers/debian/rootfs
W=$R/home/user/work
P=$W/padkey
export TERMUX_EXEC__SYSTEM_LINKER_EXEC__MODE=disable
export LD_LIBRARY_PATH=/data/data/com.termux/gl
export PATH=$P/tools/native-bin:$PATH
FQBN="esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB"
args=(--build-property "tools.esptool_py.path=$P/tools/native-bin")
while IFS= read -r line; do args+=(--build-property "$line"); done < $P/tools/native-overrides.txt
cd $P
exec env -u LD_PRELOAD $W/tools/bin/arduino-cli --config-file $P/tools/arduino-cli-native.yaml \
  compile -b "$FQBN" --build-path $P/.build --output-dir $P/build "${args[@]}" firmware/padkey "$@"
