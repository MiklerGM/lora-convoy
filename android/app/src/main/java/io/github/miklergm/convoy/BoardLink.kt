package io.github.miklergm.convoy

import android.util.Log
import java.io.IOException
import java.net.InetSocketAddress
import java.net.Socket
import java.util.concurrent.Executors
import java.util.concurrent.RejectedExecutionException

/**
 * A board's phone API over some byte stream: lines in, lines out, one board at a time. Over TCP
 * to a simulated board here (firmware/test/sim.c serve); over Bluetooth in BleBoardLink.
 */
interface BoardLink {
    /** Starts connecting, and reconnects until [close]. Lines and states arrive on the link's own thread. */
    fun open(onLine: (String) -> Unit, onState: (String) -> Unit)

    /** One line to the board, from any thread, the main one included; false if it could not go. */
    fun send(line: String): Boolean

    fun close()
}

/**
 * Bytes to lines. A line longer than any the board sends is dropped up to its end, so a broken or
 * hostile stream cannot grow without bound, and a tail is never taken for a line of its own. The
 * same for TCP reads and for Bluetooth notifications, which cut lines anywhere.
 */
class LineAssembler(private val max: Int = 512) {
    private val buf = StringBuilder()
    private var discarding = false

    fun feed(bytes: ByteArray, len: Int = bytes.size): List<String> {
        val out = ArrayList<String>()
        for (i in 0 until len) {
            val c = (bytes[i].toInt() and 0xFF).toChar()
            when {
                c == '\n' || c == '\r' -> {
                    if (!discarding && buf.isNotEmpty()) out.add(buf.toString())
                    buf.setLength(0)
                    discarding = false
                }
                discarding -> Unit
                buf.length >= max -> {
                    buf.setLength(0)
                    discarding = true
                }
                else -> buf.append(c)
            }
        }
        return out
    }

    fun reset() {
        buf.setLength(0)
        discarding = false
    }
}

/** A simulated board, as `make -C firmware sim ARGS="serve 3"` serves it; from an emulator, 10.0.2.2. */
class TcpBoardLink(private val host: String, private val port: Int) : BoardLink {

    @Volatile private var socket: Socket? = null
    @Volatile private var closed = false
    /** Writes on a thread of their own, in order: the service sends from the main thread too. */
    private val writer = Executors.newSingleThreadExecutor { r -> Thread(r, "board-tcp-write").apply { isDaemon = true } }

    override fun open(onLine: (String) -> Unit, onState: (String) -> Unit) {
        Thread({
            val lines = LineAssembler()
            while (!closed) {
                try {
                    onState("connecting to $host:$port")
                    Socket().use { s ->
                        s.connect(InetSocketAddress(host, port), 3000)
                        socket = s
                        lines.reset()
                        onState("connected to $host:$port")
                        val input = s.getInputStream()
                        val buf = ByteArray(1024)
                        while (!closed) {
                            val n = input.read(buf)
                            if (n < 0) break
                            lines.feed(buf, n).forEach(onLine)
                        }
                    }
                } catch (e: IOException) {
                    Log.w(TAG, "board link: ${e.message}")
                }
                socket = null
                if (!closed) {
                    onState("no board at $host:$port; trying again")
                    try {
                        Thread.sleep(3000)
                    } catch (e: InterruptedException) {
                        return@Thread
                    }
                }
            }
        }, "board-tcp").apply { isDaemon = true }.start()
    }

    override fun send(line: String): Boolean {
        val s = socket ?: return false
        return try {
            writer.execute {
                try {
                    s.getOutputStream().apply { write("$line\n".toByteArray(Charsets.US_ASCII)); flush() }
                } catch (e: IOException) {
                    Log.w(TAG, "board link: ${e.message}")
                }
            }
            true
        } catch (e: RejectedExecutionException) {
            false // closed meanwhile
        }
    }

    override fun close() {
        closed = true
        writer.shutdown()
        try {
            socket?.close()
        } catch (e: IOException) {
            // closing anyway
        }
    }

    companion object {
        private const val TAG = "TcpBoardLink"
    }
}
