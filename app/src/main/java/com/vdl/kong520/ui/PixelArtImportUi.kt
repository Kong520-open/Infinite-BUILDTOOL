package com.vdl.kong520.ui

import android.app.Activity
import android.content.Context

/** Independent pixel-art screen backed by the shared resumable import runtime. */
object PixelArtImportUi {
    fun show(activity: Activity, context: Context) {
        BuildImportUi.showPixelArt(activity, context)
    }
}
