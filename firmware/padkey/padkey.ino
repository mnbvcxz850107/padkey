// PadKey — turn an iPad into a keyboard / touchpad for another device.
// Board: LilyGO T-Dongle-S3 (ESP32-S3). The iPad web app talks to this
// dongle over Wi-Fi (WebSocket); the dongle acts as a USB HID keyboard,
// mouse and media-key device for whatever it is plugged into.

#include <Arduino.h>
#include <WiFi.h>
#include <ESPmDNS.h>
#include <Preferences.h>
#include <Update.h>
#include <ESPAsyncWebServer.h>
#include <BLE.h>
#include "USB.h"
#include "USBHIDKeyboard.h"
#include "USBHIDMouse.h"
#include "USBHIDConsumerControl.h"
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSansBold18pt7b.h>
#include "web_assets.h"
#include "protocol.h"

#define FW_VERSION "1.7.0"

// ---- T-Dongle-S3 pins (from LilyGO examples) ----
#define PIN_LCD_MOSI 3
#define PIN_LCD_SCLK 5
#define PIN_LCD_CS   4
#define PIN_LCD_DC   2
#define PIN_LCD_RST  1
#define PIN_LCD_BL   38   // active LOW
#define PIN_LED_DI   40   // APA102 data
#define PIN_LED_CI   39   // APA102 clock
#define PIN_BUTTON   0

// ---------------------------------------------------------------- HID ----
USBHIDKeyboard Keyboard;
USBHIDMouse Mouse;
USBHIDConsumerControl Consumer;
static volatile bool usbUp = false;
static volatile uint32_t fwdPkts = 0, fwdGiveup = 0;  // last image: BLE packets sent / given up
static volatile uint16_t bleChunk = 513;  // (diagnostic only)
static volatile bool capsOn = false;      // host Caps Lock LED
static volatile bool statusDirty = false;
static bool ledEnabled = false;   // RGB LED off by default (user preference, stored in NVS)
static KeyReport report = {};

static void keyDown(uint8_t k) {
  if (k >= 0xE0 && k <= 0xE7) {
    report.modifiers |= 1 << (k - 0xE0);
  } else {
    for (int i = 0; i < 6; i++) if (report.keys[i] == k) return;
    for (int i = 0; i < 6; i++) if (!report.keys[i]) { report.keys[i] = k; break; }
  }
  Keyboard.sendReport(&report);
}

static void keyUp(uint8_t k) {
  if (k >= 0xE0 && k <= 0xE7) {
    report.modifiers &= ~(1 << (k - 0xE0));
  } else {
    for (int i = 0; i < 6; i++) if (report.keys[i] == k) report.keys[i] = 0;
  }
  Keyboard.sendReport(&report);
}

static uint8_t mouseButtons = 0;
static void releaseAll() {
  memset(&report, 0, sizeof(report));
  Keyboard.sendReport(&report);
  for (uint8_t b : {MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE})
    if (mouseButtons & b) Mouse.release(b);
  mouseButtons = 0;
  Consumer.release();
}

static void mouseMove(int dx, int dy) {
  while (dx || dy) {
    int sx = constrain(dx, -127, 127), sy = constrain(dy, -127, 127);
    Mouse.move(sx, sy);
    dx -= sx; dy -= sy;
  }
}

static void setButtons(uint8_t mask) {
  for (uint8_t b : {MOUSE_LEFT, MOUSE_RIGHT, MOUSE_MIDDLE}) {
    if ((mask & b) && !(mouseButtons & b)) Mouse.press(b);
    if (!(mask & b) && (mouseButtons & b)) Mouse.release(b);
  }
  mouseButtons = mask;
}

// ------------------------------------------------------------ settings ----
Preferences prefs;
String token, wifiSsid, wifiPass, apPass, apSsid;
static char pairPin[8] = "";
uint32_t pinShowUntil = 0;
int pinFails = 0;
bool apMode = false;

static String randomDigits(int n) {
  String s;
  for (int i = 0; i < n; i++) s += char('0' + esp_random() % 10);
  return s;
}

static String randomHex(int n) {
  String s;
  const char *h = "0123456789abcdef";
  for (int i = 0; i < n; i++) s += h[esp_random() & 15];
  return s;
}

static void loadSettings() {
  prefs.begin("padkey", false);
  token = prefs.getString("token", "");
  if (token.length() != 32) { token = randomHex(32); prefs.putString("token", token); }
  wifiSsid = prefs.getString("ssid", "");
  wifiPass = prefs.getString("pass", "");
  apPass = prefs.getString("appass", "");
  ledEnabled = prefs.getBool("led", false);
  if (apPass.length() < 8) { apPass = randomDigits(8); prefs.putString("appass", apPass); }
  uint64_t efuse = ESP.getEfuseMac();
  uint8_t mac[6];
  for (int i = 0; i < 6; i++) mac[i] = (efuse >> (8 * i)) & 0xFF;
  char buf[24];
  snprintf(buf, sizeof(buf), "PadKey-%02X%02X", mac[4], mac[5]);
  apSsid = buf;
}

// ------------------------------------------------------------------ LED ----
static void ledShow(uint8_t r, uint8_t g, uint8_t b, uint8_t bright = 31) {
  auto out = [](uint8_t v) {
    for (int i = 7; i >= 0; i--) {
      digitalWrite(PIN_LED_DI, (v >> i) & 1);
      digitalWrite(PIN_LED_CI, 1);
      digitalWrite(PIN_LED_CI, 0);
    }
  };
  for (int i = 0; i < 4; i++) out(0);
  out(0xE0 | (bright & 31)); out(b); out(g); out(r);
  for (int i = 0; i < 4; i++) out(0xFF);
}

static void hsv(uint8_t h, uint8_t &r, uint8_t &g, uint8_t &b) {
  uint8_t region = h / 43, rem = (h - region * 43) * 6;
  uint8_t q = 255 - rem, t = rem;
  switch (region) {
    case 0: r = 255; g = t; b = 0; break;
    case 1: r = q; g = 255; b = 0; break;
    case 2: r = 0; g = 255; b = t; break;
    case 3: r = 0; g = q; b = 255; break;
    case 4: r = t; g = 0; b = 255; break;
    default: r = 255; g = 0; b = q; break;
  }
}

