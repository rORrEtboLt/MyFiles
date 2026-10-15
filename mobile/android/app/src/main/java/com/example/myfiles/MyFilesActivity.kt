package com.example.myfiles

import android.app.Activity
import android.app.AlertDialog
import android.content.Intent
import android.graphics.BitmapFactory
import android.net.Uri
import androidx.core.content.FileProvider
import com.google.android.gms.tasks.Tasks
import com.google.mlkit.vision.barcode.common.Barcode
import com.google.mlkit.vision.codescanner.GmsBarcodeScannerOptions
import com.google.mlkit.vision.codescanner.GmsBarcodeScanning
import com.google.mlkit.vision.common.InputImage
import com.google.mlkit.vision.documentscanner.GmsDocumentScannerOptions
import com.google.mlkit.vision.documentscanner.GmsDocumentScanning
import com.google.mlkit.vision.documentscanner.GmsDocumentScanningResult
import com.google.mlkit.vision.text.TextRecognition
import com.google.mlkit.vision.text.latin.TextRecognizerOptions
import org.libsdl.app.SDLActivity
import java.io.File
import java.io.FileOutputStream
import java.net.HttpURLConnection
import java.net.URL
import kotlin.concurrent.thread

/**
 * Hosts the SDL app and provides what SDL lacks on Android, called from C over JNI
 * (src/platform/platform_android.c):
 *  - document scanning with edge detection: ML Kit document scanner (Google Play services UI)
 *  - on-device OCR: ML Kit text recognition
 *  - QR labels: ML Kit code scanner (needs no camera permission)
 *  - share/print a label, blocking HTTP for the sync thread
 */
class MyFilesActivity : SDLActivity() {

    // SDL, SDL_ttf and SDL_image are linked statically into libmain.so.
    override fun getLibraries(): Array<String> = arrayOf("main")

    @Deprecated("Activity result API is not available on SDLActivity (a plain Activity)")
    override fun onActivityResult(requestCode: Int, resultCode: Int, data: Intent?) {
        super.onActivityResult(requestCode, resultCode, data)
        if (requestCode != REQ_SCAN_DOCUMENT && requestCode != REQ_SCAN_LABEL) return
        val label = requestCode == REQ_SCAN_LABEL
        if (resultCode != Activity.RESULT_OK) {
            if (label) nativeLabelDone(null, false, true, null) else nativeScanDone(null, null, null, null, true, null)
            return
        }
        val result = GmsDocumentScanningResult.fromActivityResultIntent(data)
        val uris = result?.pages?.map { it.imageUri } ?: emptyList()
        thread(name = "scan") { if (label) finishLabelPhoto(uris.firstOrNull()) else finishDocument(uris) }
    }

    private fun finishDocument(uris: List<Uri>) {
        val paths = ArrayList<String>()
        val widths = ArrayList<Int>()
        val heights = ArrayList<Int>()
        val texts = ArrayList<String>()
        var error: String? = null
        for (uri in uris) {
            try {
                val out = File(tmpDir, "scan-${System.nanoTime()}.jpg")
                contentResolver.openInputStream(uri)!!.use { input -> FileOutputStream(out).use { input.copyTo(it) } }
                val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
                BitmapFactory.decodeFile(out.path, bounds)
                paths += out.path
                widths += bounds.outWidth
                heights += bounds.outHeight
                texts += ocr(InputImage.fromFilePath(this, Uri.fromFile(out)))
            } catch (e: Exception) {
                error = e.message ?: "Could not read a scanned page"
            }
        }
        nativeScanDone(paths.toTypedArray(), widths.toIntArray(), heights.toIntArray(), texts.toTypedArray(), false, error)
    }

    private fun finishLabelPhoto(uri: Uri?) {
        if (uri == null) {
            nativeLabelDone(null, false, true, null)
            return
        }
        try {
            nativeLabelDone(ocr(InputImage.fromFilePath(this, uri)), false, false, null)
        } catch (e: Exception) {
            nativeLabelDone(null, false, false, e.message ?: "Could not read the label")
        }
    }

