package io.github.miklergm.convoy

import android.annotation.SuppressLint
import android.bluetooth.BluetoothDevice
import android.bluetooth.BluetoothGatt
import android.bluetooth.BluetoothGattCallback
import android.bluetooth.BluetoothGattCharacteristic
import android.bluetooth.BluetoothGattDescriptor
import android.bluetooth.BluetoothManager
import android.bluetooth.BluetoothProfile
import android.bluetooth.BluetoothStatusCodes
import android.bluetooth.le.ScanCallback
import android.bluetooth.le.ScanResult
import android.bluetooth.le.ScanSettings
import android.content.Context
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.util.Log
import java.util.ArrayDeque
import java.util.UUID

/**
 * A board's phone API over Bluetooth: the Nordic UART Service the firmware serves (ble_nus.c),
 * after dmd2-lora-bridge's BleRadioLink, kept to what a board needs. The board is chosen once, by
 * address; this link then connects to it and reconnects after every drop.
 *
 * The board's command characteristic takes only encrypted writes, so the first write starts
 * Android's pairing; the board refuses any phone once it has an owner (FORGET or its button frees
 * it). Callbacks arrive on binder threads, timers on the main looper; the state is guarded by this
 * object's lock.
 *
 * ponytail: no stale-bond detection and no explicit-selection bookkeeping, both of which the bridge
 * learned it needed on rides; add them from BleRadioLink when M7's soak test asks.
 */
@SuppressLint("MissingPermission") // asked for by MainActivity before a board can be chosen
class BleBoardLink(context: Context, private val address: String) : BoardLink {

    private val appContext = context.applicationContext
    private val handler = Handler(Looper.getMainLooper())
    private val lines = LineAssembler()
    private val outbound = ArrayDeque<ByteArray>()
    private var onLine: (String) -> Unit = {}
    private var onState: (String) -> Unit = {}
    private var gatt: BluetoothGatt? = null
    private var rx: BluetoothGattCharacteristic? = null
    private var mtu = DEFAULT_MTU
    private var ready = false
    private var writing = false
    private var failures = 0
    private var closed = false
    private var timeout: Runnable? = null

    @Synchronized
    override fun open(onLine: (String) -> Unit, onState: (String) -> Unit) {
        this.onLine = onLine
        this.onState = onState
        connect()
    }

    @Synchronized
    private fun connect() {
        if (closed) return
        val adapter = appContext.getSystemService(BluetoothManager::class.java)?.adapter
        if (adapter == null || !adapter.isEnabled) {
            onState("Bluetooth is off")
            retryLater()
            return
        }
        onState("connecting to $address")
        // One bound covers connecting, discovery, pairing and the subscription.
        timeout = Runnable { fail("no working link within ${LINK_TIMEOUT_MS / 1000} s") }.also {
            handler.postDelayed(it, LINK_TIMEOUT_MS)
        }
        try {
            gatt = adapter.getRemoteDevice(address)
                .connectGatt(appContext, false, callback, BluetoothDevice.TRANSPORT_LE)
        } catch (e: SecurityException) {
            onState("no Bluetooth permission")
        }
    }

    @Synchronized
    private fun retryLater() {
        if (!closed) handler.postDelayed({ connect() }, RETRY_MS)
    }

    @Synchronized
    private fun fail(reason: String) {
        Log.w(TAG, "link to $address: $reason")
        onState("$reason; trying again")
        teardown()
        retryLater()
    }

    @Synchronized
    private fun teardown() {
        timeout?.let { handler.removeCallbacks(it) }
        timeout = null
        ready = false
        writing = false
        failures = 0
        outbound.clear()
        lines.reset()
        rx = null
        mtu = DEFAULT_MTU
        gatt?.let { runCatching { it.close() } }
        gatt = null
    }