// --------------------------------------------------------------- Display ----
class TFT : public Adafruit_ST7735 {
 public:
  using Adafruit_ST7735::Adafruit_ST7735;
  void fixOffsets() { setColRowStart(26, 1); }
};
TFT tft(&SPI, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);
GFXcanvas16 canvas(160, 80);


static Fx fx = FX_NONE;
static uint32_t fxUntil = 0;
static String lastLabel;
static uint32_t lastLabelAt = 0;
static uint16_t labelColor = 0x07FF;
static int clientCount = 0, wsCount = 0, bleCount = 0;
static bool infoPage = false;
static uint32_t infoUntil = 0;
static char bigMsg[24] = "";
static uint32_t bigMsgUntil = 0;

static void showMsg(const char *m, uint32_t ms) {
  strlcpy(bigMsg, m, sizeof(bigMsg));
  bigMsgUntil = millis() + ms;
}
static void newPin() {
  for (int i = 0; i < 6; i++) pairPin[i] = '0' + esp_random() % 10;
  pairPin[6] = 0;
}

static uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
}

static void centerText(const String &s, int y, const GFXfont *f, uint16_t c, uint8_t size = 1) {
  canvas.setFont(f);
  canvas.setTextSize(size);
  int16_t x1, y1; uint16_t w, h;
  canvas.getTextBounds(s, 0, 0, &x1, &y1, &w, &h);
  canvas.setTextColor(c);
  canvas.setCursor((160 - (int)w) / 2 - x1, y);
  canvas.print(s);
}

static String ipString() {
  return apMode ? WiFi.softAPIP().toString() : WiFi.localIP().toString();
}

static void drawFrame() {
  uint32_t now = millis();
  canvas.fillScreen(0);
  uint8_t r, g, b;

  if (fx != FX_NONE && now < fxUntil) {
    uint32_t t = now / 16;
    switch (fx) {
      case FX_RAINBOW:
        for (int x = 0; x < 160; x++) { hsv((x * 2 + t * 4) & 255, r, g, b); canvas.drawFastVLine(x, 0, 80, rgb565(r, g, b)); }
        centerText("RAINBOW", 50, &FreeSansBold9pt7b, 0xFFFF);
        break;
      case FX_STROBE:
        if ((now / 60) & 1) canvas.fillScreen(0xFFFF);
        centerText("FLASH!", 52, &FreeSansBold18pt7b, (now / 60) & 1 ? 0 : 0xFFE0);
        break;
      case FX_POLICE:
        if ((now / 150) & 1) canvas.fillRect(0, 0, 80, 80, 0xF800); else canvas.fillRect(80, 0, 80, 80, 0x001F);
        centerText("ALERT", 50, &FreeSansBold18pt7b, 0xFFFF);
        break;
      case FX_FIRE:
        for (int x = 0; x < 160; x += 4) {
          int h = 30 + (esp_random() % 45);
          for (int y = 0; y < h; y += 4) {
            uint8_t heat = 255 - y * 255 / h;
            canvas.fillRect(x, 80 - y - 4, 4, 4, rgb565(255, heat * 3 / 4, heat / 6));
          }
        }
        centerText("FIRE", 50, &FreeSansBold18pt7b, 0xFFFF);
        break;
      case FX_MATRIX:
        for (int i = 0; i < 40; i++) {
          int x = (esp_random() % 27) * 6, y = esp_random() % 80;
          canvas.setFont(nullptr); canvas.setTextSize(1);
          canvas.setTextColor(rgb565(0, 120 + esp_random() % 136, 40));
          canvas.setCursor(x, y); canvas.print(char('!' + esp_random() % 90));
        }
        centerText("HACKING...", 48, &FreeSansBold9pt7b, 0xFFFF);
        break;
      default: break;
    }
    return;
  }
  fx = FX_NONE;

  // ---- Clean HUD layout: static, white / cyan / gray only, no moving colours.
  const uint16_t ACC = rgb565(0, 210, 255), ACC_DIM = rgb565(0, 70, 92);
  const uint16_t TXT = 0xFFFF, DIM = rgb565(110, 122, 145), OFF = rgb565(58, 64, 80);
  const uint16_t OK = rgb565(40, 225, 120), WARN = rgb565(255, 176, 0);
  auto small = [&](int x, int y, const String &t, uint16_t c) {
    canvas.setFont(nullptr); canvas.setTextSize(1); canvas.setTextColor(c); canvas.setCursor(x, y); canvas.print(t);
  };
  auto rightSmall = [&](int y, const String &t, uint16_t c) { small(155 - (int)t.length() * 6, y, t, c); };
  auto dot = [&](int x, const char *lbl, bool on) {
    canvas.fillCircle(x + 2, 7, 2, on ? OK : OFF);
    small(x + 7, 4, lbl, on ? TXT : DIM);
  };
  // corner brackets
  const int L = 6;
  canvas.drawFastHLine(0, 0, L, ACC_DIM);   canvas.drawFastVLine(0, 0, L, ACC_DIM);
  canvas.drawFastHLine(160 - L, 0, L, ACC_DIM); canvas.drawFastVLine(159, 0, L, ACC_DIM);
  canvas.drawFastHLine(0, 79, L, ACC_DIM);  canvas.drawFastVLine(0, 80 - L, L, ACC_DIM);
  canvas.drawFastHLine(160 - L, 79, L, ACC_DIM); canvas.drawFastVLine(159, 80 - L, L, ACC_DIM);
  // header: name + link status
  bool wifiOn = apMode || WiFi.status() == WL_CONNECTED;
  small(6, 4, "PADKEY", ACC);
  dot(70, "USB", usbUp);
  dot(98, "BLE", bleCount > 0);
  dot(126, "WIFI", wifiOn);
  canvas.drawFastHLine(6, 15, 148, ACC_DIM);
  canvas.drawFastHLine(6, 15, 22, ACC);
  // footer
  canvas.drawFastHLine(6, 66, 148, ACC_DIM);
  rightSmall(70, "v" FW_VERSION, DIM);

  if (pairPin[0] && now < pinShowUntil) {
    centerText("PAIRING PIN", 22, nullptr, DIM);
    centerText(String(pairPin), 58, &FreeSansBold18pt7b, TXT);
    small(6, 70, "ENTER ON IPAD", ACC);
    return;
  }
  if (bigMsg[0] && now < bigMsgUntil) {
    centerText(String(bigMsg), 46, &FreeSansBold9pt7b, TXT);
    small(6, 70, clientCount ? "LINKED" : "STANDBY", ACC);
    return;
  }

  bool showInfo = (infoPage && now < infoUntil) || clientCount == 0;
  if (showInfo) {
    auto row = [&](int y, const char *k, const String &v, uint16_t c) { small(6, y, k, DIM); small(42, y, v, c); };
    if (apMode) {
      row(22, "WIFI", apSsid.substring(0, 18), TXT);
      row(36, "PASS", apPass.substring(0, 18), WARN);
      row(50, "URL", ipString(), ACC);
    } else if (WiFi.status() == WL_CONNECTED) {
      row(22, "WIFI", wifiSsid.substring(0, 18), TXT);
      row(36, "URL", ipString(), ACC);
      row(50, "HOST", "padkey.local", ACC);
    } else {
      row(22, "WIFI", "connecting...", DIM);
      row(36, "BLE", apSsid.substring(0, 18), TXT);
    }
    small(6, 70, clientCount ? "LINKED" : "WAITING FOR IPAD", clientCount ? ACC : DIM);
    return;
  }

  // Connected: last action big (white, fading), otherwise READY.
  small(6, 70, "LINKED", ACC);
  uint32_t age = now - lastLabelAt;
  if (lastLabel.length() && age < 2500) {
    uint8_t v = age < 1500 ? 255 : 255 - (age - 1500) * 255 / 1000;
    const GFXfont *f = lastLabel.length() <= 6 ? &FreeSansBold18pt7b : &FreeSansBold9pt7b;
    centerText(lastLabel, lastLabel.length() <= 6 ? 54 : 46, f, rgb565(v, v, v));
  } else {
    centerText("READY", usbUp ? 54 : 48, &FreeSansBold18pt7b, TXT);
    if (!usbUp) centerText("NO USB HOST", 54, nullptr, WARN);
  }
}

