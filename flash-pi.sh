#!/bin/bash
# Flash build/ to a T-Dongle plugged into the Raspberry Pi (over ssh).
set -e
cd "$(dirname "$0")"
PI=${PI:-pi@10.86.227.244}
SSH="ssh -i $HOME/.ssh/padkey_pi -o BatchMode=yes $PI"
A0=$HOME/.arduino15/packages/esp32/hardware/esp32/4.0.0/tools/partitions/boot_app0.bin
$SSH 'mkdir -p ~/padkey-tools/fw'
scp -q -i ~/.ssh/padkey_pi build/padkey.ino.bootloader.bin build/padkey.ino.partitions.bin build/padkey.ino.bin "$A0" "$PI:padkey-tools/fw/"
$SSH 'bash -s' <<'EOF'
set -e
V=~/padkey-tools/venv/bin
port() { ls /dev/serial/by-id/*Espressif* 2>/dev/null | head -1; }
P=$(port); [ -n "$P" ] || { echo "找不到 T-Dongle"; exit 1; }
if ! lsusb -v -d 303a: 2>/dev/null | grep -q "Vendor Specific"; then
  echo "PadKey 執行中 → 切換到燒錄模式"
  $V/python - "$P" <<'PY'
import serial, sys, time
s = serial.Serial(sys.argv[1], 115200)
for dtr, rts in [(0, 1), (1, 1), (1, 0), (0, 0)]:
    s.dtr, s.rts = bool(dtr), bool(rts); time.sleep(0.05)
PY
  for i in $(seq 1 20); do sleep 1; P=$(port); [ -n "$P" ] && lsusb -d 303a:1001 >/dev/null && ! ls /dev/input/by-id/*Espressif* >/dev/null 2>&1 && break; done
fi
echo "燒錄中：$P"
F=~/padkey-tools/fw
$V/esptool --chip esp32s3 -p "$P" --before no-reset --after watchdog-reset write-flash -z \
  0x0 $F/padkey.ino.bootloader.bin 0x8000 $F/padkey.ino.partitions.bin 0xe000 $F/boot_app0.bin 0x10000 $F/padkey.ino.bin \
  2>&1 | tr '\r' '\n' | grep -E "Wrote|verified|reset|rror|Connected" || true
EOF
