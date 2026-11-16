package io.github.miklergm.convoy

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import android.os.SystemClock
import android.util.Log
import java.util.concurrent.Executors
import kotlin.math.roundToLong
import java.util.concurrent.TimeUnit

/**
 * Keeps the riders on OsmAnd's map once a second, while OsmAnd is the app in front. The riders come
 * from the source the rider chose: a board, over its phone API, or the mock.
 *
 * All OsmAnd calls run on one worker thread: they are blocking binder calls. Lines from a board
 * arrive on the link's own thread and only touch the maps below.
 */
class ConvoyService : Service() {

    private val worker = Executors.newSingleThreadScheduledExecutor()
    private lateinit var osmand: OsmAnd
    private lateinit var source: Source
    private val startMs = System.currentTimeMillis()
    /** Where the mock riders circle: OsmAnd's map centre when they started. */
    private var centre: Pair<Double, Double>? = null
    @Volatile private var link: BoardLink? = null
    private val boardRiders = BoardRiders()
    /** The board's clock against ours, run by run, from CLOCK exchanges: for when it heard each rider. */
    private val boardClock = BoardClock()
    private var nextPoll = 0L
    /** The exchanges asked on this link, and when the next is due, on our monotonic clock. */
    @Volatile private var clocksAsked = 0
    private var nextClock = 0L
    /** What this link's board has been sent: this phone's group, and the question for its riders. */
    @Volatile private var sentGroup: Invitation? = null
    @Volatile private var askedRiders = false
    /** This phone's group, for the riders' names; read again at each status. */
    @Volatile private var group: Invitation? = null
    /** The board's clash count at its last status on this link, to see it grow. */
    @Volatile private var lastClash: Int? = null
    private lateinit var log: BoardLog
    /** What this link asked LOG for: where the log of the board's own lines ended then. */
    @Volatile private var logAsked: Long? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Riders on the map", NotificationManager.IMPORTANCE_LOW))
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        source = Source.load(this)
        startForeground(1, Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_dialog_map)
            .setContentTitle("Riders on the OsmAnd map")
            .setContentText(source.describe())
            .setContentIntent(open)
            .setOngoing(true)
            .build())
        sourceText = source.describe()
        log = BoardLog(java.io.File(filesDir, BoardLog.DIR))
        log.write("# started: ${source.describe()}")
        osmand = OsmAnd(this)
        osmand.connect()
        link = source.link(this)?.also { l ->
            l.open(::onLine) { s ->
                linkState = s
                log.write("# $s")
                // its version first: a board that speaks another protocol is asked nothing more
                if (s.startsWith("connected")) {
                    clocksAsked = 0
                    sentGroup = null
                    confirmed = null
                    askedRiders = false
                    lastClash = null
                    logAsked = null
                    send("STATUS")
                }
            }
        }
        worker.scheduleWithFixedDelay(::tick, 0, 1, TimeUnit.SECONDS)
    }

    /** A line to the board, in the log too, as the cable's logger has what it sends. */
    private fun send(line: String): Boolean {
        // one that could not go is not logged: the link's state says why, once, not every second
        val sent = link?.send(line) == true
        if (sent) log.write("> $line")
        return sent
    }

    private fun onLine(line: String) {
        val receivedMono = SystemClock.elapsedRealtime()
        val received = System.currentTimeMillis()
        log.write(line)
        BoardLines.logNumber(line)?.let {
            logNext = maxOf(logNext, it + 1)
            return
        }
        BoardLines.logStart(line)?.let {
            val asked = logAsked
            when {
                it.boot != logBoot -> log.write("# the board's run %08x: its own lines from %d".format(it.boot, it.from))
                asked != null && it.from > asked -> log.write("# the board's own lines $asked to ${it.from - 1} were no longer kept")
            }
            logBoot = it.boot
            return
        }
        BoardLines.clock(line)?.let { c ->
            boardClock.observe(c.boot, c.t1Ms, c.rxUs, c.txUs, receivedMono, c.utcMs)
            // the board's calendar: our UTC at a moment of its clock, give or take the reckoning and
            // our own clock's error, for the riders' ages it tells while it has no GNSS time
            boardClock.reckoning(c.boot, receivedMono)?.let { r ->
                val boardMs = c.txUs / 1000
                val utc = received - (receivedMono - (boardMs - r.offsetMs).roundToLong())
                send("TIME $boardMs $utc ${(r.errMs + PHONE_UTC_ERR_MS).roundToLong()}")
            }
            return
        }
        val status = BoardLines.status(line)
        if (status != null) {
            if (status.proto != BoardLines.PROTO) {
                linkState = "refused: the board speaks protocol ${status.proto}, this app ${BoardLines.PROTO}"
                link?.close()
                link = null
                boardRiders.clear()
            } else {
                board = status
                // its own lines too, from where this phone's log of them ends, once per link
                if (logAsked == null) {
                    logAsked = logNext
                    send("LOG %08x %d".format(logBoot, logNext))
                }
                boardRiders.group(status.group)
                lastClash?.let { if (status.clash > it) clashAtMs = SystemClock.elapsedRealtime() }
                lastClash = status.clash
                setUp()
            }
            return
        }
        BoardLines.setup(line)?.let { taken ->
            // the board has the group this phone sent, whole: the riders of the group before go
            // (BoardRiders), and the board's are asked
            val sent = sentGroup ?: return
            if (taken.of(sent)) {
                confirmed = sent
                boardRiders.group(taken.group)
                askRiders()
            } else {
                linkState = "the board took another setup than ${sent.name}'s: $line"
            }
            return
        }
        if (line.startsWith("ERR group")) {
            linkState = "the board refused ${sentGroup?.name ?: "the group"}: $line"
            return
        }
        // the answer to a command from a screen; TIME's come every 10 s and say nothing to the rider
        if ((line.startsWith("OK ") && !line.startsWith("OK time=")) || line.startsWith("ERR")) {
            reply = line
            return
        }
        if (line == "FORGOTTEN") {
            // the board has dropped us; Android's own bond goes too, and the board stops being ours
            (link as? BleBoardLink)?.removeBond()
            Source.save(this, Source.Mock)
            linkState = "the board forgot this phone"
            stopSelf()
            return
        }
        // kept with the moment its board heard it, which the reckoned clocks place anew each tick
        BoardLines.rider(line, received, group)?.let {
            boardRiders.take(it, BoardLines.groupOf(line), BoardLines.heard(line))
        }
    }

    /**
     * This phone's group to the board, whole, once per link and again when the group changes: the
     * board keeps its riders for the key it has (firmware node_set_key). Its riders are asked once
     * it has taken the group. A RIDER line names the group it was heard in, so one the board made
     * before the new key is not taken, however late it comes.
     */
    private fun setUp() {
        if (link == null) return
        val mine = GroupActivity.Group.load(this)
        group = mine
        if (mine != null && mine != sentGroup) {
            confirmed = null
            sentGroup = mine
            send(mine.setup())
        } else if (mine == null && !askedRiders) {
            askRiders()
        }
    }

    private fun askRiders() {
        if (link == null) return
        askedRiders = true
        send("RIDERS")
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        when (intent?.action) {
            ACTION_FORGET -> send("FORGET")
            // and its status at once, so the screen shows what the board took
            ACTION_COMMAND -> intent.getStringExtra(EXTRA_LINE)?.let { if (send(it)) send("STATUS") }
        }
        return START_STICKY
    }

    private fun tick() {
        try {
            if (osmand.state == OsmAnd.State.MISSING) osmand.connect() // installed meanwhile?
            // every wait on our monotonic clock, which a change of the phone's date leaves alone; the
            // riders' ages on the board's GNSS time where it has one, else on this phone's calendar
            val mono = SystemClock.elapsedRealtime()
            val now = boardClock.calendarMs(mono, System.currentTimeMillis())
            val riders = if (source is Source.Mock) {
                val c = centre ?: osmand.mapCentre()?.also { centre = it }
                c?.let { MockRiders.at(it.first, it.second, startMs, now) }
            } else {
                // a live board says what it knows every 10 s, and this keeps its link from going quiet
                if (mono >= nextPoll) {
                    send("STATUS")
                    nextPoll = mono + 10_000
                }
                // the clocks: three exchanges as a link begins, then one every 10 s
                if ((clocksAsked < 3 || mono >= nextClock) && send("CLOCK $mono")) {
                    clocksAsked++
                    nextClock = mono + 10_000
                }
                boardRiders.all(boardClock, mono, now)
            }
            if (riders != null) osmand.show(riders.map { it.toPoint(now) })
            ConvoyService.riders = riders.orEmpty()
            ridersAtMs = now
            state = osmand.state
            version = osmand.version
        } catch (e: Exception) {
            // an exception would cancel every later tick without a word
            Log.e(TAG, "tick failed", e)
        }
    }

    override fun onDestroy() {
        log.write("# stopped")
        log.close()
        link?.close()
        worker.execute {
            osmand.clear()
            osmand.disconnect()
        }
        worker.shutdown()
        state = null
        board = null
        confirmed = null
        linkState = null
        reply = null
        riders = emptyList()
        super.onDestroy()
    }

    /** Where the riders come from, kept so a restarted service comes back to the same one. */
    sealed class Source {
        object Mock : Source()
        data class Simulated(val host: String, val port: Int) : Source()
        data class Board(val address: String, val name: String) : Source()

        fun describe() = when (this) {
            Mock -> "Mock riders"
            is Simulated -> "Simulated board at $host:$port"
            is Board -> "Board $name"
        }

        fun link(context: Context): BoardLink? = when (this) {
            Mock -> null
            is Simulated -> TcpBoardLink(host, port)
            is Board -> BleBoardLink(context, address)
        }

        companion object {
            private const val PREFS = "source"

            fun load(context: Context): Source {
                val p = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
                return when (p.getString("kind", "mock")) {
                    "tcp" -> Simulated(p.getString("host", "10.0.2.2")!!, p.getInt("port", 4403))
                    "ble" -> Board(p.getString("address", "")!!, p.getString("name", "")!!)
                    else -> Mock
                }
            }

            fun save(context: Context, source: Source) {
                val e = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit()
                when (source) {
                    Mock -> e.putString("kind", "mock")
                    is Simulated -> e.putString("kind", "tcp").putString("host", source.host).putInt("port", source.port)
                    is Board -> e.putString("kind", "ble").putString("address", source.address).putString("name", source.name)
                }
                e.apply()
            }
        }
    }

    companion object {
        private const val TAG = "ConvoyService"
        /** How far this phone's clock, set from the network, is taken to be from UTC: not measured. */
        private const val PHONE_UTC_ERR_MS = 1_000.0
        /** Asks the board to forget this phone; FORGOTTEN then ends the link on both sides. */
        const val ACTION_FORGET = "io.github.miklergm.convoy.FORGET"
        /** One line of the phone API to the board (EXTRA_LINE), from a screen: a state, a setting. */
        const val ACTION_COMMAND = "io.github.miklergm.convoy.COMMAND"
        const val EXTRA_LINE = "line"

        /** A command to the board this service is linked to; false, and nothing sent, without one. */
        fun command(context: Context, line: String): Boolean {
            if (board == null || state == null) return false
            context.startService(Intent(context, ConvoyService::class.java).setAction(ACTION_COMMAND).putExtra(EXTRA_LINE, line))
            return true
        }
        private const val CHANNEL = "riders"

        /** For the screen: null while the service is not running. */
        @Volatile var state: OsmAnd.State? = null
            private set
        @Volatile var version: String? = null
            private set
        @Volatile var sourceText: String? = null
            private set
        @Volatile var linkState: String? = null
            private set
        @Volatile var board: BoardLines.Status? = null
            private set
        /** The group the board has said it took on this link: this phone's, once it has. */
        @Volatile var confirmed: Invitation? = null
            private set
        /** Where this phone's log of the board's own lines ends: the board's run and the next line's
         * number, kept over links and over the service started again, so a new link asks only for
         * what was missed. ponytail: not over the app's process; a new one asks for all the board kept. */
        @Volatile private var logBoot = 0L
        @Volatile private var logNext = 0L
        /** The board's answer to the last command from a screen. */
        @Volatile var reply: String? = null
            private set
        /** The riders as last put on the map, and the calendar's time then. */
        @Volatile var riders: List<Rider> = emptyList()
            private set
        @Volatile var ridersAtMs = 0L
            private set
        /** When the board last heard another radio in its slot, on our monotonic clock; 0 for never. */
        @Volatile var clashAtMs = 0L
            private set
    }
}
