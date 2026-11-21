package io.github.miklergm.convoy

import android.Manifest
import android.app.Activity
import android.content.Intent
import android.content.pm.PackageManager
import android.os.Bundle
import android.widget.FrameLayout
import com.google.zxing.BarcodeFormat
import com.journeyapps.barcodescanner.DecoratedBarcodeView
import com.journeyapps.barcodescanner.DefaultDecoderFactory

/**
 * The camera, until it reads a QR code; its text comes back as EXTRA_TEXT. ZXing's own view on a
 * screen of ours: the library's CaptureActivity asks for the camera through AndroidX, which this
 * app does without.
 */
class ScanActivity : Activity() {

    private lateinit var view: DecoratedBarcodeView

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        view = DecoratedBarcodeView(this).apply {
            decoderFactory = DefaultDecoderFactory(listOf(BarcodeFormat.QR_CODE))
            setStatusText(intent.getStringExtra(EXTRA_PROMPT) ?: "")
            decodeSingle {
                setResult(RESULT_OK, Intent().putExtra(EXTRA_TEXT, it.text))
                finish()
            }
        }
        // clear of the system bars, which Android 15 draws over the app
        setContentView(FrameLayout(this).apply {
            fitsSystemWindows = true
            addView(view)
        })
        if (!allowed()) requestPermissions(arrayOf(Manifest.permission.CAMERA), 1)
    }

    private fun allowed() = checkSelfPermission(Manifest.permission.CAMERA) == PackageManager.PERMISSION_GRANTED

    override fun onResume() {
        super.onResume()
        if (allowed()) view.resume()
    }

    override fun onPause() {
        view.pause()
        super.onPause()
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        if (allowed()) view.resume() else finish() // refused: back, where pasting still works
    }

    companion object {
        const val EXTRA_TEXT = "text"
        const val EXTRA_PROMPT = "prompt"
    }
}
