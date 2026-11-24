import serial, sys, time, glob
p = glob.glob('/dev/serial/by-id/*ESP32S3*if0[0-9]*') or glob.glob('/dev/ttyACM*')
s = serial.Serial(p[0], 115200, timeout=0.1)
buf = bytearray(); last = 0; end = time.time() + float(sys.argv[1])
if len(sys.argv) > 2: s.write(b"\xa5\x01\x00\x00"); time.sleep(0.3); s.write(b"\xa5\x06\x00\x00")
while time.time() < end:
    if time.time() - last > 2: s.write(b'\xa5\x01\x00\x00'); last = time.time()
    buf += s.read(4096)
    while True:
        i = buf.find(b'\xa5')
        if i < 0: buf.clear(); break
        del buf[:i]
        if len(buf) < 4: break
        n = buf[2] | buf[3] << 8
        if len(buf) < 4 + n: break
        t, pl = buf[1], bytes(buf[4:4+n]); del buf[:4+n]
        if t in (0x81, 0x84): print(time.strftime('%H:%M:%S'), hex(t), pl.decode(errors='replace'), flush=True)