static void updateLed() {
  static bool dark = false;
  if (!ledEnabled) { if (!dark) { ledShow(0, 0, 0, 0); dark = true; } return; }
  dark = false;
  uint32_t now = millis();
  uint8_t r, g, b;
  if (fx != FX_NONE && now < fxUntil) {
    switch (fx) {
      case FX_RAINBOW: hsv(now / 4, r, g, b); ledShow(r, g, b); return;
      case FX_STROBE: if ((now / 60) & 1) ledShow(255, 255, 255); else ledShow(0, 0, 0); return;
      case FX_POLICE: if ((now / 150) & 1) ledShow(255, 0, 0); else ledShow(0, 0, 255); return;
      case FX_FIRE: ledShow(255, 40 + esp_random() % 80, 0); return;
      case FX_MATRIX: ledShow(0, 100 + esp_random() % 155, 20); return;
      default: break;
    }
  }
  if (pairPin[0] && now < pinShowUntil) { ledShow((now / 300) & 1 ? 255 : 0, 0, 255, 8); return; }
  uint32_t age = now - lastLabelAt;
  if (age < 150) {
    ledShow(((labelColor >> 11) & 31) * 8, ((labelColor >> 5) & 63) * 4, (labelColor & 31) * 8, 20);
    return;
  }
  // Idle "breathing": cyan when an iPad is connected, purple otherwise.
  float br = (sin(now / 700.0) + 1) / 2;
  uint8_t v = 10 + br * 90;
  if (clientCount) ledShow(0, v, v, 4); else ledShow(v, 0, v, 4);
}

static void setLabel(const String &s, uint16_t color) {
  lastLabel = s.substring(0, 14);
  lastLabelAt = millis();
  labelColor = color;
}

// -------------------------------------------------- input message queue ----
// WebSocket callbacks run in the network task; HID reports are sent from loop().
static QueueHandle_t queue;


static void enqueueOne(const uint8_t *d, size_t n);
// A batch carries several messages in one radio write (much faster typing over BLE).
static void enqueue(const uint8_t *d, size_t n) {
  if (n >= 2 && d[0] == M_BATCH) {
    size_t i = 1;
    while (i < n) {
      size_t len = d[i++];
      if (!len || i + len > n) break;
      enqueueOne(d + i, len);
      i += len;
    }
    return;
  }
  enqueueOne(d, n);
}

static void enqueueOne(const uint8_t *d, size_t n) {
  Msg m;
  m.len = min(n, sizeof(m.data));
  memcpy(m.data, d, m.len);
  xQueueSend(queue, &m, 0);
}

static void pcshotTrigger(uint8_t how);

