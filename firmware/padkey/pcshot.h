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
// The FAT12 metadata (boot sector, FATs, root dir) lives in RAM and is rebuilt
// on every boot; the file data lives in the "ffat" flash partition.
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
static uint8_t meta[META * SEC];
static uint8_t tmp[4096];
static SemaphoreHandle_t lock;
static volatile bool dirty = false, erasing = false;
static volatile uint32_t lastWrite = 0;
static bool active = false, needErase = false;
static void dbg(const char *m) { if (bridgeAlive()) cdcSend(D_LOG, (const uint8_t *)m, strlen(m)); }
static uint32_t sendOff = 0, sendLen = 0;
static unsigned long long lastSeq = 0;

static void w16(uint8_t *p, uint16_t v) { p[0] = v; p[1] = v >> 8; }
static void w32(uint8_t *p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = v >> (8 * i); }

static void fatSet(uint8_t *fat, uint32_t n, uint16_t v) {
  uint32_t o = n * 3 / 2;
  if (n & 1) { fat[o] = (fat[o] & 0x0F) | (v << 4); fat[o + 1] = v >> 4; }
  else { fat[o] = v; fat[o + 1] = (fat[o + 1] & 0xF0) | ((v >> 8) & 0x0F); }
}

static void dirent(uint8_t *e, const char *name11, uint8_t attr, uint16_t clus, uint32_t size) {
  memcpy(e, name11, 11);
  e[11] = attr;
  w16(e + 22, 12 << 11);                                   // 12:00
  w16(e + 24, ((2026 - 1980) << 9) | (11 << 5) | 24);      // 2026-11-24
  w16(e + 14, 12 << 11); w16(e + 16, ((2026 - 1980) << 9) | (11 << 5) | 24); w16(e + 18, ((2026 - 1980) << 9) | (11 << 5) | 24);
  w16(e + 26, clus);
  w32(e + 28, size);
}

static void buildMeta() {
  memset(meta, 0, sizeof(meta));
  uint8_t *b = meta;
  b[0] = 0xEB; b[1] = 0x3C; b[2] = 0x90;
  memcpy(b + 3, "MSWIN4.1", 8);
  w16(b + 11, SEC); b[13] = 8; w16(b + 14, FAT_LBA); b[16] = 2; w16(b + 17, 32);
  w16(b + 19, TOTAL); b[21] = 0xF8; w16(b + 22, FAT_SECS); w16(b + 24, 32); w16(b + 26, 64);
  b[36] = 0x80; b[38] = 0x29; w32(b + 39, 0x50414459);
  memcpy(b + 43, "PADKEY     ", 11); memcpy(b + 54, "FAT12   ", 8);
  b[510] = 0x55; b[511] = 0xAA;

  uint8_t *fat = meta + FAT_LBA * SEC;
  fatSet(fat, 0, 0xFF8); fatSet(fat, 1, 0xFFF);
  fatSet(fat, 2, 0xFFF);                                   // PADKEY.SH: one cluster
  for (uint32_t c = 3; c < CLUS + 1; c++) fatSet(fat, c, c + 1);
  fatSet(fat, CLUS + 1, 0xFFF);                            // SHOT.BIN: clusters 3..CLUS+1
  memcpy(meta + (FAT_LBA + FAT_SECS) * SEC, fat, FAT_SECS * SEC);

  uint8_t *root = meta + ROOT_LBA * SEC;
  dirent(root, "PADKEY     ", 0x08, 0, 0);
  dirent(root + 32, "PADKEY  SH ", 0x01, 2, pcshot_sh_len);
  dirent(root + 64, "SHOT    BIN", 0x00, 3, SHOT_SIZE);
}

static bool blank(const uint8_t *p, size_t n) {
  for (size_t i = 0; i < n; i++) if (p[i] != 0xFF) return false;
  return true;
}

// Write [off, off+len) of the partition, erasing (read-modify-write) only when needed.
static void flashWrite(uint32_t off, const uint8_t *src, uint32_t len) {
  while (len) {
    uint32_t blk = off & ~4095u, a = off - blk, n = min(len, 4096 - a);
    esp_partition_read(part, off, tmp, n);
    if (blank(tmp, n)) {
      esp_partition_write(part, off, src, n);
    } else if (n == 4096) {
      esp_partition_erase_range(part, blk, 4096);
      esp_partition_write(part, blk, src, 4096);
    } else {
      esp_partition_read(part, blk, tmp, 4096);
      memcpy(tmp + a, src, n);
      esp_partition_erase_range(part, blk, 4096);
      esp_partition_write(part, blk, tmp, 4096);
    }
    off += n; src += n; len -= n;
  }
}

