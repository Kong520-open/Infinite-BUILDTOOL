package com.vdl.kong520.ui

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.net.Uri
import android.provider.OpenableColumns
import android.os.Handler
import android.os.Looper
import java.io.File
import java.io.FileOutputStream
import java.io.IOException
import java.util.Locale

/**
 * Imports a PNG/JPEG selected through the platform picker into private storage.
 * Native projection parsing works with filesystem paths, so content URIs cannot
 * be passed through directly. JPEG conversion is deliberately delegated to
 * TpModule.preparePixelArtSource(), which already handles EXIF orientation and
 * bounded decode memory.
 */
object ProjectionImagePicker {
    private const val REQUEST_PICK_PIXEL_ART_IMAGE = 0x5058
    private const val MAX_IMPORTED_BYTES = 128L * 1024L * 1024L

    private val pickerLock = Any()
    private var pendingSelection: PendingSelection? = null

    private data class PendingSelection(
        val activity: Activity,
        val context: Context,
        val onSelected: (File) -> Unit,
        val onError: (String) -> Unit
    )

    @JvmStatic
    fun launch(
        activity: Activity,
        context: Context,
        onSelected: (File) -> Unit,
        onError: (String) -> Unit
    ) {
        val selection = PendingSelection(activity, context.applicationContext, onSelected, onError)
        synchronized(pickerLock) {
            if (pendingSelection != null) {
                onError("图片选择器已经打开")
                return
            }
            pendingSelection = selection
        }

        val intent = Intent(Intent.ACTION_OPEN_DOCUMENT).apply {
            addCategory(Intent.CATEGORY_OPENABLE)
            type = "image/*"
            putExtra(Intent.EXTRA_MIME_TYPES, arrayOf("image/png", "image/jpeg"))
            addFlags(Intent.FLAG_GRANT_READ_URI_PERMISSION)
        }
        try {
            activity.startActivityForResult(intent, REQUEST_PICK_PIXEL_ART_IMAGE)
        } catch (error: Throwable) {
            synchronized(pickerLock) {
                if (pendingSelection === selection) pendingSelection = null
            }
            onError(error.message?.takeIf { it.isNotBlank() } ?: "无法打开系统图片选择器")
        }
    }

    /** Called by [com.vdl.kong520.MainActivity.onActivityResult]. */
    @JvmStatic
    fun handleActivityResult(requestCode: Int, resultCode: Int, data: Intent?): Boolean {
        if (requestCode != REQUEST_PICK_PIXEL_ART_IMAGE) return false
        val selection = synchronized(pickerLock) {
            pendingSelection.also { pendingSelection = null }
        } ?: return true
        if (resultCode != Activity.RESULT_OK) return true
        val uri = data?.data
        if (uri == null) {
            selection.onError("未收到所选图片")
            return true
        }

        Thread({
            val result = runCatching { importImage(selection.context, uri) }
            Handler(Looper.getMainLooper()).post {
                if (selection.activity.isFinishing || selection.activity.isDestroyed) return@post
                result.onSuccess(selection.onSelected)
                    .onFailure { error ->
                        selection.onError(
                            error.message?.takeIf { it.isNotBlank() } ?: "无法读取所选图片"
                        )
                    }
            }
        }, "projection-image-import").start()
        return true
    }

    @JvmStatic
    fun cancel(activity: Activity) {
        synchronized(pickerLock) {
            if (pendingSelection?.activity === activity) pendingSelection = null
        }
    }

    @Throws(IOException::class)
    private fun importImage(context: Context, uri: Uri): File {
        val resolver = context.contentResolver
        val extension = selectedImageExtension(resolver.getType(uri), queryDisplayName(context, uri))
            ?: throw IOException("请选择 PNG 或 JPG 图片")
        val directory = File(context.filesDir, "build_projection/picked_images")
        if (!directory.isDirectory && !directory.mkdirs() && !directory.isDirectory) {
            throw IOException("无法创建图片缓存目录")
        }
        val stem = "pixel-art-${System.currentTimeMillis()}-${System.nanoTime()}"
        val target = File(directory, "$stem.$extension")
        val temporary = File(directory, "$stem.part")
        try {
            resolver.openInputStream(uri)?.use { input ->
                FileOutputStream(temporary).use { output ->
                    val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                    var total = 0L
                    while (true) {
                        val count = input.read(buffer)
                        if (count < 0) break
                        total += count.toLong()
                        if (total > MAX_IMPORTED_BYTES) {
                            throw IOException("图片超过 128 MB，无法导入")
                        }
                        output.write(buffer, 0, count)
                    }
                }
            } ?: throw IOException("无法打开所选图片")
            if (!temporary.renameTo(target)) throw IOException("无法保存所选图片")
            return target
        } finally {
            if (temporary.exists()) temporary.delete()
        }
    }

    private fun queryDisplayName(context: Context, uri: Uri): String? = runCatching {
        context.contentResolver.query(uri, arrayOf(OpenableColumns.DISPLAY_NAME), null, null, null)?.use { cursor ->
            if (cursor.moveToFirst()) cursor.getString(0) else null
        }
    }.getOrNull()

    private fun selectedImageExtension(mimeType: String?, displayName: String?): String? {
        val nameExtension = displayName?.substringAfterLast('.', "")?.lowercase(Locale.ROOT)
        if (nameExtension in setOf("png", "jpg", "jpeg")) return nameExtension
        return when (mimeType?.lowercase(Locale.ROOT)) {
            "image/png" -> "png"
            "image/jpeg", "image/jpg" -> "jpg"
            else -> null
        }
    }
}