static void handleMsg(const Msg &m, int &accX, int &accY) {
  const uint8_t *d = m.data;
  switch (d[0]) {
    case M_MOVE:
      if (m.len >= 5) { accX += (int16_t)(d[1] | d[2] << 8); accY += (int16_t)(d[3] | d[4] << 8); }
      return;
    default: break;
  }
  if (accX || accY) { mouseMove(accX, accY); accX = accY = 0; }
  switch (d[0]) {
    case M_BTN: if (m.len >= 2) setButtons(d[1] & 7); break;
    case M_SCROLL: if (m.len >= 3) Mouse.move(0, 0, (int8_t)d[1], (int8_t)d[2]); break;
    case M_KEYDOWN: if (m.len >= 2) keyDown(d[1]); break;
    case M_KEYUP: if (m.len >= 2) keyUp(d[1]); break;
    case M_TAP:
      if (m.len >= 3) {
        for (int i = 0; i < 8; i++) if (d[1] & (1 << i)) keyDown(0xE0 + i);
        keyDown(d[2]); delay(8); keyUp(d[2]);
        for (int i = 0; i < 8; i++) if (d[1] & (1 << i)) keyUp(0xE0 + i);
      }
      break;
    case M_TEXT:
      for (int i = 1; i < m.len; i++) {
        uint8_t c = d[i];
        if (capsOn && isalpha(c)) c ^= 0x20;  // Caps Lock on the host flips letters; flip back
        if (c == '\n' || (c >= 32 && c < 127) || c == '\t') { Keyboard.write(c); }
      }
      Keyboard.sendReport(&report);  // restore held keys
      break;
    case M_CONSUMER:
      if (m.len >= 3) { Consumer.press(d[1] | d[2] << 8); delay(10); Consumer.release(); }
      break;
    case M_RELEASE: releaseAll(); break;
    case M_FX:
      if (m.len >= 2) { fx = (Fx)d[1]; fxUntil = millis() + 3000; }
      break;
    case M_PCSHOT: pcshotTrigger(m.len >= 2 ? d[1] : 0); break;
    case M_LED:
      if (m.len >= 2) { ledEnabled = d[1] != 0; prefs.putBool("led", ledEnabled); statusDirty = true; }
      break;
    case M_LABEL:
      if (m.len >= 3) {
        uint16_t color = d[1] | d[2] << 8;
        String txt; txt.concat((const char *)d + 3, m.len - 3);
        setLabel(txt, color);
      }
      break;
  }
}

// ------------------------------------------------------- Host bridge ----
// A small program on the host tablet (via USB serial) pastes Unicode text
// for us and streams screenshots to the iPad.
static SemaphoreHandle_t cdcLock;
static volatile uint32_t bridgeSeen = 0;      // millis() of last heartbeat
static bool bridgeAlive() { return bridgeSeen && millis() - bridgeSeen < 6000; }

static void cdcSend(uint8_t type, const uint8_t *p, size_t n) {
  if (!cdcLock) return;
  uint8_t h[4] = {0xA5, type, (uint8_t)(n & 0xFF), (uint8_t)(n >> 8)};
  xSemaphoreTake(cdcLock, portMAX_DELAY);
  Serial.write(h, 4);
  if (n) Serial.write(p, n);
  xSemaphoreGive(cdcLock);
}

// Reassembles M_UTEXT pieces and hands the text to the bridge. Returns a status
// string for the sender ("" = in progress).
static uint8_t utextBuf[1024];
static size_t utextLen = 0;
static portMUX_TYPE utextMux = portMUX_INITIALIZER_UNLOCKED;
static String handleUtext(const uint8_t *d, size_t n) {
  if (n < 2) return "";
  uint8_t flags = d[1];
  portENTER_CRITICAL(&utextMux);
  if (flags & 1) utextLen = 0;
  size_t take = min(n - 2, sizeof(utextBuf) - utextLen);
  memcpy(utextBuf + utextLen, d + 2, take);
  utextLen += take;
  portEXIT_CRITICAL(&utextMux);
  if (!(flags & 2)) return "";
  if (!bridgeAlive()) return "nobridge";
  cdcSend(D_TEXT, utextBuf, utextLen);
  return "sent";
}

// ------------------------------------------------------------- Network ----
AsyncWebServer server(80);
AsyncWebSocket ws("/ws");
static std::vector<uint32_t> authed;
static portMUX_TYPE authMux = portMUX_INITIALIZER_UNLOCKED;
static uint32_t rebootAt = 0;
static bool otaOk = false;

static bool isAuthed(uint32_t id) {
  bool r = false;
  portENTER_CRITICAL(&authMux);
  for (auto a : authed) if (a == id) r = true;
  portEXIT_CRITICAL(&authMux);
  return r;
}

static void setAuthed(uint32_t id, bool on) {
  portENTER_CRITICAL(&authMux);
  authed.erase(std::remove(authed.begin(), authed.end(), id), authed.end());
  if (on) authed.push_back(id);
  wsCount = authed.size();
  clientCount = wsCount + bleCount;
  portEXIT_CRITICAL(&authMux);
}

static bool tokenOk(AsyncWebServerRequest *r) {
  return r->hasParam("t") && r->getParam("t")->value() == token;
}

static String jsonEsc(String v) {
  v.replace("\\", "\\\\"); v.replace("\"", "\\\"");
  return v;
}

static void pairStart() {
  if (!pairPin[0] || millis() > pinShowUntil) newPin();
  pinShowUntil = millis() + 120000;
  pinFails = 0;
}

// Returns true (and clears the PIN) when the PIN shown on screen matches.
static bool pairTry(const String &pin) {
  if (pairPin[0] && millis() < pinShowUntil && pin == pairPin) {
    pairPin[0] = 0; pinShowUntil = 0;
    showMsg("PAIRED!", 2000);
    return true;
  }
  if (++pinFails >= 5) { newPin(); pinFails = 0; }
  return false;
}

static String statusJson() {
  String s = "{\"usb\":";
  s += usbUp ? "true" : "false";
  s += ",\"caps\":"; s += capsOn ? "true" : "false";
  s += ",\"bridge\":"; s += bridgeAlive() ? "true" : "false";
  s += ",\"ap\":"; s += apMode ? "true" : "false";
  s += ",\"ip\":\""; s += ipString();
  s += "\",\"ssid\":\""; s += jsonEsc(apMode ? apSsid : wifiSsid);
  s += "\",\"rssi\":"; s += apMode ? 0 : WiFi.RSSI();
  s += ",\"fw\":\"" FW_VERSION "\"}";
  return s;
}

static String privateStatusJson() {
  String s = statusJson();
  String ap = apPass; ap.replace("\\", "\\\\"); ap.replace("\"", "\\\"");
  s.remove(s.length() - 1);
  s += ",\"apSsid\":\"" + apSsid + "\",\"apPass\":\"" + ap + "\",\"home\":\"";
  String h = wifiSsid; h.replace("\\", "\\\\"); h.replace("\"", "\\\"");
  s += h + "\"}";
  return s;
}

