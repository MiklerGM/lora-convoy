package io.github.miklergm.convoy

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.net.Uri
import android.os.IBinder
import android.os.RemoteException
import android.util.Log
import net.osmand.aidlapi.IOsmAndAidlInterface
import net.osmand.aidlapi.gpx.ImportGpxParams
import net.osmand.aidlapi.map.ALatLon
import net.osmand.aidlapi.maplayer.AMapLayer
import net.osmand.aidlapi.maplayer.AddMapLayerParams
import net.osmand.aidlapi.maplayer.RemoveMapLayerParams
import net.osmand.aidlapi.maplayer.UpdateMapLayerParams
import net.osmand.aidlapi.maplayer.point.AMapPoint
import net.osmand.aidlapi.maplayer.point.RemoveMapPointParams
import net.osmand.aidlapi.navigation.NavigateGpxParams

/**
 * Our side of OsmAnd's AIDL V2 API: one layer, one point per rider.
 *
 * Every call but [connect] and [disconnect] is a blocking binder call: the caller keeps them off
 * the main thread, on one thread. OsmAnd answers false or null, not an error, while it has not
 * enabled us (on 5.4, until the rider switches us on in Plugins) and while it is still starting,
 * so the caller simply tries again on its next tick.
 */
class OsmAnd(private val context: Context) {

    enum class State { MISSING, CONNECTING, REFUSED, SHOWING }

    @Volatile var state = State.CONNECTING
        private set
    @Volatile var version: String? = null
        private set
    /** The OsmAnd found by [connect]: the package we grant our routes to. */
    @Volatile var pkg: String? = null
        private set

    /** Set on the main thread by the connection, read on the caller's. */
    @Volatile private var api: IOsmAndAidlInterface? = null
    private var shown = emptySet<String>()

    private val connection = object : ServiceConnection {
        override fun onServiceConnected(name: ComponentName?, service: IBinder?) {
            api = IOsmAndAidlInterface.Stub.asInterface(service)
        }

        override fun onServiceDisconnected(name: ComponentName?) {
            api = null
            state = State.CONNECTING
        }
    }

    /** Which OsmAnd is installed: OsmAnd+ and OsmAnd~ share a package; then the free one, a nightly. */
    fun installed(): String? = PACKAGES.firstOrNull { pkg ->
        context.packageManager.queryIntentServices(Intent(ACTION).setPackage(pkg), 0).isNotEmpty()
    }

    fun connect() {
        val pkg = installed()
        this.pkg = pkg
        if (pkg == null) {
            state = State.MISSING
            return
        }
        state = State.CONNECTING
        // Binding to OsmAnd's service starts OsmAnd's process if it is not running.
        if (!context.bindService(Intent(ACTION).setPackage(pkg), connection, Context.BIND_AUTO_CREATE)) {
            Log.w(TAG, "bindService to $pkg returned false")
        }
    }

    fun disconnect() {
        try {
            context.unbindService(connection) // needed even after bindService returned false
        } catch (e: IllegalArgumentException) {
            // never bound
        }
        api = null
    }

    /** For one-off callers, on their own thread: waits until the service is bound, or gives up. */
    fun awaitConnected(timeoutMs: Long): Boolean {
        val until = System.currentTimeMillis() + timeoutMs
        while (api == null && state != State.MISSING && System.currentTimeMillis() < until) Thread.sleep(50)
        return api != null
    }

    /**
     * Imports a GPX and shows it on the map. True means only that OsmAnd took the request: it copies
     * the file into its tracks afterwards. [uri] must be readable by OsmAnd (see [Routes.uri]).
     */
    fun importGpx(uri: Uri, fileName: String): Boolean = call {
        it.importGpx(ImportGpxParams(uri, "$TRACKS_DIR/$fileName", TRACK_COLOR, true))
    } == true

    /**
     * Navigates along a GPX as it is: never snapped to roads, which off-road could move the route
     * onto another trail (06), and joined where the rider meets it, not at its start. OsmAnd asks
     * the rider before it replaces a route already running.
     */
    fun navigateGpx(uri: Uri): Boolean = call {
        it.navigateGpx(NavigateGpxParams(uri, false, true).apply {
            isSnapToRoad = false
            // must be set: OsmAnd's library unboxes it unchecked, and a null crashes the caller
            setPassWholeRoute(false)
        })
    } == true

    /** The centre of OsmAnd's map, or null while OsmAnd has not let us in. */
    fun mapCentre(): Pair<Double, Double>? {
        val info = call { it.appInfo } ?: run {
            if (api != null) state = State.REFUSED
            return null
        }
        version = info.osmAndVersion
        return info.mapLocation?.let { it.latitude to it.longitude }
    }

    /**
     * Puts exactly these points on the map, in one update of the whole layer: each update makes
     * OsmAnd reload its map, so one a tick rather than one per rider. On OsmAnd's OpenGL engine
     * every update still hides the whole layer while it is rebuilt (docs/05).
     */
    fun show(points: List<PointSpec>) {
        val ok = call { a ->
            val layer = UpdateMapLayerParams(AMapLayer(LAYER_ID, LAYER_NAME, Z_ORDER, points.map(::toAMapPoint)))
            // update answers false while the layer does not exist: first time, or OsmAnd restarted
            val up = a.updateMapLayer(layer) || a.addMapLayer(AddMapLayerParams(layer.layer))
            // update only adds and moves points; a point already gone answers false, no refusal
            if (up) (shown - points.map { it.id }.toSet()).forEach { a.removeMapPoint(RemoveMapPointParams(LAYER_ID, it)) }
            up
        } ?: return
        if (ok) {
            shown = points.map { it.id }.toSet()
            state = State.SHOWING
        } else {
            state = State.REFUSED
        }
    }

    fun clear() {
        call { it.removeMapLayer(RemoveMapLayerParams(LAYER_ID)) }
        shown = emptySet()
    }

    private fun <T> call(block: (IOsmAndAidlInterface) -> T): T? {
        val a = api ?: return null
        return try {
            block(a)
        } catch (e: RemoteException) {
            Log.w(TAG, "OsmAnd call failed", e)
            api = null
            state = State.CONNECTING
            null
        } catch (e: RuntimeException) {
            // OsmAnd's own library can throw while packing a request; that is no lost connection
            Log.e(TAG, "OsmAnd's library failed", e)
            null
        }
    }

    private fun toAMapPoint(p: PointSpec) = AMapPoint(
        p.id, p.shortName, p.fullName, TYPE_NAME, LAYER_ID, p.color, ALatLon(p.lat, p.lon), p.details,
        mapOf(AMapPoint.POINT_STALE_LOC_PARAM to p.stale.toString()),
    )

    companion object {
        private const val TAG = "OsmAnd"
        const val ACTION = "net.osmand.aidl.OsmandAidlServiceV2"
        val PACKAGES = listOf("net.osmand.plus", "net.osmand", "net.osmand.dev")
        private const val LAYER_ID = "convoy_riders"
        private const val LAYER_NAME = "LoRa Convoy riders"
        private const val TYPE_NAME = "Rider"
        private const val Z_ORDER = 5.5f
        /** Our routes keep to a folder of their own among OsmAnd's tracks. */
        private const val TRACKS_DIR = "LoRa Convoy"
        /** One of the names OsmAnd's importGpx takes; red is a rider asking for help. */
        private const val TRACK_COLOR = "purple"
    }
}
