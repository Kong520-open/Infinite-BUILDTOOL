package com.kong.buildtool

import android.app.Activity
import android.widget.Toast
import com.vdl.kong520.NativeCore
import com.vdl.kong520.NativeLibLoader
import com.vdl.kong520.ui.BuildExportUi
import com.vdl.kong520.ui.BuildImportUi
import com.vdl.kong520.ui.BuildProjectionUi
import com.vdl.kong520.ui.PixelArtImportUi

/** Entry point for the building tools in the injected game process. */
object BuildToolsLauncher {
    enum class Tool { IMPORT, PIXEL_ART, PROJECTION, EXPORT }

    fun openPanel(activity: Activity) {
        openWhenReady(activity) { BuildToolsPanelPopup.show(activity) }
    }

    fun open(activity: Activity, tool: Tool) {
        openWhenReady(activity) { showTool(activity, tool) }
    }

    private fun openWhenReady(activity: Activity, show: () -> Unit) {
        if (activity.isFinishing || activity.isDestroyed) return
        Thread({
            val ready = NativeLibLoader.load(BuildToolsHookInit::class.java) &&
                runCatching { NativeCore.ensureBuildToolsHooks() }.isSuccess
            activity.runOnUiThread {
                if (activity.isFinishing || activity.isDestroyed) return@runOnUiThread
                if (ready) show() else Toast.makeText(
                    activity.applicationContext,
                    "建筑工具原生运行库未能加载",
                    Toast.LENGTH_LONG
                ).show()
            }
        }, "build-tools-open").start()
    }

    private fun showTool(activity: Activity, tool: Tool) {
        when (tool) {
            Tool.IMPORT -> BuildImportUi.show(activity, activity.applicationContext)
            Tool.PIXEL_ART -> PixelArtImportUi.show(activity, activity.applicationContext)
            Tool.PROJECTION -> BuildProjectionUi.show(activity, activity.applicationContext)
            Tool.EXPORT -> BuildExportUi.show(activity, activity.applicationContext)
        }
    }
}
