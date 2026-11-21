"""Ask a running PadKey (Arduino TinyUSB CDC) to reboot into the ROM bootloader
using the DTR/RTS pattern the core listens for. Needs $TERMUX_USB_FD."""
import os, sys, time
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)) + "/..")
sys.path.insert(0, os.path.expanduser("~/work/tools"))
import serial
serial.protocol_handler_packages.append("usbserial_handlers")
s = serial.serial_for_url("termuxusb://", baudrate=115200, timeout=0.2)  # opens with DTR=RTS=1
for dtr, rts in [(0, 1), (1, 1), (1, 0), (0, 0)]:
    s._dtr_state, s._rts_state = bool(dtr), bool(rts)
    s._line_state()
    time.sleep(0.05)
print("reboot-to-bootloader sequence sent")
