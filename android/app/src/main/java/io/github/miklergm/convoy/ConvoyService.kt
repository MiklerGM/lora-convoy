package io.github.miklergm.convoy

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Intent
import android.os.IBinder
import android.util.Log
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

/**
 * Keeps the riders on OsmAnd's map once a second, while OsmAnd is the app in front. The riders
 * come from [MockRiders] until M4 puts a board here.
 *
 * All OsmAnd calls run on one worker thread: they are blocking binder calls.
 */
class ConvoyService : Service() {

    private val worker = Executors.newSingleThreadScheduledExecutor()
    private lateinit var osmand: OsmAnd
    private val startMs = System.currentTimeMillis()
    /** Where the mock riders circle: OsmAnd's map centre when they started. */
    private var centre: Pair<Double, Double>? = null

    override fun onBind(intent: Intent?): IBinder? = null

    override fun onCreate() {
        super.onCreate()
        val nm = getSystemService(NotificationManager::class.java)
        nm.createNotificationChannel(NotificationChannel(CHANNEL, "Riders on the map", NotificationManager.IMPORTANCE_LOW))
        val open = PendingIntent.getActivity(this, 0, Intent(this, MainActivity::class.java), PendingIntent.FLAG_IMMUTABLE)
        startForeground(1, Notification.Builder(this, CHANNEL)
            .setSmallIcon(android.R.drawable.ic_dialog_map)
            .setContentTitle("Mock riders on the OsmAnd map")
            .setContentIntent(open)
            .setOngoing(true)
            .build())
        osmand = OsmAnd(this)
        osmand.connect()
        worker.scheduleWithFixedDelay(::tick, 0, 1, TimeUnit.SECONDS)
    }

    private fun tick() {
        try {
            if (osmand.state == OsmAnd.State.MISSING) osmand.connect() // installed meanwhile?
            val c = centre ?: osmand.mapCentre()?.also { centre = it }
            if (c != null) {
                val now = System.currentTimeMillis()
                osmand.show(MockRiders.at(c.first, c.second, startMs, now).map { it.toPoint(now) })
            }
            state = osmand.state
            version = osmand.version
        } catch (e: Exception) {
            // an exception would cancel every later tick without a word
            Log.e(TAG, "tick failed", e)
        }
    }

    override fun onDestroy() {
        worker.execute {
            osmand.clear()
            osmand.disconnect()
        }
        worker.shutdown()
        state = null
        super.onDestroy()
    }

    companion object {
        private const val TAG = "ConvoyService"
        private const val CHANNEL = "riders"

        /** For the screen: null while the service is not running. */
        @Volatile var state: OsmAnd.State? = null
            private set
        @Volatile var version: String? = null
            private set
    }
}