    private fun ocr(image: InputImage): String {
        val recognizer = TextRecognition.getClient(TextRecognizerOptions.DEFAULT_OPTIONS)
        return try {
            Tasks.await(recognizer.process(image)).textBlocks.joinToString("\n") { it.text }
        } finally {
            recognizer.close()
        }
    }

    private fun startDocumentScanner(pageLimit: Int, requestCode: Int) {
        val options = GmsDocumentScannerOptions.Builder()
            .setGalleryImportAllowed(true)
            .setPageLimit(pageLimit)
            .setResultFormats(GmsDocumentScannerOptions.RESULT_FORMAT_JPEG)
            .setScannerMode(GmsDocumentScannerOptions.SCANNER_MODE_FULL)
            .build()
        GmsDocumentScanning.getClient(options).getStartScanIntent(this)
            .addOnSuccessListener { sender -> startIntentSenderForResult(sender, requestCode, null, 0, 0, 0) }
            .addOnFailureListener { e ->
                if (requestCode == REQ_SCAN_LABEL) nativeLabelDone(null, false, false, e.message)
                else nativeScanDone(null, null, null, null, false, e.message ?: "Document scanner unavailable")
            }
    }

    private fun scanQr() {
        val options = GmsBarcodeScannerOptions.Builder().setBarcodeFormats(Barcode.FORMAT_QR_CODE).build()
        GmsBarcodeScanning.getClient(this, options).startScan()
            .addOnSuccessListener { code -> nativeLabelDone(code.rawValue ?: "", true, false, null) }
            .addOnCanceledListener { nativeLabelDone(null, false, true, null) }
            .addOnFailureListener { e -> nativeLabelDone(null, false, false, e.message ?: "QR scanner unavailable") }
    }

    companion object {
        private const val REQ_SCAN_DOCUMENT = 4101
        private const val REQ_SCAN_LABEL = 4102
        private var tmpDir: String = ""

        private val activity: MyFilesActivity get() = mSingleton as MyFilesActivity

        @JvmStatic fun init(tmp: String) {
            tmpDir = tmp
        }

        @JvmStatic fun scanDocument() {
            activity.runOnUiThread { activity.startDocumentScanner(20, REQ_SCAN_DOCUMENT) }
        }

        /** QR label, or a photo of a handwritten number that is read with OCR. */
        @JvmStatic fun scanLabel() {
            val a = activity
            a.runOnUiThread {
                AlertDialog.Builder(a)
                    .setTitle("Find a file")
                    .setItems(arrayOf("Scan QR label", "Photograph handwritten number")) { _, which ->
                        if (which == 0) a.scanQr() else a.startDocumentScanner(1, REQ_SCAN_LABEL)
                    }
                    .setOnCancelListener { nativeLabelDone(null, false, true, null) }
                    .show()
            }
        }

        @JvmStatic fun shareFile(path: String, mime: String) {
            val a = activity
            a.runOnUiThread {
                val uri = FileProvider.getUriForFile(a, "com.example.myfiles.files", File(path))
                val send = Intent(Intent.ACTION_SEND).apply {
                    type = mime
                    putExtra(Intent.EXTRA_STREAM, uri)
                    addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
                }
                a.startActivity(Intent.createChooser(send, "Share or print label"))
            }
        }

        /** Blocking HTTP for the native sync thread. Returns null on network failure. */
        @JvmStatic fun http(method: String, url: String, bearer: String?, contentType: String?, body: ByteArray?): HttpResult? =
            try {
                val c = URL(url).openConnection() as HttpURLConnection
                c.requestMethod = method
                c.connectTimeout = 10_000
                c.readTimeout = 120_000
                bearer?.let { c.setRequestProperty("Authorization", "Bearer $it") }
                contentType?.let { c.setRequestProperty("Content-Type", it) }
                if (body != null) {
                    c.doOutput = true
                    c.setFixedLengthStreamingMode(body.size)
                    c.outputStream.use { it.write(body) }
                }
                val status = c.responseCode
                val stream = if (status >= 400) c.errorStream else c.inputStream
                val bytes = stream?.use { it.readBytes() } ?: ByteArray(0)
                c.disconnect()
                HttpResult(status, bytes)
            } catch (e: Exception) {
                null
            }
    }
}

class HttpResult(@JvmField val status: Int, @JvmField val body: ByteArray)