static int32_t onRead(uint32_t lba, uint32_t offset, void *buffer, uint32_t size) {
  uint8_t *dst = (uint8_t *)buffer;
  uint32_t pos = lba * SEC + offset, end = pos + size;
  while (pos < end) {
    uint32_t n;
    if (pos < META * SEC) {
      n = min(end, META * SEC) - pos;
      memcpy(dst, meta + pos, n);
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
    if (pos < META * SEC) {                                // FAT / directory updates (e.g. times)
      n = min(end, META * SEC) - pos;
      memcpy(meta + pos, src, n);
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

// Pre-erase the part of SHOT.BIN the last image used, so the next one writes fast.
static void eraseTask(void *arg) {
  uint32_t end = SHOT_OFF + (((uint32_t)(uintptr_t)arg + 4095) & ~4095u);
  int blankRun = 0;
  for (uint32_t off = SHOT_OFF; off < end && blankRun < 4; ) {
    uint32_t step = (off % 65536 == 0 && end - off >= 65536) ? 65536 : 4096;
    xSemaphoreTake(lock, portMAX_DELAY);
    bool stop = dirty;                                     // a new shot is being written: leave it alone
    bool used = false;
    if (!stop) {                                           // skip blocks that are already blank
      for (uint32_t o = 0; o < step && !used; o += sizeof(tmp)) { esp_partition_read(part, off + o, tmp, sizeof(tmp)); used = !blank(tmp, sizeof(tmp)); }
      if (used) esp_partition_erase_range(part, off, step);
    }
    xSemaphoreGive(lock);
    if (stop) break;
    blankRun = used ? 0 : (step == 65536 ? blankRun + 1 : blankRun);   // images are contiguous: stop after 256 KB of blank
    off += step;
    vTaskDelay(1);
  }
  dbg("pcshot erased");
  erasing = false;
  vTaskDelete(nullptr);
}

// Call before USB.begin().
static void begin() {
  lock = xSemaphoreCreateMutex();
  part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, nullptr);
  if (!part || part->size < CLUS * 4096) { part = nullptr; return; }
  buildMeta();
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
// Learned on Xfce: lines longer than ~80 chars get lost, and Enter pressed
// right after typing is sometimes ignored, so: two short lines, 1 s pauses.
// Also avoids " | # ~ @ \ which move around on UK and other non-US layouts.
static void runLine(const char *cmd) {
  tapKey(0xE2, 0x3B);                                      // Alt+F2
  delay(1000);
  typeAscii(cmd);
  delay(1000);
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
      setLabel("PC IMAGE", 0x07FF);
    } else {
      needErase = needErase || ok;                         // an old shot: just clean up
    }
  }
  if (!active && needErase && !erasing && !dirty && millis() - lastWrite > 1500) {
    needErase = false; erasing = true;
    xTaskCreate(eraseTask, "pcerase", 3072, (void *)(uintptr_t)SHOT_SIZE, 1, nullptr);
  }
  if (active) {
    uint8_t buf[1024];
    uint32_t k = min((uint32_t)sizeof(buf), sendLen - sendOff);
    xSemaphoreTake(lock, portMAX_DELAY);
    esp_partition_read(part, SHOT_OFF + SEC + sendOff, buf, k);
    xSemaphoreGive(lock);
    bool ok = forwardImageChunk(buf, k);
    sendOff += k;
    if (!ok) {                                            // client stalled: give up this image cleanly
      broadcastText("imgend");
      dbg("pcshot aborted");
      active = false; needErase = true;
      return;
    }
    if (sendOff >= sendLen) {
      broadcastText("imgend");
      char dm[64]; snprintf(dm, sizeof(dm), "pcshot sent len=%lu mtu=%u pkts=%lu giveup=%lu",
        (unsigned long)sendLen, (unsigned)bleMtu, (unsigned long)fwdPkts, (unsigned long)fwdGiveup);
      dbg(dm);
      active = false;
      needErase = true;
    }
  }
}

}  // namespace pcshot
