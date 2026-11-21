"""PadKey bridge — runs on the host tablet (the one the T-Dongle is plugged into).

  * Text from the iPad (Chinese, emoji, anything) -> Android clipboard -> the
    dongle presses Ctrl+V.
  * New screenshots (/sdcard/DCIM/Screenshots, e.g. S Pen "Smart select") ->
    shrunk to JPEG -> sent through the dongle to the iPad's tracing page.

Talks to the dongle over its USB serial port via the fd that `termux-usb -E -e`
provides ($TERMUX_USB_FD). Started by bridge/run.sh.
"""
import glob
import io
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "tools"))
for sp in glob.glob(os.path.join(HERE, "..", "..", ".venv", "lib", "python3*", "site-packages")):
    sys.path.append(sp)  # pyserial lives in the project venv
import serial  # noqa: E402
from PIL import Image  # noqa: E402

serial.protocol_handler_packages.append("usbserial_handlers")

TERMUX_BIN = "/data/data/com.termux/files/usr/bin"
SHOT_DIRS = ["/sdcard/DCIM/Screenshots", "/sdcard/Pictures/Screenshots"]
MAX_SIDE = 1400          # px, longest side sent to the iPad
JPEG_QUALITY = 72

H_HELLO, H_PASTE, H_IMG_BEGIN, H_IMG_DATA, H_IMG_END = 1, 2, 3, 4, 5
D_HELLO, D_TEXT, D_ACK = 0x81, 0x82, 0x83


LOG_FILE = os.path.join(HERE, "..", "bridge.log")


def log(*a):
    line = " ".join(str(x) for x in (time.strftime("%H:%M:%S"),) + a)
    print(line, flush=True)
    with open(LOG_FILE, "a") as f:
        f.write(line + "\n")


class Link:
    def __init__(self):
        self.s = serial.serial_for_url("termuxusb://", baudrate=115200, timeout=0.05)
        self.buf = bytearray()

    def send(self, typ, payload=b""):
        self.s.write(bytes([0xA5, typ, len(payload) & 0xFF, len(payload) >> 8]) + payload)

    def frames(self, wait=0.0):
        """Yield complete frames received so far (optionally waiting a bit)."""
        end = time.time() + wait
        while True:
            chunk = self.s.read(4096)
            if chunk:
                self.buf += chunk
            while True:
                i = self.buf.find(b"\xa5")
                if i < 0:
                    self.buf.clear()
                    break
                del self.buf[:i]
                if len(self.buf) < 4:
                    break
                n = self.buf[2] | self.buf[3] << 8
                if len(self.buf) < 4 + n:
                    break
                yield self.buf[1], bytes(self.buf[4:4 + n])
                del self.buf[:4 + n]
            if time.time() >= end:
                return


def set_clipboard(text):
    subprocess.run([TERMUX_BIN + "/termux-clipboard-set"], input=text.encode(), timeout=10, check=True)


def newest_shot(since):
    best = None
    for d in SHOT_DIRS:
        for f in glob.glob(os.path.join(d, "*")):
            if not f.lower().endswith((".jpg", ".jpeg", ".png", ".webp")):
                continue
            try:
                m = os.path.getmtime(f)
            except OSError:
                continue
            if m > since and (best is None or m > best[0]):
                best = (m, f)
    return best


def encode_image(path):
    for _ in range(10):              # the screenshot may still be being written
        try:
            im = Image.open(path)
            im.load()
            break
        except Exception:
            time.sleep(0.3)
    else:
        raise RuntimeError("cannot read " + path)
    im = im.convert("RGB")
    im.thumbnail((MAX_SIDE, MAX_SIDE))
    out = io.BytesIO()
    im.save(out, "JPEG", quality=JPEG_QUALITY, optimize=True)
    return out.getvalue(), im.size


def send_image(link, path):
    data, (w, h) = encode_image(path)
    log(f"image {os.path.basename(path)} -> {w}x{h}, {len(data) // 1024} KB")
    t0 = time.time()
    link.send(H_IMG_BEGIN, f"{len(data)} {w} {h}".encode())
    for off in range(0, len(data), 1024):
        link.send(H_IMG_DATA, data[off:off + 1024])
        # wait for the dongle to push this chunk over the air
        deadline = time.time() + 10
        acked = False
        while time.time() < deadline and not acked:
            for typ, payload in link.frames(wait=0.05):
                if typ == D_ACK:
                    acked = True
                elif typ == D_TEXT:
                    handle_text(link, payload)
        if not acked:
            log("no ack from dongle, image aborted")
            return
    link.send(H_IMG_END)
    log(f"image sent in {time.time() - t0:.1f}s")


def handle_text(link, payload):
    text = payload.decode("utf-8", errors="replace")
    log("paste:", text[:40])
    try:
        set_clipboard(text)
        time.sleep(0.15)
        link.send(H_PASTE)
    except Exception as e:
        log("clipboard failed:", e)


def main():
    link = Link()
    log("bridge running")
    last_hello = 0
    shots_since = time.time()
    last_scan = 0
    while True:
        now = time.time()
        if now - last_hello > 2:
            link.send(H_HELLO)
            last_hello = now
        for typ, payload in link.frames(wait=0.1):
            if typ == D_TEXT:
                handle_text(link, payload)
        if now - last_scan > 1:
            last_scan = now
            shot = newest_shot(shots_since)
            if shot:
                shots_since = shot[0]
                try:
                    send_image(link, shot[1])
                except Exception as e:
                    log("image failed:", e)


if __name__ == "__main__":
    main()
