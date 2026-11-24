// PC screenshots for offline computers (no network, nothing installed).
//
// The dongle also shows up as a small USB drive "PADKEY" with two files:
//   PADKEY.SH  read-only screenshot script (pcshot.sh, built into the firmware)
//   SHOT.BIN   a pre-allocated "drop box" that fills the rest of the drive
// The iPad asks for a shot -> we open the desktop's Run dialog (Alt+F2) and type
// a one-line command that runs PADKEY.SH. The script takes the screenshot and
// writes "PADKEY1 <seq> <len>" (512-byte header) + the image into SHOT.BIN in
// place. We notice the writes, read the image back from flash and stream it to
// the iPad over the same path as the tablet bridge's screenshots.
//
// The FAT12 metadata (boot sector, FATs, root dir) is generated on the fly;
// the file data lives in the "ffat" flash partition.
#pragma once
#include "USBMSC.h"
#include <esp_partition.h>

namespace pcshot {

static const uint32_t SEC = 512;
static const uint32_t META = 24;                 // sectors before the data area
static const uint32_t CLUS = 2528;               // 4 KB clusters (= the 9.9 MB ffat partition)
static const uint32_t TOTAL = META + CLUS * 8;
static const uint32_t FAT_LBA = 6, FAT_SECS = 8, ROOT_LBA = 22;
static const uint32_t SHOT_OFF = 4096;           // flash offset of SHOT.BIN (cluster 3)
static const uint32_t SHOT_SIZE = (CLUS - 1) * 4096;

static USBMSC msc;
static const esp_partition_t *part = nullptr;
static SemaphoreHandle_t lock;
static volatile bool dirty = false, erasing = false;
static volatile uint32_t lastWrite = 0;
static bool active = false, needErase = false;
static void dbg(const char *m) { if (bridgeAlive()) cdcSend(D_LOG, (const uint8_t *)m, strlen(m)); }
static uint32_t sendOff = 0, sendLen = 0;
static unsigned long long lastSeq = 0;

// RAM is tight (Wi-Fi + BLE + web server): with 16 KB of buffers here the BLE
// stack starved and iOS dropped the link every 30 s. So nothing big is kept:
// the FAT metadata is generated on the fly, and flash blocks are tracked with
// 2 bits each instead of read-modify-write through a 4 KB buffer.
enum : uint8_t { B_UNKNOWN = 0, B_ERASED = 1, B_WRITING = 2, B_DIRTY = 3 };
static uint8_t bstate[(CLUS + 3) / 4];                     // 2 bits per 4 KB flash block
static uint8_t bget(uint32_t b) { return (bstate[b >> 2] >> ((b & 3) * 2)) & 3; }
static void bset(uint32_t b, uint8_t v) { uint8_t sh = (b & 3) * 2; bstate[b >> 2] = (bstate[b >> 2] & ~(3 << sh)) | (v << sh); }

static void w16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }

static uint16_t fatEntry(uint32_t n) {
  if (n == 0) return 0xFF8;
  if (n == 1 || n == 2 || n == CLUS + 1) return 0xFFF;     // 2 = PADKEY.SH, CLUS+1 = end of SHOT.BIN
  if (n < CLUS + 1) return n + 1;                          // SHOT.BIN: clusters 3..CLUS+1 in a chain
  return 0;
}

static void dirent(uint8_t *e, const char *name11, uint8_t attr, uint16_t clus, uint32_t size) {
  const uint16_t date = ((2026 - 1980) << 9) | (11 << 5) | 24, time = 12 << 11;
  memcpy(e, name11, 11);
  e[11] = attr;
  w16(e + 14, time); w16(e + 16, date); w16(e + 18, date);
  w16(e + 22, time); w16(e + 24, date);
  w16(e + 26, clus);
  w32(e + 28, size);
}

// One 512-byte sector of the metadata area (boot sector, 2 FATs, root dir).
static void metaSector(uint32_t sec, uint8_t *b) {
  memset(b, 0, SEC);
  if (sec == 0) {
    b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
    memcpy(b + 3, "MSWIN4.1", 8);
    w16(b + 11, SEC); b[13] = 8; w16(b + 14, FAT_LBA); b[16] = 2; w16(b + 17, 32);
    w16(b + 19, TOTAL); b[21] = 0xF8; w16(b + 22, FAT_SECS); w16(b + 24, 32); w16(b + 26, 64);
    b[36] = 0x80; b[38] = 0x29; w32(b + 39, 0x50414459);
    memcpy(b + 43, "PADKEY     ", 11); memcpy(b + 54, "FAT12   ", 8);
    b[510] = 0x55; b[511] = 0xAA;
  } else if (sec >= FAT_LBA && sec < FAT_LBA + 2 * FAT_SECS) {
    uint32_t base = ((sec - FAT_LBA) % FAT_SECS) * SEC;    // byte offset inside this FAT copy
    uint32_t n0 = base * 2 / 3, n1 = (base + SEC) * 2 / 3 + 1;
    for (uint32_t n = n0 ? n0 - 1 : 0; n <= n1; n++) {     // every FAT12 entry touching this sector
      uint16_t v = fatEntry(n);
      int32_t o = (int32_t)(n * 3 / 2) - (int32_t)base;
      uint8_t lo, hi; bool odd = n & 1;
      if (odd) { lo = (v << 4) & 0xF0; hi = v >> 4; } else { lo = v & 0xFF; hi = (v >> 8) & 0x0F; }
      if (o >= 0 && o < (int32_t)SEC) b[o] |= lo;
      if (o + 1 >= 0 && o + 1 < (int32_t)SEC) b[o + 1] |= hi;
    }
  } else if (sec == ROOT_LBA) {
    dirent(b, "PADKEY     ", 0x08, 0, 0);
    dirent(b + 32, "PADKEY  SH ", 0x01, 2, pcshot_sh_len);
    dirent(b + 64, "SHOT    BIN", 0x00, 3, SHOT_SIZE);
  }
}

