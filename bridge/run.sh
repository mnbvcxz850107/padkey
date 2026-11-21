#!/bin/bash
# Keeps the PadKey bridge running; reconnects whenever the dongle comes back.
# Start (inside Debian):  bash ~/work/padkey/bridge/run.sh
HERE="$(cd "$(dirname "$0")" && pwd)"
TU=/data/data/com.termux/files/usr/bin/termux-usb
LOCK="$HERE/../.flashing"
/data/data/com.termux/files/usr/bin/termux-wake-lock 2>/dev/null
echo "PadKey bridge：保持這個視窗開著（可以切到背景）。按 Ctrl+C 結束。"
while true; do
  if [ -e "$LOCK" ]; then sleep 2; continue; fi       # flash.sh is using the port
  D=$(timeout 15 $TU -l 2>/dev/null | grep -o '/dev/bus/usb/[0-9/]*' | head -1)
  if [ -z "$D" ]; then sleep 3; continue; fi
  timeout 120 $TU -r "$D" >/dev/null 2>&1 || { sleep 3; continue; }
  # Only a running PadKey has the HID interface (the ROM bootloader does not).
  if ! timeout 20 $TU -E -e "python3 $HOME/work/tools/usbdesc.py" "$D" 2>/dev/null | grep -q "class=03"; then sleep 3; continue; fi
  $TU -E -e "/usr/bin/python3 -u $HERE/padkey_bridge.py" "$D"
  echo "（連線中斷，3 秒後重新連線）"; sleep 3
done
