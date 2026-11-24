package uk.padkey.bridge

import android.hardware.usb.UsbConstants
import android.hardware.usb.UsbDevice
import android.hardware.usb.UsbDeviceConnection
import android.hardware.usb.UsbEndpoint
import android.hardware.usb.UsbManager
import java.io.IOException

/** USB serial (CDC-ACM) link to the T-Dongle, framed like the Termux bridge:
 *  0xA5, type, len_lo, len_hi, payload. */
class UsbLink(private val usb: UsbManager, val device: UsbDevice) {
    private var conn: UsbDeviceConnection? = null
    private lateinit var epIn: UsbEndpoint
    private lateinit var epOut: UsbEndpoint
    private val rx = ByteArray(16384)
    private val buf = java.io.ByteArrayOutputStream()

    fun open() {
        val c = usb.openDevice(device) ?: throw IOException("無法開啟 USB 裝置")
        var comm = -1
        var gotData = false
        for (i in 0 until device.interfaceCount) {
            val itf = device.getInterface(i)
            if (itf.interfaceClass == UsbConstants.USB_CLASS_COMM && comm < 0) {
                c.claimInterface(itf, true); comm = itf.id
            }
            if (itf.interfaceClass == UsbConstants.USB_CLASS_CDC_DATA && !gotData) {
                for (e in 0 until itf.endpointCount) {
                    val ep = itf.getEndpoint(e)
                    if (ep.type != UsbConstants.USB_ENDPOINT_XFER_BULK) continue
                    if (ep.direction == UsbConstants.USB_DIR_IN) epIn = ep else epOut = ep
                }
                c.claimInterface(itf, true); gotData = true
            }
        }
        if (!gotData) { c.close(); throw IOException("找不到 USB 序列埠介面") }
        // DTR + RTS on (never the special toggle sequence that reboots into the bootloader)
        if (comm >= 0) c.controlTransfer(0x21, 0x22, 3, comm, null, 0, 1000)
        conn = c
    }

    fun close() { try { conn?.close() } catch (_: Exception) {} ; conn = null }

    @Synchronized
    fun send(type: Int, payload: ByteArray = ByteArray(0)) {
        val c = conn ?: throw IOException("沒有連線")
        val frame = ByteArray(4 + payload.size)
        frame[0] = 0xA5.toByte(); frame[1] = type.toByte()
        frame[2] = (payload.size and 0xFF).toByte(); frame[3] = (payload.size shr 8).toByte()
        System.arraycopy(payload, 0, frame, 4, payload.size)
        var off = 0
        while (off < frame.size) {
            val n = c.bulkTransfer(epOut, frame, off, minOf(16384, frame.size - off), 3000)
            if (n < 0) throw IOException("USB 寫入失敗")
            off += n
        }
    }

    /** Reads whatever arrived (waits up to [timeoutMs]) and returns complete frames. */
    fun poll(timeoutMs: Int): List<Pair<Int, ByteArray>> {
        val c = conn ?: throw IOException("沒有連線")
        val n = c.bulkTransfer(epIn, rx, rx.size, timeoutMs)
        if (n > 0) buf.write(rx, 0, n)
        else if (n < 0 && !deviceStillThere()) throw IOException("T-Dongle 已拔除")
        val out = ArrayList<Pair<Int, ByteArray>>()
        var data = buf.toByteArray()
        var i = 0
        while (true) {
            while (i < data.size && data[i] != 0xA5.toByte()) i++
            if (data.size - i < 4) break
            val len = (data[i + 2].toInt() and 0xFF) or ((data[i + 3].toInt() and 0xFF) shl 8)
            if (data.size - i < 4 + len) break
            out.add(Pair(data[i + 1].toInt() and 0xFF, data.copyOfRange(i + 4, i + 4 + len)))
            i += 4 + len
        }
        buf.reset(); if (i < data.size) buf.write(data, i, data.size - i)
        return out
    }

    private fun deviceStillThere() = usb.deviceList.values.any { it.deviceName == device.deviceName }

    companion object {
        const val VID = 0x303A
        const val H_HELLO = 1; const val H_PASTE = 2; const val H_IMG_BEGIN = 3; const val H_IMG_DATA = 4; const val H_IMG_END = 5
        const val D_HELLO = 0x81; const val D_TEXT = 0x82; const val D_ACK = 0x83

        /** The running PadKey firmware exposes a HID interface; the ROM bootloader does not. */
        fun isPadKey(d: UsbDevice): Boolean =
            d.vendorId == VID && (0 until d.interfaceCount).any { d.getInterface(it).interfaceClass == UsbConstants.USB_CLASS_HID }
    }
}
