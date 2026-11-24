#!/bin/sh
# PadKey 電腦截圖：截取整個螢幕 -> 存到這支 T-Dongle（PADKEY 隨身碟）-> 藍牙傳到 iPad。
# 不需要網路，也不用安裝任何東西。通常由 T-Dongle 自動打字執行；也可以手動執行：
#   sh /media/<使用者>/PADKEY/PADKEY.SH
d=${1:-$(dirname "$0")}
t=$(mktemp -d 2>/dev/null || echo /tmp/padkey$$) && mkdir -p "$t" || exit 1
trap 'rm -rf "$t"' EXIT
f=$t/s.png
have() { command -v "$1" >/dev/null 2>&1; }
ok() { [ -s "$f" ]; }
fail() { have notify-send && notify-send "PadKey" "$1"; echo "PadKey: $1" >&2; exit 1; }
sleep 0.8   # let the Run dialog close first

# 1) screenshot with whatever the desktop already has
if [ -n "$WAYLAND_DISPLAY" ]; then
  have grim && grim "$f" 2>/dev/null
  ok || { have gnome-screenshot && gnome-screenshot -f "$f" 2>/dev/null; }
  ok || { have spectacle && spectacle -b -n -f -o "$f" 2>/dev/null; }
fi
ok || { have xfce4-screenshooter && xfce4-screenshooter -f -s "$f" 2>/dev/null; }
ok || { have gnome-screenshot && gnome-screenshot -f "$f" 2>/dev/null; }
ok || { have spectacle && spectacle -b -n -f -o "$f" 2>/dev/null; }
ok || { have mate-screenshot && mate-screenshot -f "$f" 2>/dev/null; }
ok || { have scrot && scrot "$f" 2>/dev/null; }
ok || { have import && import -window root "$f" 2>/dev/null; }
ok || { have grim && grim "$f" 2>/dev/null; }
ok || { have ffmpeg && [ -n "$DISPLAY" ] && ffmpeg -loglevel error -y -f x11grab -i "$DISPLAY" -frames:v 1 "$f"; }
ok || fail "截圖失敗：找不到可用的截圖程式"

# 2) shrink to a JPEG (much faster over Bluetooth); keep the PNG if nothing can convert
o=$f; j=$t/s.jpg
if have python3 && python3 -c 'import PIL' 2>/dev/null; then
  python3 -c 'import sys
from PIL import Image
i = Image.open(sys.argv[1]).convert("RGB"); i.thumbnail((1600, 1600)); i.save(sys.argv[2], quality=78)' "$f" "$j" && o=$j
fi
[ "$o" = "$j" ] || { have ffmpeg && ffmpeg -loglevel error -y -i "$f" -vf "scale='min(1600,iw)':-2" -q:v 5 "$j" && o=$j; }
[ "$o" = "$j" ] || { have convert && convert "$f" -resize '1600x1600>' -quality 78 "$j" && o=$j; }

# 3) drop it into SHOT.BIN on the dongle: 512-byte text header + image, written in place
[ -f "$d/SHOT.BIN" ] || fail "找不到 PADKEY 隨身碟"
n=$(wc -c <"$o" | tr -d ' ')
[ "$n" -lt 10000000 ] || fail "截圖太大"
printf '%-511s\n' "PADKEY1 $(date +%s)$$ $n" >"$t/h"
cat "$t/h" "$o" >"$t/all"
dd if="$t/all" of="$d/SHOT.BIN" bs=65536 conv=notrunc,fsync 2>/dev/null || fail "寫入 T-Dongle 失敗"
sync