    private val callback = object : BluetoothGattCallback() {
        override fun onConnectionStateChange(g: BluetoothGatt, status: Int, newState: Int) {
            synchronized(this@BleBoardLink) {
                if (g !== gatt) return
                if (status == BluetoothGatt.GATT_SUCCESS && newState == BluetoothProfile.STATE_CONNECTED) {
                    // a whole RIDER line in one notification, if the phone agrees
                    if (!g.requestMtu(PREFERRED_MTU) && !g.discoverServices()) fail("service discovery refused")
                } else {
                    fail("disconnected ($status)")
                }
            }
        }

        override fun onMtuChanged(g: BluetoothGatt, newMtu: Int, status: Int) {
            synchronized(this@BleBoardLink) {
                if (g !== gatt) return
                if (status == BluetoothGatt.GATT_SUCCESS) mtu = newMtu
                if (!g.discoverServices()) fail("service discovery refused")
            }
        }

        override fun onServicesDiscovered(g: BluetoothGatt, status: Int) {
            synchronized(this@BleBoardLink) {
                if (g !== gatt) return
                val service = g.getService(SERVICE_UUID)
                val tx = service?.getCharacteristic(TX_UUID)
                rx = service?.getCharacteristic(RX_UUID)
                val cccd = tx?.getDescriptor(CCCD_UUID)
                if (status != BluetoothGatt.GATT_SUCCESS || rx == null || cccd == null) {
                    fail("not a LoRa Convoy board")
                    return
                }
                val subscribed = g.setCharacteristicNotification(tx, true) &&
                    if (Build.VERSION.SDK_INT >= 33) {
                        g.writeDescriptor(cccd, BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE) ==
                            BluetoothStatusCodes.SUCCESS
                    } else {
                        @Suppress("DEPRECATION")
                        cccd.value = BluetoothGattDescriptor.ENABLE_NOTIFICATION_VALUE
                        @Suppress("DEPRECATION")
                        g.writeDescriptor(cccd)
                    }
                // without it nothing would ever arrive, and the link would look healthy while deaf
                if (!subscribed) fail("could not subscribe to the board's lines")
            }
        }

        override fun onDescriptorWrite(g: BluetoothGatt, d: BluetoothGattDescriptor, status: Int) {
            synchronized(this@BleBoardLink) {
                if (g !== gatt || d.uuid != CCCD_UUID) return
                if (status != BluetoothGatt.GATT_SUCCESS) {
                    fail("subscription refused ($status)")
                    return
                }
                timeout?.let { handler.removeCallbacks(it) }
                timeout = null
                ready = true
                onState("connected to $address")
            }
        }

        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
            inbound(g, c, value)
        }

        @Deprecated("Android 12's callback")
        @Suppress("DEPRECATION")
        override fun onCharacteristicChanged(g: BluetoothGatt, c: BluetoothGattCharacteristic) {
            inbound(g, c, c.value ?: return)
        }

