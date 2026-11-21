# PadKey

把 iPad / iPhone 變成觸控板 + 鍵盤（含 Touch Bar、注音、炫砲按鍵），
透過 LilyGO T-Dongle-S3（ESP32-S3）以 USB 鍵盤滑鼠的身分控制另一台裝置。

- `firmware/padkey/` — ESP32-S3 韌體（USB HID、Wi‑Fi 熱點 + WebSocket、BLE）
- `web/index.html` — 網頁 App 原始碼
- `docs/` — GitHub Pages 版本（給 Bluefy 透過藍牙連線用）
- `tools/` — 在 Android（Termux）上編譯與燒錄的工具