static void onWs(AsyncWebSocket *s, AsyncWebSocketClient *c, AwsEventType type, void *arg, uint8_t *data, size_t len) {
  if (type == WS_EVT_DISCONNECT) {
    bool was = isAuthed(c->id());
    setAuthed(c->id(), false);
    if (was && clientCount == 0) { uint8_t m = M_RELEASE; enqueue(&m, 1); }
    return;
  }
  if (type != WS_EVT_DATA) return;
  AwsFrameInfo *info = (AwsFrameInfo *)arg;
  if (!info->final || info->index != 0 || info->len != len) return;
  if (info->opcode == WS_TEXT) {
    String msg((const char *)data, len);
    if (msg.startsWith("auth ")) {
      bool ok = msg.substring(5) == token;
      setAuthed(c->id(), ok);
      c->text(ok ? String("ok ") + privateStatusJson() : String("bad"));
      if (ok) { const uint8_t hello[] = {M_LABEL, 0xFF, 0x07, 'H', 'E', 'L', 'L', 'O'}; enqueue(hello, sizeof(hello)); }
      else c->close();
    }
    return;
  }
  if (info->opcode == WS_BINARY && len > 0 && isAuthed(c->id())) {
    if (data[0] == M_UTEXT) { String r = handleUtext(data, len); if (r.length()) c->text("utext " + r); }
    else enqueue(data, len);
  }
}

static void setupWeb() {
  ws.onEvent(onWs);
  server.addHandler(&ws);

  auto gz = [](const char *type, const uint8_t *buf, size_t n) {
    return [=](AsyncWebServerRequest *r) {
      AsyncWebServerResponse *res = r->beginResponse(200, type, buf, n);
      res->addHeader("Content-Encoding", "gzip");
      res->addHeader("Cache-Control", "no-cache");
      r->send(res);
    };
  };
  server.on("/", HTTP_GET, gz("text/html", index_html_gz, index_html_gz_len));
  server.on("/manifest.json", HTTP_GET, gz("application/manifest+json", manifest_json_gz, manifest_json_gz_len));
  server.on("/icon.png", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(r->beginResponse(200, "image/png", icon_png, icon_png_len));
  });
  server.on("/apple-touch-icon.png", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(r->beginResponse(200, "image/png", icon_png, icon_png_len));
  });

  server.on("/api/info", HTTP_GET, [](AsyncWebServerRequest *r) {
    r->send(200, "application/json", statusJson());
  });

  // Show a fresh PIN on the dongle screen.
  server.on("/api/pairstart", HTTP_POST, [](AsyncWebServerRequest *r) {
    pairStart();
    r->send(200, "application/json", "{\"ok\":true}");
  });

  server.on("/api/pair", HTTP_POST, [](AsyncWebServerRequest *r) {
    String pin = r->hasParam("pin", true) ? r->getParam("pin", true)->value() : "";
    if (pairTry(pin)) {
      r->send(200, "application/json", String("{\"token\":\"") + token + "\"}");
      return;
    }
    r->send(403, "application/json", "{\"error\":\"bad pin\"}");
  });

  server.on("/api/scan", HTTP_GET, [](AsyncWebServerRequest *r) {
    if (!tokenOk(r)) { r->send(401); return; }
    int n = WiFi.scanComplete();
    if (n == WIFI_SCAN_FAILED) { WiFi.scanNetworks(true); r->send(202, "application/json", "[]"); return; }
    if (n == WIFI_SCAN_RUNNING) { r->send(202, "application/json", "[]"); return; }
    String s = "[";
    for (int i = 0; i < n; i++) {
      if (i) s += ",";
      String ssid = WiFi.SSID(i);
      ssid.replace("\\", "\\\\"); ssid.replace("\"", "\\\"");
      s += "{\"ssid\":\"" + ssid + "\",\"rssi\":" + WiFi.RSSI(i) + ",\"open\":" +
           (WiFi.encryptionType(i) == WIFI_AUTH_OPEN ? "true" : "false") + "}";
    }
    s += "]";
    WiFi.scanDelete();
    WiFi.scanNetworks(true);
    r->send(200, "application/json", s);
  });

  server.on("/api/wifi", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!tokenOk(r)) { r->send(401); return; }
    String ssid = r->hasParam("ssid", true) ? r->getParam("ssid", true)->value() : "";
    String pass = r->hasParam("pass", true) ? r->getParam("pass", true)->value() : "";
    prefs.putString("ssid", ssid);
    prefs.putString("pass", pass);
    showMsg("WIFI SAVED", 3000);
    r->send(200, "application/json", "{\"ok\":true}");
    rebootAt = millis() + 1500;
  });

  server.on("/api/appass", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!tokenOk(r)) { r->send(401); return; }
    String pass = r->hasParam("pass", true) ? r->getParam("pass", true)->value() : "";
    if (pass.length() < 8 || pass.length() > 63) { r->send(400, "application/json", "{\"error\":\"8-63 chars\"}"); return; }
    prefs.putString("appass", pass);
    showMsg("HOTSPOT SAVED", 3000);
    r->send(200, "application/json", "{\"ok\":true}");
    rebootAt = millis() + 1500;
  });

  // Forget home Wi-Fi => hotspot-only mode.
  server.on("/api/wififorget", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!tokenOk(r)) { r->send(401); return; }
    prefs.remove("ssid"); prefs.remove("pass");
    showMsg("HOTSPOT MODE", 3000);
    r->send(200, "application/json", "{\"ok\":true}");
    rebootAt = millis() + 1500;
  });

  server.on("/api/reboot", HTTP_POST, [](AsyncWebServerRequest *r) {
    if (!tokenOk(r)) { r->send(401); return; }
    r->send(200, "application/json", "{\"ok\":true}");
    rebootAt = millis() + 500;
  });

  // Firmware update over Wi-Fi: POST multipart file to /api/update?t=TOKEN
  server.on("/api/update", HTTP_POST,
    [](AsyncWebServerRequest *r) {
      bool ok = otaOk && !Update.hasError();
      r->send(ok ? 200 : 500, "text/plain", ok ? "OK, rebooting\n" : "FAILED\n");
      if (ok) rebootAt = millis() + 800;
    },
    [](AsyncWebServerRequest *r, String filename, size_t index, uint8_t *data, size_t len, bool final) {
      if (index == 0) {
        otaOk = tokenOk(r);
        if (!otaOk) return;
        showMsg("UPDATING...", 60000);
        if (!Update.begin(UPDATE_SIZE_UNKNOWN)) otaOk = false;
      }
      if (!otaOk) return;
      if (Update.write(data, len) != len) otaOk = false;
      if (final && !Update.end(true)) otaOk = false;
    });

  server.onNotFound([](AsyncWebServerRequest *r) {
    // Captive-portal style: anything unknown goes to the app.
    r->redirect("/");
  });
  server.begin();
}

