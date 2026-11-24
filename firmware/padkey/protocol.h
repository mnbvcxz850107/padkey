// Message format shared by the WebSocket and BLE transports.
#pragma once
#include <stdint.h>

struct Msg {
  uint8_t len;
  uint8_t data[95];
};

enum : uint8_t {
  M_MOVE = 1, M_BTN = 2, M_SCROLL = 3, M_KEYDOWN = 4, M_KEYUP = 5, M_TAP = 6,
  M_TEXT = 7, M_CONSUMER = 8, M_RELEASE = 9, M_FX = 10, M_LABEL = 11,
  M_UTEXT = 12,  // UTF-8 text for the tablet-side bridge: [12, flags(1=first,2=last), bytes...]
  M_LED = 13,     // [13, on] RGB LED on/off (remembered)
  M_BATCH = 14,   // [14, len, msg..., len, msg...] several messages in one BLE write
  M_PCSHOT = 15   // [15] screenshot of the computer (Alt+F2 Run dialog + PADKEY drive)
};

enum Fx : uint8_t { FX_NONE = 0, FX_RAINBOW, FX_STROBE, FX_POLICE, FX_FIRE, FX_MATRIX };

// USB serial link to the bridge program on the host tablet. Frame:
//   0xA5, type, len_lo, len_hi, payload[len]
enum : uint8_t {
  H_HELLO = 1,      // host -> dongle: heartbeat, dongle answers D_HELLO
  H_PASTE = 2,      // host -> dongle: clipboard is ready, press Ctrl+V
  H_IMG_BEGIN = 3,  // host -> dongle: payload "<size> <w> <h>" (ASCII)
  H_IMG_DATA = 4,   // host -> dongle: image bytes (<= 1024), dongle answers D_ACK
  H_IMG_END = 5,    // host -> dongle
  H_PCSHOT = 6,     // host -> dongle: same as the iPad's M_PCSHOT (for testing)
  D_HELLO = 0x81,   // dongle -> host
  D_TEXT = 0x82,    // dongle -> host: UTF-8 text to paste
  D_ACK = 0x83,     // dongle -> host: image chunk forwarded
  D_LOG = 0x84,     // dongle -> host: short status line (only while a host program says hello)
};
