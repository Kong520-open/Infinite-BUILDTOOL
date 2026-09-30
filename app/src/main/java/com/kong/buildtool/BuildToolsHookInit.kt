package com.kong.buildtool

import android.app.Activity
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.util.Log
import android.view.KeyEvent
import com.vdl.kong520.NativeCore
import com.vdl.kong520.NativeLibLoader
import com.vdl.kong520.ui.BuildProjectionMaterialPreviewOverlay
import com.vdl.kong520.ui.BuildProjectionPrinterOverlay
import com.vdl.kong520.ui.ProjectionImagePicker
import de.robv.android.xposed.IXposedHookLoadPackage
import de.robv.android.xposed.XC_MethodHook
import de.robv.android.xposed.XposedBridge
import de.robv.android.xposed.XposedHelpers
import de.robv.android.xposed.callbacks.XC_LoadPackage.LoadPackageParam
import java.lang.ref.WeakReference
import java.util.concurrent.atomic.AtomicBoolean

/** In-game entry point for the building tools. */
class BuildToolsHookInit : IXposedHookLoadPackage {
    private val mapAnvilUiClosePoll = object : Runnable {
        override fun run() {
            if (!mapAnvilUiClosePollRunning.get()) return
            try {
                if (!pollMapAnvilUiClose()) pollMapChestUiClose()
            } catch (error: UnsatisfiedLinkError) {
                mapAnvilUiClosePollRunning.set(false)
                bridgeLog("automatic container UI close JNI is unavailable; polling stopped", error)
            } catch (error: Throwable) {
                bridgeLog("automatic container UI close poll failed", error)
            }
            if (mapAnvilUiClosePollRunning.get()) {
                mapAnvilUiCloseHandler?.postDelayed(this, MAP_ANVIL_CLOSE_POLL_MS)
            }
        }
    }

    override fun handleLoadPackage(lpparam: LoadPackageParam?) {
        if (lpparam == null) {
            bridgeLog("LoadPackage callback did not include parameters")
            return
        }

        // This is deliberately emitted before the Activity hook is installed.
        // Its presence proves that LSPosed discovered the module in this
        // process, even if the process metadata comes from a vendor helper.
        bridgeLog(
            "LoadPackage received; package=${lpparam.packageName}, " +
                "process=${lpparam.processName}"
        )
        // Some NetEase/MIUI combinations report a helper package here while
        // retaining the game's process name (for example package
        // com.miui.contentcatcher in process com.netease.x19). Install the
        // Activity hook first and check the concrete activity when created:
        // avoid rejecting this process based on a callback package/classloader
        // that does not describe the real game activity.
        bridgeLog("installing Activity hooks; Minecraft is identified on Activity creation")
        installActivityHooks()
    }

    private fun installActivityHooks() {
        if (!activityHooksInstalled.compareAndSet(false, true)) return
        try {
            // Activity belongs to the boot class loader. Passing its Class
            // directly is intentional: the LoadPackage classLoader can point
            // at a vendor helper package instead of the game on some devices.
            XposedHelpers.findAndHookMethod(
                Activity::class.java,
                "onCreate",
                Bundle::class.java,
                object : XC_MethodHook() {
                    override fun afterHookedMethod(param: MethodHookParam?) {
                        val activity = param?.thisObject as? Activity ?: return
                        if (activity.javaClass.name != MINECRAFT_ACTIVITY) return
                        bridgeLog("Minecraft MainActivity created; initializing UI")
                        initializeFor(activity)
                    }
                }
            )
            XposedHelpers.findAndHookMethod(
                Activity::class.java,
                "onActivityResult",
                Int::class.javaPrimitiveType,
                Int::class.javaPrimitiveType,
                Intent::class.java,
                object : XC_MethodHook() {
                    override fun afterHookedMethod(param: MethodHookParam?) {
                        val activity = param?.thisObject as? Activity ?: return
                        if (activity.javaClass.name != MINECRAFT_ACTIVITY) return
                        ProjectionImagePicker.handleActivityResult(
                            param.args[0] as Int,
                            param.args[1] as Int,
                            param.args[2] as? Intent
                        )
                    }
                }
            )
            bridgeLog("activity hooks installed")
        } catch (error: Throwable) {
            activityHooksInstalled.set(false)
            bridgeLog("unable to install building-tools activity hooks", error)
        }
    }

    private fun initializeFor(activity: Activity) {
        // Keep only a weak reference: Minecraft can recreate its Activity
        // without restarting the injected process or the native runtime.
        minecraftActivityRef = WeakReference(activity)
        if (!initialized.compareAndSet(false, true)) {
            installWatermarkWhenReady(activity)
            return
        }
        // The in-game entry must not depend on the native runtime. If a module
        // update leaves its .so temporarily unavailable, keep the visible
        // switch and let its click path show the precise loader result.
        installWatermarkWhenReady(activity)
        // Match the reference Xposed entry: native loading happens outside the
        // game's Activity.onCreate thread. JNI_OnLoad registers methods and
        // starts its own watcher, so blocking the render/UI thread here can
        // prevent the game from completing startup on some builds.
        Thread({
            try {
                if (NativeLibLoader.load(BuildToolsHookInit::class.java)) {
                    NativeCore.ensureBuildToolsHooks()
                    startMapAnvilUiClosePoll()
                    bridgeLog("native runtime initialized for " + activity.javaClass.name)
                } else {
                    initialized.set(false)
                    bridgeLog("building-tools native library was not loaded",
                        NativeLibLoader.getLastLoadError() ?: NativeCore.getNativeLoadError())
                }
            } catch (error: Throwable) {
                initialized.set(false)
                bridgeLog("unable to initialize building tools", error)
            }
        }, "BuildToolsNativeLoader").start()
    }