static void startAP() {
  apMode = true;
  WiFi.mode(wifiSsid.length() ? WIFI_AP_STA : WIFI_AP);
  WiFi.softAP(apSsid.c_str(), apPass.c_str());
}

static void setupWifi() {
  WiFi.persistent(false);
  WiFi.setHostname("padkey");
  if (wifiSsid.length()) {
    WiFi.mode(WIFI_STA);
    WiFi.begin(wifiSsid.c_str(), wifiPass.c_str());
    uint32_t t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) {
      drawFrame(); tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 80);
      updateLed();
      delay(50);
    }
  }
  if (WiFi.status() != WL_CONNECTED) startAP();
  WiFi.setSleep(false);  // lowest latency
  MDNS.begin("padkey");
  MDNS.addService("http", "tcp", 80);
}

// ----------------------------------------------------------------- BLE ----
// Same binary protocol as the WebSocket, for iOS browsers with Web Bluetooth
// (e.g. Bluefy). Text writes are control commands: "auth <token>",
// "pairstart", "pair <pin>". Replies come back as notifications.
static const BLEUUID BLE_SVC("6b1a0001-4d2f-4c8e-a5b1-7061646b6579");
static const BLEUUID BLE_CMD("6b1a0002-4d2f-4c8e-a5b1-7061646b6579");
static const BLEUUID BLE_EVT("6b1a0003-4d2f-4c8e-a5b1-7061646b6579");
static BLEServer bleServer;
static BLECharacteristic bleCmd, bleEvt;
static std::vector<uint16_t> bleAuthed;
static portMUX_TYPE bleMux = portMUX_INITIALIZER_UNLOCKED;
static bool bleOk = false;
static volatile uint16_t bleMtu = 23;
static volatile bool bleReadvertise = false;

static bool bleIsAuthed(uint16_t h) {
  bool r = false;
  portENTER_CRITICAL(&bleMux);
  for (auto a : bleAuthed) if (a == h) r = true;
  portEXIT_CRITICAL(&bleMux);
  return r;
}

static void bleSetAuthed(uint16_t h, bool on) {
  portENTER_CRITICAL(&bleMux);
  bleAuthed.erase(std::remove(bleAuthed.begin(), bleAuthed.end(), h), bleAuthed.end());
  if (on) bleAuthed.push_back(h);
  bleCount = bleAuthed.size();
  clientCount = wsCount + bleCount;
  portEXIT_CRITICAL(&bleMux);
}

static void bleReply(uint16_t h, const String &s) {
  // Retry while the radio buffers are full (e.g. right after an image burst),
  // otherwise short control messages like "imgend" could be lost. Bounded so a
  // reply that will never fit this link's MTU can't stall the receive path.
  for (int tries = 0; tries < 80; tries++) {
    if (bleEvt.notify(h, (const uint8_t *)s.c_str(), s.length())) return;
    delay(3);
  }
}

static String bleStatusJson() {  // short: fits in one notification
  String s = "{\"usb\":"; s += usbUp ? "true" : "false";
  s += ",\"caps\":"; s += capsOn ? "true" : "false";
  s += ",\"bridge\":"; s += bridgeAlive() ? "true" : "false";
  s += ",\"led\":"; s += ledEnabled ? "true" : "false";
  s += ",\"ble\":true,\"fw\":\"" FW_VERSION "\"}";
  return s;
}

// Replies to BLE commands are NOT sent from inside the write callback: notifying
// there can swallow the write's own ATT response, and the client then drops the
// link after the 30 s ATT timeout (iOS: reason 0x13, "connects then disconnects").
// The callback queues the reply; loop() sends it once the write has completed.
struct BlePending { uint16_t h; String s; };
static std::vector<BlePending> blePending;
static portMUX_TYPE blePendMux = portMUX_INITIALIZER_UNLOCKED;
static SemaphoreHandle_t blePendLock;
static void bleReplyLater(uint16_t h, const String &s) {
  if (!blePendLock) return;
  xSemaphoreTake(blePendLock, portMAX_DELAY);
  if (blePending.size() < 16) blePending.push_back({h, s});
  xSemaphoreGive(blePendLock);
}
static void bleFlushReplies() {
  if (!blePendLock) return;
  std::vector<BlePending> todo;
  xSemaphoreTake(blePendLock, portMAX_DELAY);
  todo.swap(blePending);
  xSemaphoreGive(blePendLock);
  for (auto &r : todo) bleReply(r.h, r.s);
}

static void bleLog(const char *fmt, ...) {   // connection diagnostics over USB (only while a host listens)
  if (!bridgeAlive()) return;
  char m[64]; va_list ap; va_start(ap, fmt); vsnprintf(m, sizeof(m), fmt, ap); va_end(ap);
  cdcSend(D_LOG, (const uint8_t *)m, strlen(m));
}

