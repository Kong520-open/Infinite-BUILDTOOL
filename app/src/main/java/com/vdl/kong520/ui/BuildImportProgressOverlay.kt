package com.vdl.kong520.ui

import android.R.attr.progressBarStyleHorizontal
import android.app.Activity
import android.app.Dialog
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.os.Handler
import android.os.Looper
import android.os.SystemClock
import android.view.Gravity
import android.view.ViewGroup
import android.view.Window
import android.view.WindowManager
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import com.vdl.kong520.TpModule
import java.lang.ref.WeakReference
import java.util.Locale

/** A non-interactive in-game progress window. FLAG_NOT_TOUCHABLE lets all touches reach the game. */
object BuildImportProgressOverlay {
    private val handler = Handler(Looper.getMainLooper())
    private var dialog: Dialog? = null
    private var ownerActivityRef: WeakReference<Activity>? = null
    private var ticker: Runnable? = null
    private var lastSourceFileName = "未知文件"
    private var activeSessionId: Long? = null
    private var stateTracker: BuildImportOverlayStateTracker? = null
    private var pendingTerminalSessionId: Long? = null
    private var pendingTerminalState: Int? = null

    fun show(
        activity: Activity,
        sourceFileName: String? = null,
        eyebrow: String = "BUILD IMPORT",
        sessionId: Long? = null
    ) {
        sourceFileName?.trim()?.takeIf { it.isNotEmpty() }?.let { lastSourceFileName = it }
        if (activity.isFinishing || activity.isDestroyed) return
        val currentDialog = dialog
        if (currentDialog?.isShowing == true) {
            val currentOwner = ownerActivityRef?.get()
            if (currentOwner === activity &&
                !activity.isFinishing && !activity.isDestroyed &&
                activeSessionId == sessionId) {
                return
            }
        }
        if (currentDialog != null) hide()

        val density = activity.resources.displayMetrics.density
        fun dp(value: Float) = (value * density).toInt()
        val overlay = Dialog(activity).apply {
            requestWindowFeature(Window.FEATURE_NO_TITLE)
            setCancelable(false)
        }
        overlay.setOnDismissListener {
            if (dialog === overlay) {
                ticker?.let(handler::removeCallbacks)
                ticker = null
                dialog = null
                ownerActivityRef = null
                activeSessionId = null
                stateTracker = null
            }
        }
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(13f), dp(11f), dp(13f), dp(11f))
            background = rounded("#F5FFFEFA", dp(13f), "#E7E0D7")
        }
        val title = TextView(activity).apply {
            text = if (eyebrow.contains("IMAGE", ignoreCase = true)) "图片导入进度" else "建筑导入进度"
            textSize = 11f
            typeface = Typeface.DEFAULT_BOLD
            letterSpacing = 0.02f
            setTextColor(Color.parseColor("#AA4E31"))
        }
        val state = TextView(activity).apply {
            textSize = 12f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor("#302D29"))
            setPadding(0, dp(5f), 0, 0)
        }
        val elapsed = TextView(activity).apply {
            textSize = 10f
            typeface = Typeface.DEFAULT_BOLD
            gravity = Gravity.END or Gravity.CENTER_VERTICAL
            setTextColor(Color.parseColor("#AA4E31"))
            setPadding(dp(8f), dp(5f), 0, 0)
            visibility = TextView.GONE
        }
        val stateRow = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        stateRow.addView(state, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        stateRow.addView(elapsed, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
        val progress = ProgressBar(activity, null, progressBarStyleHorizontal).apply {
            max = 100
            progressTintList = ColorStateList.valueOf(Color.parseColor("#CF6846"))
            progressBackgroundTintList = ColorStateList.valueOf(Color.parseColor("#EDE8E0"))
        }
        val count = TextView(activity).apply {
            textSize = 10f
            setTextColor(Color.parseColor("#77716A"))
            setPadding(0, dp(8f), 0, 0)
        }
        val detail = TextView(activity).apply {
            textSize = 10f
            setTextColor(Color.parseColor("#77716A"))
            setPadding(0, dp(3f), 0, 0)
        }
        root.addView(title)
        root.addView(stateRow)
        root.addView(progress, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(6f)).apply { topMargin = dp(8f) })
        root.addView(count)
        root.addView(detail)
        overlay.setContentView(root)
        overlay.show()
        overlay.window?.let { window ->
            window.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            window.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
            window.addFlags(
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
                    WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
            )
            window.setGravity(Gravity.TOP or Gravity.END)
            window.attributes = window.attributes.apply {
                x = dp(16f)
                y = dp(58f)
            }
            window.setLayout(dp(240f), ViewGroup.LayoutParams.WRAP_CONTENT)
        }
        dialog = overlay
        ownerActivityRef = WeakReference(activity)
        activeSessionId = sessionId
        stateTracker = BuildImportOverlayStateTracker().also { tracker ->
            if (sessionId != null && pendingTerminalSessionId == sessionId) {
                pendingTerminalState?.let(tracker::latchTerminal)
                pendingTerminalSessionId = null
                pendingTerminalState = null
            }
        }
        var importingStartedAtMs = 0L
        var accumulatedImportMs = 0L

        val update = object : Runnable {
            override fun run() {
                if (dialog !== overlay || !overlay.isShowing) return
                val owner = ownerActivityRef?.get()
                if (owner == null || owner !== activity ||
                    owner.isFinishing || owner.isDestroyed) {
                    hide()
                    return
                }
                val observedTotal = runCatching { TpModule.getBuildImportTotalBlocks() }
                    .getOrDefault(0L)
                val observedImported = runCatching { TpModule.getBuildImportImportedBlocks() }
                    .getOrDefault(0L)
                val observedState = runCatching { TpModule.getBuildImportState() }
                    .getOrDefault(-1)
                val presentation = stateTracker ?: return
                val snapshot = presentation.update(observedState, observedTotal, observedImported)
                val total = snapshot.total
                val imported = snapshot.imported
                val percent = if (total > 0) ((imported * 100L) / total).toInt() else 0
                val importState = snapshot.state
                val nowMs = SystemClock.elapsedRealtime()
                if (observedState == 2) {
                    if (importingStartedAtMs == 0L) importingStartedAtMs = nowMs
                } else if (importingStartedAtMs != 0L) {
                    accumulatedImportMs += (nowMs - importingStartedAtMs).coerceAtLeast(0L)
                    importingStartedAtMs = 0L
                }
                val elapsedMs = accumulatedImportMs + if (importingStartedAtMs != 0L) {
                        (nowMs - importingStartedAtMs).coerceAtLeast(0L)
                    } else {
                        0L
                    }
                if (accumulatedImportMs > 0L || importingStartedAtMs != 0L) {
                    val elapsedSeconds = (elapsedMs / 1000L).coerceAtLeast(0L)
                    elapsed.text = "耗时 ${formatElapsed(elapsedSeconds)}"
                    elapsed.visibility = TextView.VISIBLE
                } else {
                    elapsed.visibility = TextView.GONE
                }
                if (importState == 5) {
                    state.text = "导入完成"
                    elapsed.visibility = TextView.GONE
                    progress.visibility = ProgressBar.GONE
                    count.text = "文件名：$lastSourceFileName"
                    val averageSpeed = imported.toDouble() * 1000.0 / elapsedMs.coerceAtLeast(1L)
                    detail.text = "总耗时：${formatElapsed(elapsedMs / 1000L)}\n" +
                        "方块数：${formatCount(imported)}\n" +
                        "平均速度：${formatSpeed(averageSpeed)} 方块/秒"
                    handler.postDelayed({ if (dialog === overlay) hide() }, 5000)
                    return
                }

                progress.visibility = ProgressBar.VISIBLE
                progress.progress = percent
                state.text = stateLabel(importState)
                count.text = "已导入 ${formatCount(imported)} / ${formatCount(total)} 方块  ·  $percent%"
                val nativeStatus = runCatching { TpModule.getBuildImportStatus() }
                    .getOrDefault("")
                detail.text = conciseImportOverlayStatus(importState, nativeStatus)
                if (importState == 6 || importState == IMPORT_OVERLAY_ENDED_STATE) {
                    handler.postDelayed({ if (dialog === overlay) hide() }, 3000)
                } else {
                    handler.postDelayed(this, 400)
                }
            }
        }
        ticker = update
        handler.post(update)
    }

    /** Called by the import monitor so a short-lived native terminal state cannot be missed. */
    fun notifyTerminal(sessionId: Long, terminalState: Int) {
        if (terminalState != 5 && terminalState != 6) return
        handler.post {
            pendingTerminalSessionId = sessionId
            pendingTerminalState = terminalState
            if (activeSessionId != sessionId) return@post
            stateTracker?.latchTerminal(terminalState)
            pendingTerminalSessionId = null
            pendingTerminalState = null
            ticker?.let {
                handler.removeCallbacks(it)
                handler.post(it)
            }
        }
    }

    /** Rebinds an already visible overlay after pause/resume or checkpoint restore. */
    fun bindSession(sessionId: Long) {
        handler.post {
            if (dialog?.isShowing != true) return@post
            if (activeSessionId != sessionId) {
                activeSessionId = sessionId
                stateTracker = BuildImportOverlayStateTracker()
            }
            if (pendingTerminalSessionId == sessionId) {
                pendingTerminalState?.let { stateTracker?.latchTerminal(it) }
                pendingTerminalSessionId = null
                pendingTerminalState = null
            }
            ticker?.let {
                handler.removeCallbacks(it)
                handler.post(it)
            }
        }
    }

    fun hide() {
        ticker?.let(handler::removeCallbacks)
        ticker = null
        activeSessionId = null
        stateTracker = null
        ownerActivityRef = null
        dialog?.let { current ->
            dialog = null
            if (current.isShowing) current.dismiss()
        }
    }

    private fun rounded(fill: String, radius: Int, stroke: String? = null) = GradientDrawable().apply {
        setColor(Color.parseColor(fill))
        cornerRadius = radius.toFloat()
        if (stroke != null) setStroke(1, Color.parseColor(stroke))
    }

    private fun formatCount(value: Long) = String.format(Locale.ROOT, "%,d", value)
    private fun formatSpeed(value: Double) = String.format(Locale.ROOT, "%.1f", value)
    private fun formatElapsed(totalSeconds: Long): String {
        val hours = totalSeconds / 3600L
        val minutes = (totalSeconds % 3600L) / 60L
        val seconds = totalSeconds % 60L
        return String.format(Locale.ROOT, "%02d:%02d:%02d", hours, minutes, seconds)
    }
    private fun stateLabel(state: Int) = when (state) {
        0 -> "等待导入"
        1 -> "正在解析建筑"
        2 -> "正在导入"
        3 -> "导入已暂停"
        4 -> "世界或维度已变化"
        5 -> "导入完成"
        6 -> "导入失败"
        7 -> "正在校验区块"
        IMPORT_OVERLAY_ENDED_STATE -> "导入已结束"
        else -> "未知状态"
    }
}