    private fun startMapAnvilUiClosePoll() {
        // LSPosed creates this entry class while the process is still in a
        // zygote/USAP initialization path, before a main Looper exists. Never
        // construct Handler in the companion object's static initializer.
        val mainLooper = Looper.getMainLooper()
        if (mainLooper == null) {
            bridgeLog("automatic anvil UI close is waiting for the game main Looper")
            return
        }
        if (!mapAnvilUiClosePollRunning.compareAndSet(false, true)) return
        val handler = Handler(mainLooper)
        mapAnvilUiCloseHandler = handler
        if (!handler.post(mapAnvilUiClosePoll)) {
            mapAnvilUiClosePollRunning.set(false)
            mapAnvilUiCloseHandler = null
            bridgeLog("automatic anvil UI close cannot post to the game main Looper")
        }
    }

    private fun pollMapAnvilUiClose(): Boolean {
        // Do not claim the native ticket until there is a live, focused game
        // Activity. Claiming is one-shot, so every precondition is checked
        // before calling into native code.
        val activity = minecraftActivityRef?.get() ?: return false
        if (activity.javaClass.name != MINECRAFT_ACTIVITY ||
            activity.isFinishing || activity.isDestroyed || !activity.hasWindowFocus()) return false

        val ticket = NativeCore.takePendingMapAnvilUiCloseRequest()
        if (ticket == 0L) return false
        if (activity.isFinishing || activity.isDestroyed || !activity.hasWindowFocus() ||
            !NativeCore.isMapAnvilUiCloseStillSafe(ticket)) {
            bridgeLog("anvil UI close ticket $ticket was claimed but failed the final safety check")
            return true
        }

        // The game's own Back path closes the anvil. Deliver the same down/up
        // sequence to its real Activity rather than dismissing an Android
        // dialog or sending only a server-side ContainerClose packet.
        val downTime = SystemClock.uptimeMillis()
        val downHandled = activity.dispatchKeyEvent(
            KeyEvent(downTime, downTime, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, 0)
        )
        val upHandled = activity.dispatchKeyEvent(
            KeyEvent(downTime, SystemClock.uptimeMillis(), KeyEvent.ACTION_UP,
                KeyEvent.KEYCODE_BACK, 0)
        )
        // A delivered-but-unhandled Back is not evidence that the game left
        // the anvil screen. Keep the native handoff fail-closed in that case.
        if (downHandled && upHandled) {
            NativeCore.markMapAnvilUiCloseDispatched(ticket)
        }
        bridgeLog("anvil UI Back dispatched ticket=$ticket down=$downHandled up=$upHandled")
        return true
    }

    private fun pollMapChestUiClose() {
        val activity = minecraftActivityRef?.get() ?: return
        if (activity.javaClass.name != MINECRAFT_ACTIVITY ||
            activity.isFinishing || activity.isDestroyed || !activity.hasWindowFocus()) return

        val ticket = NativeCore.takePendingMapChestUiCloseRequest()
        if (ticket == 0L) return
        if (activity.isFinishing || activity.isDestroyed || !activity.hasWindowFocus() ||
            !NativeCore.isMapChestUiCloseStillSafe(ticket)) {
            bridgeLog("chest UI close ticket $ticket was claimed but failed the final safety check")
            return
        }

        // This is the tool's exact visible chest. Back closes the game's own
        // screen and container together; a raw
        // ContainerClose packet alone left the visible screen stranded.
        val downTime = SystemClock.uptimeMillis()
        val downHandled = activity.dispatchKeyEvent(
            KeyEvent(downTime, downTime, KeyEvent.ACTION_DOWN, KeyEvent.KEYCODE_BACK, 0)
        )
        val upHandled = activity.dispatchKeyEvent(
            KeyEvent(downTime, SystemClock.uptimeMillis(), KeyEvent.ACTION_UP,
                KeyEvent.KEYCODE_BACK, 0)
        )
        if (downHandled && upHandled) {
            NativeCore.markMapChestUiCloseDispatched(ticket)
        }
        bridgeLog("chest UI Back dispatched ticket=$ticket down=$downHandled up=$upHandled")
    }

    private fun installWatermarkWhenReady(activity: Activity) {
        // Activity.onCreate can run before the final decor hierarchy is
        // attached. Posting avoids silently losing the right-bottom control on
        // game builds that replace their content view late in onCreate.
        activity.window.decorView.post {
            if (!activity.isFinishing && !activity.isDestroyed) {
                BuildToolsWatermark.install(activity)
                // A projection can survive an Activity recreation in the
                // game process. Reattach the activity-owned printer control
                // when that happens; it never uses a system overlay or XML.
                BuildProjectionPrinterOverlay.attachToHostIfNeeded(activity)
                // The complete-material window is a separate compact panel.
                // It has its own lifecycle and never replaces the printer UI.
                BuildProjectionMaterialPreviewOverlay.attachToHostIfNeeded(activity)
                bridgeLog("BUILD TOOLS watermark attached")
            }
        }
    }

    private fun bridgeLog(message: String, error: Throwable? = null) {
        val formatted = "$TAG: $message"
        Log.i(TAG, message)
        XposedBridge.log(formatted)
        if (error != null) XposedBridge.log(error)
    }

    private companion object {
        const val TAG = "BuildToolsHook"
        const val MINECRAFT_ACTIVITY = "com.mojang.minecraftpe.MainActivity"
        const val MAP_ANVIL_CLOSE_POLL_MS = 100L
        val initialized = AtomicBoolean(false)
        val activityHooksInstalled = AtomicBoolean(false)
        val mapAnvilUiClosePollRunning = AtomicBoolean(false)
        @Volatile var mapAnvilUiCloseHandler: Handler? = null
        @Volatile var minecraftActivityRef: WeakReference<Activity>? = null
    }
}