// Program [off, off+len) of the partition. Image data is disposable, so a block
// that may hold old bytes is simply erased the first time this write burst touches it.
static void flashWrite(uint32_t off, const uint8_t *src, uint32_t len) {
  while (len) {
    uint32_t blk = off / 4096, n = min(len, 4096 - (off % 4096));
    uint8_t st = bget(blk);
    if (st != B_ERASED && st != B_WRITING) esp_partition_erase_range(part, blk * 4096, 4096);
    bset(blk, B_WRITING);
    esp_partition_write(part, off, src, n);
    off += n; src += n; len -= n;
  }
}

static void endWriteBurst() {                              // call with lock held
  for (uint32_t b = 0; b < CLUS; b++) if (bget(b) == B_WRITING) bset(b, B_DIRTY);
}

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t size) {
  uint8_t *dst = (uint8_t *)buffer;
  uint32_t pos = lba * SEC + offset, end = pos + size;
  uint8_t sec[SEC];
  while (pos < end) {
    uint32_t n;
    if (pos < META * SEC) {
      uint32_t s = pos / SEC, o = pos % SEC;
      n = min(end - pos, SEC - o);
      metaSector(s, sec);
      memcpy(dst, sec + o, n);
    } else {
      uint32_t rel = pos - META * SEC;
      if (rel < 4096) {                                    // PADKEY.SH
        n = min(end - pos, 4096 - rel);
        memset(dst, 0, n);
        if (rel < pcshot_sh_len) memcpy(dst, pcshot_sh + rel, min(n, (uint32_t)pcshot_sh_len - rel));
      } else {
        n = end - pos;
        xSemaphoreTake(lock, portMAX_DELAY);
        esp_partition_read(part, rel, dst, n);
        xSemaphoreGive(lock);
      }
    }
    pos += n; dst += n;
  }
  return size;
}

static int32_t onWrite(uint32_t lba, uint32_t offset, uint8_t *buffer, uint32_t size) {
  uint8_t *src = buffer;
  uint32_t pos = lba * SEC + offset, end = pos + size;
  while (pos < end) {
    uint32_t n;
    if (pos < META * SEC) {                                // FAT / directory updates (times, dirty flag): ignored
      n = min(end, META * SEC) - pos;
    } else {
      uint32_t rel = pos - META * SEC;
      if (rel < 4096) n = min(end - pos, 4096 - rel);      // PADKEY.SH is read-only
      else {
        n = end - pos;
        xSemaphoreTake(lock, portMAX_DELAY);
        dirty = true;
        flashWrite(rel, src, n);
        lastWrite = millis();
        xSemaphoreGive(lock);
      }
    }
    pos += n; src += n;
  }
  return size;
}

static bool onStartStop(uint8_t power, bool start, bool eject) { return true; }

// Erase the blocks the last image used, so the next one writes fast.
static void eraseTask(void *arg) {
  for (uint32_t b = SHOT_OFF / 4096; b < CLUS; b++) {
    xSemaphoreTake(lock, portMAX_DELAY);
    bool stop = dirty;                                     // a new shot is being written: leave it alone
    if (!stop && bget(b) == B_DIRTY) { esp_partition_erase_range(part, b * 4096, 4096); bset(b, B_ERASED); }
    xSemaphoreGive(lock);
    if (stop) break;
    if ((b & 31) == 0) vTaskDelay(1);
  }
  dbg("pcshot erased");
  erasing = false;
  vTaskDelete(nullptr);
}

// Call before USB.begin().
#ifndef PADKEY_MSC
#define PADKEY_MSC 1
#endif
static void begin() {
  lock = xSemaphoreCreateMutex();
  if (!PADKEY_MSC) return;
  part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, nullptr);
  if (!part || part->size < CLUS * 4096) { part = nullptr; return; }
  msc.vendorID("PadKey");
  msc.productID("Screenshot");
  msc.productRevision("1.0");
  msc.onRead(onRead);
  msc.onWrite(onWrite);
  msc.onStartStop(onStartStop);
  msc.mediaPresent(true);
  msc.isWritable(true);
  msc.begin(TOTAL, SEC);
}

