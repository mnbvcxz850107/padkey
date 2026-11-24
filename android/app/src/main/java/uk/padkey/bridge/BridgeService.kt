package uk.padkey.bridge

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.BroadcastReceiver
import android.content.ClipData
import android.content.ClipboardManager
import android.content.ContentUris
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.pm.ServiceInfo
import android.graphics.Bitmap
import android.graphics.BitmapFactory
import android.hardware.usb.UsbManager
import android.os.Build
import android.os.Handler
import android.os.IBinder
import android.os.Looper
import android.provider.MediaStore
import java.io.ByteArrayOutputStream
import java.text.SimpleDateFormat
import java.util.Date
import java.util.Locale
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit

/** Foreground service: keeps the USB link to the T-Dongle, pastes text from the
 *  iPad (clipboard + Ctrl+V) and streams new screenshots to the iPad.
 *  Never logs or stores the pasted text itself. */
class BridgeService : Service() {

    private val main = Handler(Looper.getMainLooper())
    @Volatile private var running = false
    @Volatile private var paused = false
    private var worker: Thread? = null

    private val control = object : BroadcastReceiver() {
        override fun onReceive(c: Context, i: Intent) {
            when (i.action) {
                ACTION_PAUSE -> { paused = true; log("已暫停（燒錄韌體中）") }
                ACTION_RESUME -> { paused = false; log("繼續執行") }
            }
        }
    }

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CH, "PadKey 橋接", NotificationManager.IMPORTANCE_LOW))
        val f = IntentFilter().apply { addAction(ACTION_PAUSE); addAction(ACTION_RESUME) }
        if (Build.VERSION.SDK_INT >= 33) registerReceiver(control, f, Context.RECEIVER_EXPORTED)
        else @Suppress("UnspecifiedRegisterReceiverFlag") registerReceiver(control, f)
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        val n = Notification.Builder(this, CH)
            .setContentTitle("PadKey 橋接執行中")
            .setContentText("iPad 中文送字、截圖描圖")
            .setSmallIcon(android.R.drawable.stat_sys_data_bluetooth)
            .setContentIntent(open)
            .setOngoing(true)
            .build()
        if (Build.VERSION.SDK_INT >= 29) startForeground(1, n, ServiceInfo.FOREGROUND_SERVICE_TYPE_CONNECTED_DEVICE)
        else startForeground(1, n)
        if (worker?.isAlive != true) {
            running = true
            worker = Thread({ loop() }, "padkey-bridge").also { it.start() }
        }
        return START_STICKY
    }

    override fun onDestroy() {
        running = false
        worker?.interrupt()
        try { unregisterReceiver(control) } catch (_: Exception) {}
        state = "已停止"
        super.onDestroy()
    }

    // ------------------------------------------------------------------ main loop
    private fun loop() {
        val usb = getSystemService(UsbManager::class.java)
        var shotsSince = System.currentTimeMillis() / 1000
        while (running) {
            if (paused) { state = "暫停中（燒錄韌體）"; sleep(1000); continue }
            val dev = usb.deviceList.values.firstOrNull { UsbLink.isPadKey(it) }
            if (dev == null) { state = "等待 T-Dongle 插上"; sleep(2000); continue }
            if (!usb.hasPermission(dev)) {
                state = "等待 USB 權限（請在畫面上按允許）"
                val pi = PendingIntent.getBroadcast(this, 0, Intent(ACTION_USB_PERM).setPackage(packageName), PendingIntent.FLAG_IMMUTABLE)
                usb.requestPermission(dev, pi)
                sleep(5000); continue
            }
            val link = UsbLink(usb, dev)
            try {
                link.open()
                state = "已連線 T-Dongle"; log("連上 T-Dongle")
                var lastHello = 0L; var lastScan = 0L; var greeted = false
                while (running && !paused) {
                    val now = System.currentTimeMillis()
                    if (now - lastHello > 2000) { link.send(UsbLink.H_HELLO); lastHello = now }
                    val texts = ArrayList<ByteArray>()
                    for ((t, p) in link.poll(100)) {
                        if (t == UsbLink.D_TEXT) texts.add(p)
                        else if (t == UsbLink.D_HELLO && !greeted) { greeted = true; log("T-Dongle " + String(p)) }
                    }
                    if (texts.isNotEmpty()) paste(link, texts)
                    if (now - lastScan > 1000) {
                        lastScan = now
                        val shot = newestScreenshot(shotsSince)
                        if (shot != null) { shotsSince = shot.second; sendImage(link, shot.first) }
                    }
                }
            } catch (e: Exception) {
                log("連線中斷：" + (e.message ?: e.javaClass.simpleName))
            } finally {
                link.close()
            }
            sleep(2000)
        }
    }

    // ------------------------------------------------------------------ text -> clipboard -> Ctrl+V
    private fun paste(link: UsbLink, parts: List<ByteArray>) {
        val total = parts.sumOf { it.size }
        val all = ByteArray(total); var o = 0
        for (p in parts) { System.arraycopy(p, 0, all, o, p.size); o += p.size }
        val text = String(all, Charsets.UTF_8)
        val done = CountDownLatch(1)
        main.post {
            try {
                val cm = getSystemService(ClipboardManager::class.java)
                cm.setPrimaryClip(ClipData.newPlainText("PadKey", text))
            } catch (_: Exception) {}
            done.countDown()
        }
        done.await(2, TimeUnit.SECONDS)
        sleep(120)
        link.send(UsbLink.H_PASTE)
        pastes++
        log("貼上 ${text.length} 字")          // the text itself is never logged
    }

    // ------------------------------------------------------------------ screenshots -> iPad
    private fun newestScreenshot(since: Long): Pair<android.net.Uri, Long>? {
        val proj = arrayOf(MediaStore.Images.Media._ID, MediaStore.Images.Media.DISPLAY_NAME,
            MediaStore.Images.Media.DATE_ADDED, MediaStore.Images.Media.RELATIVE_PATH, MediaStore.Images.Media.IS_PENDING)
        val sel = "${MediaStore.Images.Media.RELATIVE_PATH} LIKE ? AND ${MediaStore.Images.Media.DATE_ADDED} > ?"
        try {
            contentResolver.query(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, proj, sel,
                arrayOf("%Screenshots%", since.toString()), "${MediaStore.Images.Media.DATE_ADDED} DESC")?.use { c ->
                while (c.moveToNext()) {
                    val name = c.getString(1) ?: ""
                    if (name.startsWith("thumbnail_") || name.startsWith(".") || c.getInt(4) == 1) continue
                    return Pair(ContentUris.withAppendedId(MediaStore.Images.Media.EXTERNAL_CONTENT_URI, c.getLong(0)), c.getLong(2))
                }
            }
        } catch (e: SecurityException) {
            state = "需要「相片」權限才能傳截圖"
        }
        return null
    }

    private fun sendImage(link: UsbLink, uri: android.net.Uri) {
        val t0 = System.currentTimeMillis()
        var bmp: Bitmap? = null
        for (i in 0 until 10) {                    // the file may still be being written
            try {
                val o = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, o) }
                var s = 1; while (maxOf(o.outWidth, o.outHeight) / (s * 2) >= MAX_SIDE) s *= 2
                bmp = contentResolver.openInputStream(uri)?.use { BitmapFactory.decodeStream(it, null, BitmapFactory.Options().apply { inSampleSize = s }) }
                if (bmp != null) break
            } catch (_: Exception) {}
            sleep(300)
        }
        var b = bmp ?: run { log("截圖讀取失敗"); return }
        val k = MAX_SIDE.toFloat() / maxOf(b.width, b.height)
        if (k < 1f) b = Bitmap.createScaledBitmap(b, (b.width * k).toInt(), (b.height * k).toInt(), true)
        val jpg = ByteArrayOutputStream().also { b.compress(Bitmap.CompressFormat.JPEG, 72, it) }.toByteArray()
        link.send(UsbLink.H_IMG_BEGIN, "${jpg.size} ${b.width} ${b.height}".toByteArray())
        var off = 0
        while (off < jpg.size) {
            val n = minOf(1024, jpg.size - off)
            link.send(UsbLink.H_IMG_DATA, jpg.copyOfRange(off, off + n))
            off += n
            val deadline = System.currentTimeMillis() + 10000
            var acked = false
            while (!acked && System.currentTimeMillis() < deadline) {
                for ((t, p) in link.poll(50)) {
                    if (t == UsbLink.D_ACK) acked = true
                    else if (t == UsbLink.D_TEXT) paste(link, listOf(p))
                }
            }
            if (!acked) { log("圖片傳送中斷"); return }
        }
        link.send(UsbLink.H_IMG_END)
        images++
        log("截圖 ${b.width}×${b.height} 傳到 iPad，${(System.currentTimeMillis() - t0) / 100 / 10.0} 秒")
    }

    private fun sleep(ms: Long) { try { Thread.sleep(ms) } catch (_: InterruptedException) {} }

    companion object {
        const val CH = "bridge"
        const val MAX_SIDE = 1400
        const val ACTION_PAUSE = "uk.padkey.bridge.PAUSE"
        const val ACTION_RESUME = "uk.padkey.bridge.RESUME"
        const val ACTION_USB_PERM = "uk.padkey.bridge.USB_PERMISSION"

        @Volatile var state = "未啟動"
        @Volatile var pastes = 0
        @Volatile var images = 0
        val events = ArrayDeque<String>()

        fun log(s: String) {
            val line = SimpleDateFormat("HH:mm:ss", Locale.US).format(Date()) + "  " + s
            synchronized(events) { events.addFirst(line); while (events.size > 40) events.removeLast() }
        }

        fun start(c: Context) {
            val i = Intent(c, BridgeService::class.java)
            if (Build.VERSION.SDK_INT >= 26) c.startForegroundService(i) else c.startService(i)
        }
    }
}
