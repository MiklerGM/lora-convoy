package io.github.miklergm.convoy

import org.xml.sax.Attributes
import org.xml.sax.InputSource
import org.xml.sax.helpers.DefaultHandler
import java.io.ByteArrayInputStream
import java.io.StringReader
import java.security.MessageDigest
import javax.xml.parsers.SAXParserFactory
import kotlin.math.asin
import kotlin.math.cos
import kotlin.math.sin
import kotlin.math.sqrt

/** What a GPX holds, read before anything else trusts it. */
data class GpxSummary(
    val name: String?,
    val trackPoints: Int,
    val routePoints: Int,
    val waypoints: Int,
    val lengthM: Double,
)

/**
 * GPX files arrive from messengers and file managers: untrusted, possibly large, possibly not GPX
 * at all. This reads one once, with no DTD and no external entities, and refuses anything whose
 * root is not `gpx`.
 */
object Gpx {

    /** The owner's multi-day route is 4 MB and the Trans-Morocco Trail 6.9 MB (06). */
    const val MAX_BYTES = 20 * 1024 * 1024

    fun sha256(bytes: ByteArray): String =
        MessageDigest.getInstance("SHA-256").digest(bytes).joinToString("") { "%02x".format(it) }

    /** Throws IllegalArgumentException for anything that is not a readable GPX. */
    fun summarize(bytes: ByteArray): GpxSummary {
        require(bytes.size <= MAX_BYTES) { "larger than ${MAX_BYTES / 1024 / 1024} MB" }
        val factory = SAXParserFactory.newInstance().apply { isNamespaceAware = true }
        // Not every parser knows every feature (Android's does not), hence one by one; the
        // resolver below covers external entities on any parser.
        for ((feature, on) in listOf(
            "http://apache.org/xml/features/disallow-doctype-decl" to true,
            "http://xml.org/sax/features/external-general-entities" to false,
            "http://xml.org/sax/features/external-parameter-entities" to false,
        )) {
            try {
                factory.setFeature(feature, on)
            } catch (e: Exception) {
                // unsupported here
            }
        }
        val handler = Handler()
        try {
            val reader = factory.newSAXParser().xmlReader
            reader.setEntityResolver { _, _ -> InputSource(StringReader("")) }
            reader.contentHandler = handler
            reader.parse(InputSource(ByteArrayInputStream(bytes)))
        } catch (e: IllegalArgumentException) {
            throw e
        } catch (e: Exception) {
            throw IllegalArgumentException("not readable as GPX: ${e.message}", e)
        }
        require(handler.root == "gpx") { "not a GPX file" }
        return GpxSummary(handler.name?.trim()?.ifEmpty { null }, handler.trkpt, handler.rtept, handler.wpt, handler.lengthM)
    }

    /** A file name OsmAnd can keep: the route's name, if it has one, then the start of its hash. */
    fun fileName(name: String?, sha: String): String {
        val safe = name.orEmpty().replace(Regex("[^\\p{L}\\p{N} _-]"), "").trim().take(40)
        return "${safe.ifEmpty { "route" }}-${sha.take(8)}.gpx"
    }

    private class Handler : DefaultHandler() {
        var root: String? = null
        var name: String? = null
        var trkpt = 0
        var rtept = 0
        var wpt = 0
        var lengthM = 0.0

        private val path = ArrayDeque<String>()
        private var text: StringBuilder? = null
        private var last: Pair<Double, Double>? = null

        override fun startElement(uri: String?, localName: String, qName: String?, attrs: Attributes) {
            if (root == null) root = localName
            if (localName == "trkseg" || localName == "rte") last = null
            if (localName == "trkpt" || localName == "rtept" || localName == "wpt") {
                val lat = attrs.getValue("lat")?.toDoubleOrNull()
                val lon = attrs.getValue("lon")?.toDoubleOrNull()
                if (lat != null && lon != null && lat in -90.0..90.0 && lon in -180.0..180.0) {
                    when (localName) {
                        "trkpt" -> trkpt++
                        "rtept" -> rtept++
                        else -> wpt++
                    }
                    if (localName != "wpt") {
                        last?.let { lengthM += metres(it.first, it.second, lat, lon) }
                        last = lat to lon
                    }
                }
            }
            // the route's own name: the file's, else the first track's or route's
            if (localName == "name" && name == null && path.lastOrNull() in setOf("metadata", "trk", "rte")) {
                text = StringBuilder()
            }
            path.addLast(localName)
        }

        override fun characters(ch: CharArray, start: Int, length: Int) {
            text?.appendRange(ch, start, start + length)
        }

        override fun endElement(uri: String?, localName: String, qName: String?) {
            path.removeLastOrNull()
            if (localName == "name") text?.let { name = it.toString(); text = null }
        }
    }

    /** Great-circle distance; a GPX's own error dwarfs the sphere's. */
    fun metres(lat1: Double, lon1: Double, lat2: Double, lon2: Double): Double {
        val p1 = Math.toRadians(lat1)
        val p2 = Math.toRadians(lat2)
        val dp = p2 - p1
        val dl = Math.toRadians(lon2 - lon1)
        val h = sin(dp / 2) * sin(dp / 2) + cos(p1) * cos(p2) * sin(dl / 2) * sin(dl / 2)
        return 2 * 6_371_000.0 * asin(sqrt(h))
    }
}