static void typeAscii(const char *s) {
  for (; *s; s++) {
    uint8_t c = *s;
    if (capsOn && isalpha(c)) c ^= 0x20;
    Keyboard.write(c);
    delay(6);
  }
  Keyboard.sendReport(&report);
}

static void tapKey(uint8_t mod, uint8_t key) {
  if (mod) keyDown(mod);
  keyDown(key); delay(40); keyUp(key);
  if (mod) keyUp(mod);
}

// Alt+F2 opens the desktop's Run dialog (Xfce, GNOME, KDE, MATE, Cinnamon).
// The Run dialog auto-completes from its history (the completed tail is
// selected), so Delete is pressed before Enter. Short lines, 1 s pauses.
// Also avoids " | # ~ @ \ which move around on UK and other non-US layouts.
static void runLine(const char *cmd) {
  tapKey(0xE2, 0x3B);                                      // Alt+F2
  delay(1000);
  typeAscii(cmd);
  delay(1000);
  tapKey(0, 0x4C);                                         // Delete: drop any history auto-completion
  delay(150);                                              // (the completed tail is selected text)
  tapKey(0, 0x28);                                         // Enter
}

static void trigger(uint8_t how) {
  if (!part) { broadcastText("pcshot nodrive"); return; }
  releaseAll();
  setLabel("PC SHOT", 0x07FF);
  runLine("udisksctl mount -b /dev/disk/by-label/PADKEY");   // fails harmlessly if already mounted
  delay(1500);
  runLine("sh -c 'sh /*/*/PADKEY/*.SH;sh /run/*/*/PADKEY/*.SH'");
  broadcastText("pcshot typed");
}

static void imageSize(uint32_t &w, uint32_t &h) {
  uint8_t p[24];
  w = h = 0;
  esp_partition_read(part, SHOT_OFF + 512, p, sizeof(p));
  if (p[0] == 0x89 && p[1] == 'P') {                       // PNG IHDR
    w = p[16] << 24 | p[17] << 16 | p[18] << 8 | p[19];
    h = p[20] << 24 | p[21] << 16 | p[22] << 8 | p[23];
  }
}

// Called from loop(): notice a finished shot and stream it, one chunk per call.
static void poll() {
  if (!part) return;
  if (!active && dirty && !erasing && millis() - lastWrite > 600) {
    char hdr[SEC + 1];
    xSemaphoreTake(lock, portMAX_DELAY);
    dirty = false;
    endWriteBurst();
    esp_partition_read(part, SHOT_OFF, hdr, SEC);
    xSemaphoreGive(lock);
    hdr[SEC] = 0;
    // (parsed by hand: the C library's scanf has no %llu here)
    unsigned long long seq = 0; unsigned long n = 0;
    bool ok = !strncmp(hdr, "PADKEY1 ", 8);
    if (ok) { char *e; seq = strtoull(hdr + 8, &e, 10); ok = *e == ' '; if (ok) n = strtoul(e + 1, nullptr, 10); }
    if (ok && n > 0 && n <= SHOT_SIZE - SEC && seq != lastSeq) {
      lastSeq = seq; active = true; sendOff = 0; sendLen = n;
      uint32_t w, h; imageSize(w, h);
      char m[48]; snprintf(m, sizeof(m), "img %lu %lu %lu", n, (unsigned long)w, (unsigned long)h);
      broadcastText(m);
      dbg(m);
      { char sm[48]; snprintf(sm, sizeof(sm), "pcshot start mtu=%u chunk=%u", (unsigned)bleMtu, (unsigned)bleChunk); dbg(sm); }
      setLabel("PC IMAGE", 0x07FF);
    } else {
      needErase = needErase || ok;                         // an old shot: just clean up
    }
  }
  if (!active && needErase && !erasing && !dirty && millis() - lastWrite > 1500) {
    needErase = false; erasing = true;
    xTaskCreate(eraseTask, "pcerase", 2048, nullptr, 1, nullptr);
  }
  if (active) {
    uint8_t buf[1024];
    uint32_t k = min((uint32_t)sizeof(buf), sendLen - sendOff);
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_partition_read(part, SHOT_OFF + SEC + sendOff, buf, k);
    xSemaphoreGive(lock);
    forwardImageChunk(buf, k);
    sendOff += k;
    if (sendOff >= sendLen) {
      broadcastText("imgend");
      char dm[80]; snprintf(dm, sizeof(dm), "pcshot sent len=%lu mtu=%u chunk=%u pkts=%lu giveup=%lu",
        (unsigned long)sendLen, (unsigned)bleMtu, (unsigned)bleChunk, (unsigned long)fwdPkts, (unsigned long)fwdGiveup);
      dbg(dm);
      active = false;
      needErase = true;
    }
  }
}

}  // namespace pcshot