        override fun onCharacteristicWrite(g: BluetoothGatt, c: BluetoothGattCharacteristic, status: Int) {
            synchronized(this@BleBoardLink) {
                if (g !== gatt || c.uuid != RX_UUID) return
                writing = false
                var delay = 0L
                when (status) {
                    BluetoothGatt.GATT_SUCCESS -> {
                        outbound.poll() // confirmed: the next fragment may go
                        failures = 0
                    }
                    // 5 and 15: the board wants an encrypted link. Android starts pairing and the
                    // same fragment simply goes again once that is done.
                    GATT_INSUFFICIENT_AUTHENTICATION, GATT_INSUFFICIENT_ENCRYPTION -> {
                        onState("pairing with the board: accept Android's dialog")
                        delay = PAIRING_WAIT_MS
                    }
                    else -> if (++failures > MAX_WRITE_FAILURES) {
                        fail("writes keep failing ($status)")
                        return
                    }
                }
                handler.postDelayed({ pump() }, delay)
            }
        }
    }

    private fun inbound(g: BluetoothGatt, c: BluetoothGattCharacteristic, value: ByteArray) {
        val got = synchronized(this) {
            if (g !== gatt || c.uuid != TX_UUID) return
            lines.feed(value)
        }
        got.forEach(onLine) // outside the lock: the caller may send in turn
    }

    @Synchronized
    override fun send(line: String): Boolean {
        if (!ready || closed) return false
        val bytes = "$line\n".toByteArray(Charsets.US_ASCII)
        val chunk = (mtu - ATT_OVERHEAD).coerceAtLeast(20)
        var at = 0
        while (at < bytes.size) {
            val end = minOf(at + chunk, bytes.size)
            outbound.add(bytes.copyOfRange(at, end))
            at = end
        }
        pump()
        return true
    }

    /** One write at a time: the stack refuses a second while the first is outstanding. */
    @Synchronized
    private fun pump() {
        if (writing || !ready) return
        val g = gatt ?: return
        val c = rx ?: return
        val next = outbound.peek() ?: return
        val started = try {
            if (Build.VERSION.SDK_INT >= 33) {
                g.writeCharacteristic(c, next, BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT) ==
                    BluetoothStatusCodes.SUCCESS
            } else {
                @Suppress("DEPRECATION")
                c.value = next
                c.writeType = BluetoothGattCharacteristic.WRITE_TYPE_DEFAULT
                @Suppress("DEPRECATION")
                g.writeCharacteristic(c)
            }
        } catch (e: SecurityException) {
            false
        }
        if (started) {
            writing = true
        } else if (++failures > MAX_WRITE_FAILURES) {
            fail("writes refused")
        } else {
            handler.postDelayed({ pump() }, WRITE_RETRY_MS)
        }
    }

    /** Drops Android's own bond with the board, after the board said FORGOTTEN. */
    fun removeBond() {
        try {
            val device = appContext.getSystemService(BluetoothManager::class.java)?.adapter?.getRemoteDevice(address)
            device?.javaClass?.getMethod("removeBond")?.invoke(device)
        } catch (e: Exception) {
            Log.w(TAG, "could not remove the bond: ${e.message}")
        }
    }

    @Synchronized
    override fun close() {
        closed = true
        handler.removeCallbacksAndMessages(null)
        teardown()
    }

    /** Boards in range, by the name their firmware advertises; for the screen that picks one. */
    @SuppressLint("MissingPermission")
    class Scanner(context: Context) {
        private val adapter = context.getSystemService(BluetoothManager::class.java)?.adapter
        private val found = LinkedHashMap<String, String>() // address -> name
        private var callback: ScanCallback? = null

        /** False when Bluetooth is off or missing. [onFound] gets address and name, on a binder thread. */
        fun start(onFound: (String, String) -> Unit): Boolean {
            val scanner = adapter?.takeIf { it.isEnabled }?.bluetoothLeScanner ?: return false
            stop()
            val cb = object : ScanCallback() {
                override fun onScanResult(callbackType: Int, result: ScanResult) {
                    // The UART service's UUID is in many DIY devices; the name tells ours apart.
                    val name = result.scanRecord?.deviceName ?: return
                    if (!name.startsWith(NAME_PREFIX)) return
                    val fresh = synchronized(found) { found.put(result.device.address, name) == null }
                    if (fresh) onFound(result.device.address, name)
                }
            }
            callback = cb
            scanner.startScan(null, ScanSettings.Builder().setScanMode(ScanSettings.SCAN_MODE_LOW_LATENCY).build(), cb)
            return true
        }

        fun stop() {
            val cb = callback ?: return
            callback = null
            runCatching { adapter?.bluetoothLeScanner?.stopScan(cb) }
        }
    }

    companion object {
        private const val TAG = "BleBoardLink"
        /** The firmware advertises this and eight hex digits of its address (ble_nus.c). */
        const val NAME_PREFIX = "LoRaConvoy"
        val SERVICE_UUID: UUID = UUID.fromString("6e400001-b5a3-f393-e0a9-e50e24dcca9e")
        val RX_UUID: UUID = UUID.fromString("6e400002-b5a3-f393-e0a9-e50e24dcca9e")
        val TX_UUID: UUID = UUID.fromString("6e400003-b5a3-f393-e0a9-e50e24dcca9e")
        val CCCD_UUID: UUID = UUID.fromString("00002902-0000-1000-8000-00805f9b34fb")
        private const val DEFAULT_MTU = 23
        private const val PREFERRED_MTU = 247
        private const val ATT_OVERHEAD = 3
        /** Long enough to answer a pairing dialog. */
        private const val LINK_TIMEOUT_MS = 30_000L
        private const val RETRY_MS = 5_000L
        private const val WRITE_RETRY_MS = 50L
        private const val PAIRING_WAIT_MS = 1_500L
        private const val MAX_WRITE_FAILURES = 3
        private const val GATT_INSUFFICIENT_AUTHENTICATION = 5
        private const val GATT_INSUFFICIENT_ENCRYPTION = 15
    }
}
