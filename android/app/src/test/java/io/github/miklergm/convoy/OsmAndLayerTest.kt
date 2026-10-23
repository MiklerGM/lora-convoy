package io.github.miklergm.convoy

import java.lang.reflect.Proxy
import net.osmand.aidlapi.IOsmAndAidlInterface
import net.osmand.aidlapi.map.ALatLon
import net.osmand.aidlapi.maplayer.AddMapLayerParams
import net.osmand.aidlapi.maplayer.RemoveMapLayerParams
import net.osmand.aidlapi.maplayer.UpdateMapLayerParams
import net.osmand.aidlapi.maplayer.point.AMapPoint
import net.osmand.aidlapi.maplayer.point.RemoveMapPointParams
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class OsmAndLayerTest {

    /**
     * OsmAnd's side, as its AIDL API 5.4 keeps layers (ConnectedApp): an update adds and moves
     * points, and never takes one away; and so does adding a layer that is there already.
     */
    private val layers = mutableMapOf<String, MutableMap<String, AMapPoint>>()
    private val osmand = Proxy.newProxyInstance(javaClass.classLoader, arrayOf(IOsmAndAidlInterface::class.java)) { _, m, args ->
        when (m.name) {
            "addMapLayer" -> (args[0] as AddMapLayerParams).layer.let { l ->
                layers.getOrPut(l.id) { mutableMapOf() }.putAll(l.points.associateBy { it.id })
                true
            }
            "updateMapLayer" -> (args[0] as UpdateMapLayerParams).layer.let { l -> layers[l.id]?.putAll(l.points.associateBy { it.id }) } != null
            "removeMapLayer" -> layers.remove((args[0] as RemoveMapLayerParams).id) != null
            "removeMapPoint" -> (args[0] as RemoveMapPointParams).let { layers[it.layerId]?.remove(it.pointId) } != null
            else -> throw UnsupportedOperationException(m.name)
        }
    } as IOsmAndAidlInterface

    private fun point(id: String) = AMapPoint(id, id, id, "Rider", "", 0, ALatLon(47.0, 9.0), emptyList(), emptyMap())
    private fun onMap() = layers.values.single().keys

    @Test
    fun `later shows move the points, and take away those gone`() {
        assertTrue(OsmAnd.putLayer(osmand, listOf(point("slot1"), point("slot2")), null))
        assertTrue(OsmAnd.putLayer(osmand, listOf(point("slot2"), point("slot3")), setOf("slot1", "slot2")))
        assertEquals(setOf("slot2", "slot3"), onMap())
    }

    @Test
    fun `a new service leaves none of the points from before it`() {
        // the service before: then its process died, OsmAnd lived on, and the board came back knowing no one
        OsmAnd.putLayer(osmand, listOf(point("slot1"), point("slot2")), null)
        assertTrue(OsmAnd.putLayer(osmand, emptyList(), null))
        assertEquals(emptySet<String>(), onMap())
    }
}
