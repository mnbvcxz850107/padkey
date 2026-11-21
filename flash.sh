#!/bin/bash
# Flash build/ to the T-Dongle over the tablet's USB port (via termux-usb).
# Switches a running PadKey into the bootloader automatically.
set -e
cd "$(dirname "$0")"
# Pause the bridge (it shares the USB port) until flashing is done.
touch .flashing; trap 'rm -f .flashing' EXIT
pkill -f "python3 -u .*padkey_bridge[.]py" 2>/dev/null && sleep 1
TU=/data/data/com.termux/files/usr/bin/termux-usb
PY=$HOME/work/.venv/bin/python
dev() { timeout 15 $TU -l | grep -o '/dev/bus/usb/[0-9/]*' | head -1; }
D=$(dev); [ -z "$D" ] && { echo "找不到 T-Dongle"; exit 1; }
timeout 90 $TU -r "$D" >/dev/null
if ! timeout 30 $TU -E -e "python3 $HOME/work/tools/usbdesc.py" "$D" | grep -q "class=ff"; then
  echo "PadKey 執行中 → 切換到燒錄模式"
  timeout 30 $TU -E -e "$PY $PWD/tools/to_bootloader.py" "$D" || true
  for i in $(seq 1 30); do
    sleep 1
    N=$(dev)
    if [ -n "$N" ] && timeout 60 $TU -r "$N" >/dev/null 2>&1 && timeout 20 $TU -E -e "python3 $HOME/work/tools/usbdesc.py" "$N" 2>/dev/null | grep -q "class=ff"; then D=$N; break; fi
  done
fi
echo "燒錄中：$D"
A0=$HOME/.arduino15/packages/esp32/hardware/esp32/4.0.0/tools/partitions/boot_app0.bin
B=$PWD/build
$HOME/work/tools/espusb --before no-reset --after watchdog-reset write-flash -z \
  0x0 $B/padkey.ino.bootloader.bin 0x8000 $B/padkey.ino.partitions.bin 0xe000 $A0 0x10000 $B/padkey.ino.bin \
  2>&1 | tr '\r' '\n' | grep -v -E "━|^\s*$" | grep -E "Wrote|verified|Hard|reset|rror|Connected"
