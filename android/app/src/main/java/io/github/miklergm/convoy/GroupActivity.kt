package io.github.miklergm.convoy

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.text.InputType
import android.widget.EditText
import android.widget.FrameLayout
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView

/**
 * The group: join one with an invitation, or start one and send each rider theirs. The board is
 * set up from the invitation by the service, on its next status (ConvoyService).
 */
class GroupActivity : Activity() {

    private lateinit var current: TextView
    private lateinit var invitations: LinearLayout

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        val pad = (16 * resources.displayMetrics.density).toInt()
        current = TextView(this).apply { textSize = 16f; setPadding(0, pad, 0, pad) }
        val pasted = EditText(this).apply { hint = "Paste an invitation (${Invitation.PREFIX}…)"; minLines = 2 }
        val name = EditText(this).apply { hint = "Group name"; inputType = InputType.TYPE_CLASS_TEXT }
        val riders = EditText(this).apply {
            hint = "Riders' names, you first, with commas between (2 to 32)"
            inputType = InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_CAP_WORDS
        }
        invitations = LinearLayout(this).apply { orientation = LinearLayout.VERTICAL }
        val column = LinearLayout(this).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(pad, pad, pad, pad)
            addView(TextView(this@GroupActivity).apply { text = "Group"; textSize = 20f })
            addView(current)
            addView(TextView(this@GroupActivity).apply { text = "Join a group" })
            addView(pasted)
            addView(button("Join") { join(pasted.text.toString(), pasted) })
            addView(TextView(this@GroupActivity).apply { text = "\nStart a group"; })
            addView(name)
            addView(riders)
            addView(button("Create") { create(name, riders) })
            addView(invitations)
        }
        setContentView(FrameLayout(this).apply {
            fitsSystemWindows = true
            addView(ScrollView(this@GroupActivity).apply { addView(column) })
        })
        show()
    }

    private fun join(text: String, field: EditText) {
        val invitation = Invitation.decode(text)
        if (invitation == null) {
            field.error = "Not an invitation this app reads"
            return
        }
        Group.join(this, invitation)
        show()
    }

    private fun create(nameField: EditText, ridersField: EditText) {
        val name = nameField.text.toString().trim()
        val names = ridersField.text.split(',').map { it.trim() }.filter { it.isNotEmpty() }
        if (name.isEmpty() || name.toByteArray().size > Invitation.MAX_NAME) {
            nameField.error = "1 to ${Invitation.MAX_NAME} bytes"
            return
        }
        if (names.size !in 2..32 || names.any { it.toByteArray().size > Invitation.MAX_RIDER_NAME }) {
            ridersField.error = "2 to 32 names, each up to ${Invitation.MAX_RIDER_NAME} bytes"
            return
        }
        // ponytail: one radio mode and a 2 s interval for every group until group sizing (T-phy)
        val group = Invitation.newGroup(name, names, 2000)
        Group.join(this, group[0]) // the organiser rides too, first
        Group.saveCreated(this, group[0])
        show()
    }

    private fun show() {
        val mine = Group.load(this)
        current.text = mine?.let {
            "${it.name}: you are ${it.riderName(it.slot)}, rider ${it.slot + 1} of ${it.riders}, a position every " +
                "${it.intervalMs / 1000.0} s. Its boards show group=%02x.".format(it.groupTag())
        } ?: "No group yet."
        invitations.removeAllViews()
        val created = Group.created(this) ?: return
        invitations.addView(TextView(this).apply { text = "\nSend each rider their own invitation." })
        for (slot in 0 until created.riders) {
            if (created.copy(slot = slot) == mine) continue // this phone's own
            val invitation = created.copy(slot = slot).encode()
            val who = created.riderName(slot)
            invitations.addView(button("Send $who their invitation") {
                startActivity(Intent.createChooser(Intent(Intent.ACTION_SEND).apply {
                    type = "text/plain"
                    putExtra(Intent.EXTRA_TEXT, invitation)
                }, "Invitation for $who"))
            })
        }
    }

    /** The group this phone is in, and the one it created, kept in its private preferences. */
    object Group {
        private const val PREFS = "group"

        fun load(context: Context): Invitation? =
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString("joined", null)?.let(Invitation::decode)

        fun join(context: Context, invitation: Invitation) {
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString("joined", invitation.encode()).apply()
        }

        fun created(context: Context): Invitation? =
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).getString("created", null)?.let(Invitation::decode)

        fun saveCreated(context: Context, invitation: Invitation) {
            context.getSharedPreferences(PREFS, Context.MODE_PRIVATE).edit().putString("created", invitation.encode()).apply()
        }
    }
}
