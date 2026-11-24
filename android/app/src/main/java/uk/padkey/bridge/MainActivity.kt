package uk.padkey.bridge

import android.Manifest
import android.app.Activity
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.graphics.Color
import android.graphics.Typeface
import android.net.Uri
import android.os.Build
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.provider.Settings
import android.view.Gravity
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.Switch
import android.widget.TextView

/** Small status / settings screen for the bridge. */
class MainActivity : Activity() {
    private val ui = Handler(Looper.getMainLooper())
    private lateinit var status: TextView
    private lateinit var counts: TextView
    private lateinit var logView: TextView
    private lateinit var permRow: LinearLayout
    private val prefs by lazy { getSharedPreferences("padkey", Context.MODE_PRIVATE) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        val root = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL; setPadding(pad, pad, pad, pad); setBackgroundColor(Color.rgb(13, 6, 32)) }
        fun text(s: String, size: Float, color: Int = Color.WHITE, bold: Boolean = false) = TextView(this).apply {
            text = s; textSize = size; setTextColor(color); if (bold) typeface = Typeface.DEFAULT_BOLD; setPadding(0, pad / 3, 0, pad / 3)
        }
        root.addView(text("PADKEY 橋接", 26f, Color.rgb(0, 240, 255), true))
        root.addView(text("讓 iPad 的「整串輸入」中文貼到這台平板，並把截圖傳到 iPad 描圖。不會記錄你打的字。", 14f, Color.rgb(170, 160, 210)))

        val sw = Switch(this).apply {
            text = "啟用（開機時也自動啟動）"; textSize = 18f; setTextColor(Color.WHITE)
            isChecked = prefs.getBoolean("enabled", true)
            setOnCheckedChangeListener { _, on ->
                prefs.edit().putBoolean("enabled", on).apply()
                if (on) BridgeService.start(this@MainActivity) else stopService(Intent(this@MainActivity, BridgeService::class.java))
            }
        }
        root.addView(sw)
        status = text("", 20f, Color.rgb(57, 255, 136), true); root.addView(status)
        counts = text("", 14f, Color.rgb(170, 160, 210)); root.addView(counts)

        permRow = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        root.addView(permRow)

        root.addView(text("紀錄", 16f, Color.rgb(255, 43, 214), true))
        logView = text("", 13f, Color.rgb(200, 200, 220)).apply { typeface = Typeface.MONOSPACE }
        root.addView(ScrollView(this).apply { addView(logView) }, LinearLayout.LayoutParams(-1, 0, 1f))
        setContentView(root)

        if (prefs.getBoolean("enabled", true)) BridgeService.start(this)
    }

    override fun onResume() { super.onResume(); refresh() }
    override fun onPause() { super.onPause(); ui.removeCallbacksAndMessages(null) }

    private fun refresh() {
        status.text = "● " + BridgeService.state
        counts.text = "本次：貼上 ${BridgeService.pastes} 次・截圖 ${BridgeService.images} 張"
        logView.text = synchronized(BridgeService.events) { BridgeService.events.joinToString("\n") }
        buildPermissionButtons()
        ui.postDelayed({ refresh() }, 1000)
    }

    private fun buildPermissionButtons() {
        permRow.removeAllViews()
        fun btn(label: String, go: () -> Unit) = permRow.addView(Button(this).apply { text = label; setOnClickListener { go() } })
        val photo = if (Build.VERSION.SDK_INT >= 33) Manifest.permission.READ_MEDIA_IMAGES else Manifest.permission.READ_EXTERNAL_STORAGE
        if (checkSelfPermission(photo) != PackageManager.PERMISSION_GRANTED)
            btn("允許讀取相片（截圖傳到 iPad 需要，請選「允許全部」）") { requestPermissions(arrayOf(photo), 1) }
        if (Build.VERSION.SDK_INT >= 33 && checkSelfPermission(Manifest.permission.POST_NOTIFICATIONS) != PackageManager.PERMISSION_GRANTED)
            btn("允許通知（讓橋接在背景穩定執行）") { requestPermissions(arrayOf(Manifest.permission.POST_NOTIFICATIONS), 2) }
        val pm = getSystemService(PowerManager::class.java)
        if (!pm.isIgnoringBatteryOptimizations(packageName))
            btn("不要讓系統省電關掉橋接") {
                startActivity(Intent(Settings.ACTION_REQUEST_IGNORE_BATTERY_OPTIMIZATIONS, Uri.parse("package:$packageName")))
            }
    }
}
