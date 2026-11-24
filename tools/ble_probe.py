"""Reproduce the iPad's BLE role from the Pi: pair, auth, trigger a PC screenshot,
and count the image chunks that actually arrive over Bluetooth."""
import asyncio, glob, threading, time, sys
import serial
from bleak import BleakScanner, BleakClient

SVC = "6b1a0001-4d2f-4c8e-a5b1-7061646b6579"
CMD = "6b1a0002-4d2f-4c8e-a5b1-7061646b6579"
EVT = "6b1a0003-4d2f-4c8e-a5b1-7061646b6579"

ser = serial.Serial(glob.glob("/dev/ttyACM*")[0], 115200, timeout=0.1)
state = {"pin": None, "log": []}

def serial_reader():
    buf = bytearray(); last = 0
    while True:
        if time.time() - last > 2: ser.write(b"\xa5\x01\x00\x00"); last = time.time()
        buf += ser.read(4096)
        while True:
            i = buf.find(b"\xa5")
            if i < 0: buf.clear(); break
            del buf[:i]
            if len(buf) < 4: break
            n = buf[2] | buf[3] << 8
            if len(buf) < 4 + n: break
            t, pl = buf[1], bytes(buf[4:4 + n]); del buf[:4 + n]
            if t == 0x84:
                m = pl.decode(errors="replace"); state["log"].append(m); print("  [CDC]", m, flush=True)
                if m.startswith("pin "): state["pin"] = m[4:].strip()

threading.Thread(target=serial_reader, daemon=True).start()

rx = {"bin_chunks": 0, "bin_bytes": 0, "expect": None, "token": None, "authed": False, "first_dt": None, "last": 0}
t_img = [0]

async def main():
    print("掃描 PadKey…", flush=True)
    dev = await BleakScanner.find_device_by_filter(lambda d, a: d.name and "PadKey" in d.name, timeout=20)
    if not dev: print("找不到 PadKey（可能被 iPad 佔著）"); return
    
    print("連線", dev, flush=True)
    async with BleakClient(dev) as cli:
        try: await cli._acquire_mtu()
        except Exception: pass
        print("MTU", cli.mtu_size, flush=True)
        def on_evt(_, data):
            if data and data[0] == 1:
                rx["bin_chunks"] += 1; rx["bin_bytes"] += len(data) - 1; rx["last"] = time.time()
                if rx["first_dt"] is None and t_img[0]: rx["first_dt"] = time.time() - t_img[0]
            else:
                m = bytes(data).decode(errors="replace"); print("  [BLE len=%d]" % len(data), m, flush=True)
                if m.startswith("token "): rx["token"] = m[6:]
                if m.startswith("ok "): rx["authed"] = True
                if m.startswith("img "):
                    rx["expect"] = int(m.split()[1]); rx["bin_chunks"] = 0; rx["bin_bytes"] = 0; rx["first_dt"] = None; t_img[0] = time.time()
        await cli.start_notify(EVT, on_evt)
        await cli.write_gatt_char(CMD, b"pairstart", response=False)
        for _ in range(50):
            if state["pin"]: break
            await asyncio.sleep(0.1)
        if not state["pin"]: print("沒有拿到 PIN"); return
        print("PIN =", state["pin"], flush=True)
        for _ in range(6):
            await cli.write_gatt_char(CMD, ("pair " + state["pin"]).encode(), response=False)
            for _ in range(20):
                if rx["token"]: break
                await asyncio.sleep(0.1)
            if rx["token"]: break
            print("pair 重試", flush=True)
        if not rx["token"]: print("配對失敗"); return
        print("token =", rx["token"], flush=True)
        for attempt in range(6):
            await cli.write_gatt_char(CMD, ("auth " + rx["token"]).encode(), response=False)
            for _ in range(20):
                if rx["authed"]: break
                await asyncio.sleep(0.1)
            if rx["authed"]: break
            print("auth 重試", attempt + 1, flush=True)
        print("authed =", rx["authed"], "→ 觸發電腦截圖", flush=True)
        ser.write(b"\xa5\x06\x00\x00")                 # H_PCSHOT via USB
        # collect until the stream goes quiet
        start = time.time()
        while time.time() - start < 40:
            await asyncio.sleep(0.5)
            if rx["expect"] and rx["last"] and time.time() - rx["last"] > 6 and rx["bin_bytes"]:
                break
        print("\n==== 結果 ====")
        print("預期大小 :", rx["expect"], "bytes")
        print("收到 chunk:", rx["bin_chunks"])
        print("收到 bytes:", rx["bin_bytes"])
        print("第一個 chunk 延遲:", f'{rx["first_dt"]:.2f}s' if rx["first_dt"] else "從未收到")

asyncio.run(main())
