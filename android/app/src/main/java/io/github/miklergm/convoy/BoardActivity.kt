package io.github.miklergm.convoy

import android.app.Activity
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/**
 * The board's settings as its status tells them, each changed by one command (docs/PROTOCOL.md).
 * The board keeps them over a restart, as it keeps its group, so it rides as set with no phone.
 * The radio mode is the group's, the same on every board, so it is not here.
 */
class BoardActivity : Activity() {

    private val handler = Handler(Looper.getMainLooper())
    private lateinit var info: TextView
    private lateinit var answer: TextView
    /** Each setting's line, and how it reads from a status. */
    private val shown = mutableListOf<Pair<TextView, (BoardLines.Status) -> String>>()

    private val refresh = object : Runnable {
        override fun run() {
            show()
            handler.postDelayed(this, 1000)
        }
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        info = TextView(this).apply { textSize = 16f; setPadding(0, pad, 0, pad) }
        answer = TextView(this).apply { setPadding(0, pad, 0, pad) }
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
            addView(info)
            setting("POWER", 0..3, "Transmit power, 0 the strongest") { "${it.power ?: "?"}, ${it.dbm ?: "?"} dBm" }
            setting("MOVE", 1..1000, "A new position after moving, m") { "${it.moveM ?: "?"} m" }
            setting("BEAT", 0..7, "Standing still, sends every 2^n frames") { s -> s.beat?.let { heartbeat(it) } ?: "?" }
            setting("PASSENGERS", 0..3, "Other riders carried in each packet, for those out of reach") {
                "${it.passengers ?: "?"}"
            }
            addView(answer)
        }
        setContentView(FrameLayout(this).apply {
            fitsSystemWindows = true
            addView(ScrollView(this@BoardActivity).apply { addView(column) })
        })
    }

    override fun onResume() {
        super.onResume()
        handler.post(refresh)
    }

    override fun onPause() {
        handler.removeCallbacks(refresh)
        super.onPause()
    }

    /** A setting: what it is now, a field for a new value, and the button that sends it. */
    private fun LinearLayout.setting(command: String, range: IntRange, title: String, now: (BoardLines.Status) -> String) {
        val value = TextView(this@BoardActivity).apply { textSize = 16f }
        shown.add(value to { s -> "$title: ${now(s)}" })
        val field = EditText(this@BoardActivity).apply {
            inputType = InputType.TYPE_CLASS_NUMBER
            hint = "${range.first} to ${range.last}"
        }
        addView(value)
        addView(LinearLayout(this@BoardActivity).apply {
            addView(field, LinearLayout.LayoutParams(0, LinearLayout.LayoutParams.WRAP_CONTENT, 1f))
            addView(button("Set") {
                val v = field.text.toString().toIntOrNull()
                when {
                    v == null || v !in range -> field.error = "${range.first} to ${range.last}"
                    !ConvoyService.command(this@BoardActivity, "$command $v") -> answer.text = "No board connected."
                    else -> field.text.clear()
                }
            })
        })
    }

    /** 2^beat frames, in seconds where the group's interval is known. */
    private fun heartbeat(beat: Int): String {
        val frames = 1 shl beat
        val intervalMs = (ConvoyService.confirmed ?: GroupActivity.Group.load(this))?.intervalMs
            ?: return "$beat, every $frames frames"
        return "$beat, every ${frames * intervalMs / 1000.0} s"
    }

    private fun show() {
        val s = ConvoyService.board
        info.text = s?.let {
            "${it.board}, rider ${it.slot + 1}${it.slots?.let { n -> " of $n" } ?: ""}, group %08x, mode ${it.mode ?: "?"}"
                .format(it.group)
        } ?: "No board connected."
        shown.forEach { (view, read) -> view.text = s?.let(read) ?: "" }
        ConvoyService.reply?.let { answer.text = "The board said: $it" }
    }
}
