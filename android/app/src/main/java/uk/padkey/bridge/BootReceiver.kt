package uk.padkey.bridge

import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent

/** Starts the bridge after the tablet boots (if the user left it switched on). */
class BootReceiver : BroadcastReceiver() {
    override fun onReceive(c: Context, i: Intent) {
        if (i.action == Intent.ACTION_BOOT_COMPLETED &&
            c.getSharedPreferences("padkey", Context.MODE_PRIVATE).getBoolean("enabled", true)) {
            BridgeService.start(c)
        }
    }
}