static void setupBle() {
  blePendLock = xSemaphoreCreateMutex();
  if (!BLE.begin(apSsid.c_str())) return;
  BLE.setMTU(517);
  bleServer = BLE.createServer();
  if (!bleServer) return;
  bleServer.advertiseOnDisconnect(true);
  bleServer.onMtuChanged([](BLEServer &srv, const BLEConnInfo &c, uint16_t mtu) { bleMtu = mtu; bleLog("ble mtu h=%u %u", (unsigned)c.getHandle(), (unsigned)mtu); });
  bleServer.onConnect([](BLEServer &srv, const BLEConnInfo &c) {
    bleLog("ble connect h=%u mtu=%u", (unsigned)c.getHandle(), (unsigned)c.getMTU());
    bleMtu = c.getMTU();
    BLEConnParams p = {12, 24, 0, 400};  // 15-30 ms interval: snappy cursor
    srv.updateConnParams(c.getHandle(), p);
    bleReadvertise = true;               // stay discoverable (done from loop(), not in this callback)
  });
  bleServer.onDisconnect([](BLEServer &srv, const BLEConnInfo &c, uint8_t reason) {
    bleLog("ble disconnect h=%u reason=0x%02x", (unsigned)c.getHandle(), (unsigned)reason);
    bool was = bleIsAuthed(c.getHandle());
    bleSetAuthed(c.getHandle(), false);
    if (was && clientCount == 0) { uint8_t m = M_RELEASE; enqueue(&m, 1); }
  });
  BLEService svc = bleServer.createService(BLE_SVC);
  // Permissions must match properties exactly, or the library rejects the characteristic.
  bleCmd = svc.createCharacteristic(BLE_CMD, BLEProperty::Write | BLEProperty::WriteNR, BLEPermission::WriteOpen);
  bleEvt = svc.createCharacteristic(BLE_EVT, BLEProperty::Read | BLEProperty::Notify, BLEPermission::ReadOpen);
  if (!bleCmd || !bleEvt) return;
  bleCmd.onWrite([](BLECharacteristic &chr, const BLEConnInfo &c) {
    size_t n = 0;
    const uint8_t *d = chr.getValue(&n);
    if (!d || !n) return;
    uint16_t h = c.getHandle();
    if (d[0] >= 'A') {  // text command
      String cmd; cmd.concat((const char *)d, n);
      if (cmd.startsWith("auth ")) {
        bool ok = cmd.substring(5) == token;
        bleSetAuthed(h, ok);
        bleLog("ble auth h=%u %s", (unsigned)h, ok ? "ok" : "BAD");
        bleReplyLater(h, ok ? "ok " + bleStatusJson() : String("bad"));
        if (ok) { const uint8_t hello[] = {M_LABEL, 0xFF, 0x07, 'B', 'L', 'E', ' ', 'O', 'K'}; enqueue(hello, sizeof(hello)); }
      } else if (cmd == "pairstart") {
        pairStart();
        bleReplyLater(h, "pin");
      } else if (cmd.startsWith("pair ")) {
        bleReplyLater(h, pairTry(cmd.substring(5)) ? "token " + token : String("badpin"));
      }
      return;
    }
    if (!bleIsAuthed(h)) return;
    if (d[0] == M_UTEXT) { String r = handleUtext(d, n); if (r.length()) bleReplyLater(h, "utext " + r); }
    else enqueue(d, n);
  });
  if (!bleServer.start()) return;
  BLEAdvertising adv = BLE.getAdvertising();
  adv.addServiceUUID(BLE_SVC);
  bleOk = (bool)adv.start();
}

static void bleBroadcastStatus() {
  if (!bleOk) return;
  portENTER_CRITICAL(&bleMux);
  std::vector<uint16_t> ids = bleAuthed;
  portEXIT_CRITICAL(&bleMux);
  if (ids.empty()) return;
  String s = "st " + bleStatusJson();
  for (auto h : ids) bleReply(h, s);
}

// -------------------------------------------------------------- Button ----
static uint32_t btnDownAt = 0;
static bool btnLongDone = false;

static void handleButton() {
  bool down = digitalRead(PIN_BUTTON) == LOW;
  uint32_t now = millis();
  if (down && !btnDownAt) { btnDownAt = now; btnLongDone = false; }
  if (down && btnDownAt && !btnLongDone && now - btnDownAt > 5000) {
    // Long press: forget all paired iPads.
    token = randomHex(32);
    prefs.putString("token", token);
    ws.closeAll();
    portENTER_CRITICAL(&bleMux);
    std::vector<uint16_t> bids = bleAuthed;
    portEXIT_CRITICAL(&bleMux);
    for (auto h : bids) { bleSetAuthed(h, false); bleServer.disconnect(h); }
    showMsg("UNPAIRED ALL", 3000);
    btnLongDone = true;
  }
  if (!down && btnDownAt) {
    if (!btnLongDone && now - btnDownAt > 30) { infoPage = true; infoUntil = now + 15000; }
    btnDownAt = 0;
  }
}

// --------------------------------------------------------------- Setup ----
static void usbEvent(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (base != ARDUINO_USB_EVENTS) return;
  if (id == ARDUINO_USB_STARTED_EVENT || id == ARDUINO_USB_RESUME_EVENT) usbUp = true;
  if (id == ARDUINO_USB_STOPPED_EVENT || id == ARDUINO_USB_SUSPEND_EVENT) usbUp = false;
}

// Send a screenshot chunk to every paired client. BLE: split to MTU size,
// each notification starts with 0x01 so the app can tell it from text.
static bool forwardImageChunk(const uint8_t *p, size_t n) {
  portENTER_CRITICAL(&authMux);
  std::vector<uint32_t> wsIds = authed;
  portEXIT_CRITICAL(&authMux);
  for (auto id : wsIds) {
    uint8_t *buf = (uint8_t *)malloc(n + 1);
    if (!buf) return false;
    buf[0] = 0x01; memcpy(buf + 1, p, n);
    ws.binary(id, buf, n + 1);
    free(buf);
  }
  portENTER_CRITICAL(&bleMux);
  std::vector<uint16_t> ids = bleAuthed;
  portEXIT_CRITICAL(&bleMux);
  if (ids.empty() || !bleOk) return true;
  size_t room = max(20, (int)bleMtu - 3) - 1;
  uint8_t pkt[520];
  for (size_t off = 0; off < n; off += room) {
    size_t k = min(room, n - off);
    pkt[0] = 0x01; memcpy(pkt + 1, p + off, k);
    for (auto h : ids) {
      for (int tries = 0; tries < 200; tries++) {        // wait for the radio buffers to drain
        if (bleEvt.notify(h, pkt, k + 1)) break;
        delay(3);
      }
    }
  }
  return true;
}

