// Message format shared by the WebSocket and BLE transports.
#pragma once
#include <stdint.h>

struct Msg {
  uint8_t len;
  uint8_t data[95];
};

enum : uint8_t {
  M_MOVE = 1, M_BTN = 2, M_SCROLL = 3, M_KEYDOWN = 4, M_KEYUP = 5, M_TAP = 6,
  M_TEXT = 7, M_CONSUMER = 8, M_RELEASE = 9, M_FX = 10, M_LABEL = 11
};

enum Fx : uint8_t { FX_NONE = 0, FX_RAINBOW, FX_STROBE, FX_POLICE, FX_FIRE, FX_MATRIX };
