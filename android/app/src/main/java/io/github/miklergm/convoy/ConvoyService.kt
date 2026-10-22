package io.github.miklergm.convoy

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.os.IBinder
import android.util.Log
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.Executors
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
    private val boardRiders = ConcurrentHashMap<String, Rider>()
    private var nextPoll = 0L
    /** What this link's board has been sent: this phone's group, and the question for its riders. */
    @Volatile private var sentGroup: Invitation? = null
    @Volatile private var askedRiders = false

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
        osmand = OsmAnd(this)
        osmand.connect()
        link = source.link(this)?.also { l ->
            l.open(::onLine) { s ->
                linkState = s
                // its version first: a board that speaks another protocol is asked nothing more
                if (s.startsWith("connected")) {
                    sentGroup = null
                    askedRiders = false
                    l.send("STATUS")
                }
            }
        }
        worker.scheduleWithFixedDelay(::tick, 0, 1, TimeUnit.SECONDS)
    }

    private fun onLine(line: String) {
        val status = BoardLines.status(line)
        if (status != null) {
            if (status.proto != BoardLines.PROTO) {
                linkState = "refused: the board speaks protocol ${status.proto}, this app ${BoardLines.PROTO}"
                link?.close()
                link = null
                boardRiders.clear()
            } else {
                board = status
                setUp(status)
            }
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
        BoardLines.rider(line, System.currentTimeMillis())?.let { boardRiders[it.id] = it }
    }

    /**
     * This phone's group to the board, once per link and again when the group changes: the board
     * keeps its riders for the key it has (firmware node_set_key). Then what the board knows,
     * asked after the key, so no rider of a former group comes back in the answer.
     */
    private fun setUp(status: BoardLines.Status) {
        val l = link ?: return
        val mine = GroupActivity.Group.load(this)
        if (mine != null && mine != sentGroup) {
            if (!status.keyed || status.group != mine.groupTag()) boardRiders.clear()
            mine.commands().forEach(l::send)
            sentGroup = mine
            askedRiders = false
        }
        if (!askedRiders) {
            l.send("RIDERS")
            askedRiders = true
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        if (intent?.action == ACTION_FORGET) link?.send("FORGET")
        return START_STICKY
    }

    private fun tick() {
        try {
            if (osmand.state == OsmAnd.State.MISSING) osmand.connect() // installed meanwhile?
            val now = System.currentTimeMillis()
            val riders = if (source is Source.Mock) {
                val c = centre ?: osmand.mapCentre()?.also { centre = it }
                c?.let { MockRiders.at(it.first, it.second, startMs, now) }
            } else {
                // a live board says what it knows every 10 s, and this keeps its link from going quiet
                if (now >= nextPoll) {
                    link?.send("STATUS")
                    nextPoll = now + 10_000
                }
                boardRiders.values.toList()
            }
            if (riders != null) osmand.show(riders.map { it.toPoint(now) })
            state = osmand.state
            version = osmand.version
        } catch (e: Exception) {
            // an exception would cancel every later tick without a word
            Log.e(TAG, "tick failed", e)
        }
    }

    override fun onDestroy() {
        link?.close()
        worker.execute {
            osmand.clear()
            osmand.disconnect()
        }
        worker.shutdown()
        state = null
        board = null
        linkState = null
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
        /** Asks the board to forget this phone; FORGOTTEN then ends the link on both sides. */
        const val ACTION_FORGET = "io.github.miklergm.convoy.FORGET"
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
    }
}