static void broadcastText(const String &s) {
  portENTER_CRITICAL(&authMux);
  std::vector<uint32_t> wsIds = authed;
  portEXIT_CRITICAL(&authMux);
  for (auto id : wsIds) ws.text(id, s);
  portENTER_CRITICAL(&bleMux);
  std::vector<uint16_t> ids = bleAuthed;
  portEXIT_CRITICAL(&bleMux);
  for (auto h : ids) bleReply(h, s);
}

#include "pcshot.h"
static void pcshotTrigger(uint8_t how) { pcshot::trigger(how); }

// USB serial frames from the host bridge.
static void pollBridge() {
  static uint8_t buf[1100];
  static size_t have = 0, need = 0;
  static uint8_t type = 0;
  while (Serial.available()) {
    int c = Serial.read();
    if (c < 0) break;
    if (have == 0 && c != 0xA5) continue;           // resync
    buf[have++] = c;
    if (have == 4) { type = buf[1]; need = buf[2] | buf[3] << 8; if (need > 1024) { have = 0; continue; } }
    if (have >= 4 && have == 4 + need) {
      const uint8_t *p = buf + 4;
      bridgeSeen = millis();
      switch (type) {
        case H_HELLO: {
          char hb[96];
          snprintf(hb, sizeof(hb), "%s reset=%d up=%lus heap=%u min=%u blk=%u", FW_VERSION, (int)esp_reset_reason(), (unsigned long)(millis() / 1000),
                   (unsigned)ESP.getFreeHeap(), (unsigned)ESP.getMinFreeHeap(), (unsigned)ESP.getMaxAllocHeap());
          cdcSend(D_HELLO, (const uint8_t *)hb, strlen(hb));
          break;
        }
        case H_PASTE: { const uint8_t m[] = {M_TAP, 0x01, 0x19}; enqueue(m, 3);         // Ctrl+V
                        broadcastText("utext pasted"); break; }
        case H_IMG_BEGIN: { String a; a.concat((const char *)p, need); broadcastText("img " + a);
                            setLabel("IMAGE", 0x07FF); break; }
        case H_IMG_DATA: forwardImageChunk(p, need); cdcSend(D_ACK, nullptr, 0); break;
        case H_IMG_END: broadcastText("imgend"); break;
        case H_PCSHOT: { const uint8_t m[] = {M_PCSHOT}; enqueue(m, 1); break; }
      }
      have = 0;
    }
  }
}

static void kbdEvent(void *arg, esp_event_base_t base, int32_t id, void *data) {
  if (id == ARDUINO_USB_HID_KEYBOARD_LED_EVENT) {
    capsOn = ((arduino_usb_hid_keyboard_event_data_t *)data)->capslock;
    statusDirty = true;
  }
}

void setup() {
  pinMode(PIN_BUTTON, INPUT_PULLUP);
  pinMode(PIN_LED_DI, OUTPUT);
  pinMode(PIN_LED_CI, OUTPUT);
  ledShow(0, 0, 0);
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, LOW);  // backlight on

  queue = xQueueCreate(64, sizeof(Msg));

  USB.onEvent(usbEvent);
  USB.productName("PadKey Keyboard");
  USB.manufacturerName("PadKey");
  Keyboard.onEvent(ARDUINO_USB_HID_KEYBOARD_LED_EVENT, kbdEvent);
  Keyboard.begin();
  Mouse.begin();
  Consumer.begin();
  pcshot::begin();
  USB.begin();
  Serial.setRxBufferSize(4096);
  Serial.begin(115200);
  cdcLock = xSemaphoreCreateMutex();

  SPI.begin(PIN_LCD_SCLK, -1, PIN_LCD_MOSI, PIN_LCD_CS);
  tft.initR(INITR_MINI160x80_PLUGIN);
  tft.fixOffsets();
  tft.setRotation(3);
  tft.setSPISpeed(40000000);
  tft.fillScreen(0);

  loadSettings();
  setupWifi();
  setupWeb();
  setupBle();
  // (no boot banner: the USB serial line carries framed bridge data)
}

void loop() {
  Msg m;
  int accX = 0, accY = 0;
  int handled = 0;
  while (handled < 32 && xQueueReceive(queue, &m, 0) == pdTRUE) {
    handleMsg(m, accX, accY);
    handled++;
  }
  if (accX || accY) mouseMove(accX, accY);

  static uint32_t lastDraw = 0, lastLed = 0, lastStatus = 0, lastClean = 0;
  uint32_t now = millis();
  bool animating = fx != FX_NONE || now - lastLabelAt < 2600;
  if (now - lastDraw > (animating ? 33u : 80u)) {
    lastDraw = now;
    drawFrame();
    tft.drawRGBBitmap(0, 0, canvas.getBuffer(), 160, 80);
  }
  if (now - lastLed > 20) { lastLed = now; updateLed(); }
  static bool lastBridge = false;
  if (bridgeAlive() != lastBridge) { lastBridge = bridgeAlive(); statusDirty = true; }
  if (now - lastStatus > 3000 || (statusDirty && now - lastStatus > 100)) {
    statusDirty = false;
    lastStatus = now;
    String s = "st " + privateStatusJson();
    portENTER_CRITICAL(&authMux);
    std::vector<uint32_t> ids = authed;
    portEXIT_CRITICAL(&authMux);
    for (auto id : ids) ws.text(id, s);
    bleBroadcastStatus();
  }
  if (now - lastClean > 1000) { lastClean = now; ws.cleanupClients(); }

  // In AP+STA mode, drop the AP once home Wi-Fi works and nobody uses the AP.
  if (apMode && WiFi.status() == WL_CONNECTED && WiFi.softAPgetStationNum() == 0) {
    WiFi.softAPdisconnect(true);
    WiFi.mode(WIFI_STA);
    apMode = false;
  }

  pollBridge();
  bleFlushReplies();
  pcshot::poll();
  if (bleReadvertise) { bleReadvertise = false; delay(50); if (bleOk) BLE.getAdvertising().start(); }
  handleButton();
  if (rebootAt && now > rebootAt) ESP.restart();
  delay(1);
}
