#!/bin/bash
# Build PadKey firmware. Output: build/padkey.ino.bin (+ merged image)
set -e
cd "$(dirname "$0")"
python3 tools/gen_assets.py
FQBN="esp32:esp32:esp32s3:USBMode=default,CDCOnBoot=cdc,FlashSize=16M,PartitionScheme=app3M_fat9M_16MB"
../tools/bin/arduino-cli compile -b "$FQBN" --output-dir build firmware/padkey "$@"
