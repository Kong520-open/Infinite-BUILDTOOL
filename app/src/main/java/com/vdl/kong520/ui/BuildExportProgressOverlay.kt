package com.vdl.kong520.ui

import android.R.attr.progressBarStyleHorizontal
import android.app.Activity
import android.app.Application
import android.app.Dialog
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.os.Handler
import android.os.Looper
import android.os.Bundle
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.view.WindowManager
import android.widget.Button
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.TextView
import com.vdl.kong520.TpModule
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean

/** In-game export status whose controls consume touches without blocking the game outside it. */
object BuildExportProgressOverlay {
    private data class TravelTarget(
        val x: Int,
        val z: Int,
        val distanceBlocks: Int,
        val waiting: Boolean,
        val batchIndex: Int,
        val batchCount: Int
    )

    private data class Snapshot(
        val state: Int,
        val status: String,
        val total: Long,
        val processed: Long,
        val teleportMode: Int,
        val teleportAllowed: Boolean,
        val target: TravelTarget?
    )

    private const val WAITING_FOR_PLAYER_STATE = 8
    private const val CAPTURING_CONTAINERS_STATE = 9
    private const val TELEPORT_MODE_AUTOMATIC = 1
    private const val TELEPORT_MODE_SEMI_AUTOMATIC = 2
    private const val TELEPORT_MODE_DISABLED = 3
    private const val POLL_INTERVAL_MS = 350L
    private const val TERMINAL_DISPLAY_MS = 3000L

    private val handler = Handler(Looper.getMainLooper())
    private var dialog: Dialog? = null
    private var ticker: Runnable? = null
    private var pendingHide: Runnable? = null
    private var ownerActivity: Activity? = null
    private var lifecycleCallbacks: Application.ActivityLifecycleCallbacks? = null

    fun show(activity: Activity): Boolean {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            handler.post { show(activity) }
            return false
        }
        if (activity.isFinishing || activity.isDestroyed) return false

        val initial = readSnapshot() ?: return false
        if (!isActive(initial.state)) return false
        ensureLifecycleTracking(activity)
        if (ownerActivity === activity && dialog?.isShowing == true) return true
        // Recreate an existing terminal overlay if a new export starts during
        // its three-second completion display window.
        hide()

        val density = activity.resources.displayMetrics.density
        fun dp(value: Float) = (value * density).toInt()

        val overlay = Dialog(activity).apply {
            requestWindowFeature(Window.FEATURE_NO_TITLE)
            setCancelable(false)
        }
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            isClickable = true
            setPadding(dp(14f), dp(12f), dp(14f), dp(12f))
            background = rounded("#F7FFFEFA", dp(14f), "#FFE7E0D7")
        }
        val title = TextView(activity).apply {
            text = "BUILD EXPORT"
            textSize = 10f
            typeface = Typeface.DEFAULT_BOLD
            letterSpacing = 0f
            setTextColor(Color.parseColor("#FFAA4E31"))
        }
        val state = TextView(activity).apply {
            textSize = 15f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor("#FF302D29"))
            setPadding(0, dp(4f), 0, 0)
        }
        val progress = ProgressBar(activity, null, progressBarStyleHorizontal).apply {
            max = 1000
            progressTintList = ColorStateList.valueOf(Color.parseColor("#FFCF6846"))
            progressBackgroundTintList = ColorStateList.valueOf(Color.parseColor("#FFE7E0D7"))
        }
        val count = TextView(activity).apply {
            textSize = 11f
            setTextColor(Color.parseColor("#FF77716A"))
            setPadding(0, dp(6f), 0, 0)
        }
        val regionProgress = TextView(activity).apply {
            textSize = 11f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor("#FFAA4E31"))
            setPadding(0, dp(5f), 0, 0)
            visibility = View.GONE
        }
        val navigation = TextView(activity).apply {
            textSize = 13f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor("#FF8A5C27"))
            setPadding(dp(10f), dp(8f), dp(10f), dp(8f))
            background = rounded("#FFF9EFDC", dp(9f), "#FFEED9B2")
            visibility = View.GONE
        }
        val detail = TextView(activity).apply {
            textSize = 10f
            setTextColor(Color.parseColor("#FF77716A"))
            setPadding(0, dp(5f), 0, 0)
            maxLines = 2
        }

        fun overlayAction(label: String, accent: Boolean = false) = Button(activity).apply {
            text = label
            isAllCaps = false
            textSize = 11f
            minHeight = 0
            minimumHeight = 0
            minWidth = 0
            minimumWidth = 0
            setPadding(dp(4f), 0, dp(4f), 0)
            setTextColor(
                if (accent) Color.WHITE
                else Color.parseColor("#FF302D29")
            )
            background = rounded(
                if (accent) "#FFCF6846" else "#FFFFFEFA",
                dp(9f),
                if (accent) null else "#FFDED7CE"
            )
        }
        val nextRegion = overlayAction("下一个区块", true)
        val cancelExport = overlayAction("取消导出")
        val actions = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            visibility = View.GONE
        }
        actions.addView(nextRegion, LinearLayout.LayoutParams(0, dp(38f), 1f).apply {
            marginEnd = dp(6f)
        })
        actions.addView(cancelExport, LinearLayout.LayoutParams(0, dp(38f), 1f))

        root.addView(title)
        root.addView(state)
        root.addView(
            progress,
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(7f)).apply {
                topMargin = dp(9f)
            }
        )
        root.addView(count)
        root.addView(regionProgress)
        root.addView(
            navigation,
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                topMargin = dp(8f)
            }
        )
        root.addView(detail)
        root.addView(actions, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            dp(38f)
        ).apply {
            topMargin = dp(9f)
        })

        overlay.setContentView(root)
        overlay.window?.let { window ->
            // Keep focus with the game while allowing controls inside this
            // window to consume touches; touches outside remain with the game.
            window.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            window.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
            window.addFlags(
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                    WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
            )
            window.setGravity(Gravity.TOP or Gravity.END)
            window.attributes = window.attributes.apply {
                x = dp(12f)
                y = dp(54f)
            }
        }
        try {
            overlay.show()
        } catch (_: RuntimeException) {
            runCatching { overlay.dismiss() }
            return false
        }
        overlay.window?.setLayout(dp(272f), ViewGroup.LayoutParams.WRAP_CONTENT)
        dialog = overlay
        ownerActivity = activity
        overlay.setOnDismissListener {
            if (dialog === overlay) {
                ticker?.let(handler::removeCallbacks)
                ticker = null
                pendingHide?.let(handler::removeCallbacks)
                pendingHide = null
                dialog = null
                ownerActivity = null
            }
        }
        val actionInFlight = AtomicBoolean(false)

        fun render(snapshot: Snapshot) {
            val total = snapshot.total.coerceAtLeast(0L)
            val processed = snapshot.processed.coerceAtLeast(0L).let {
                if (total > 0L) it.coerceAtMost(total) else it
            }
            val progressValue = if (total > 0L) {
                ((processed.toDouble() / total.toDouble()) * 1000.0)
                    .toInt()
                    .coerceIn(0, 1000)
            } else {
                0
            }
            progress.progress = progressValue
            state.text = stateLabel(snapshot.state)
            count.text = if (total > 0L) {
                String.format(
                    Locale.ROOT,
                    "已处理 %,d / %,d  ·  %.1f%%",
                    processed,
                    total,
                    progressValue / 10f
                )
            } else {
                "已处理 ${formatCount(processed)} 个方块位置"
            }

            val target = snapshot.target
            val showNavigation = isActive(snapshot.state) && target != null &&
                (snapshot.teleportMode != TELEPORT_MODE_AUTOMATIC || !snapshot.teleportAllowed)
            if (showNavigation) {
                navigation.visibility = View.VISIBLE
                val distance = if (target.distanceBlocks >= 0) {
                    "距离 ${formatCount(target.distanceBlocks.toLong())} 格"
                } else {
                    "距离暂不可用"
                }
                val regionText = if (target.batchIndex > 0 && target.batchCount > 0) {
                    "分区 ${target.batchIndex} / ${target.batchCount}\n"
                } else {
                    ""
                }
                navigation.text = if (target.waiting || snapshot.state == WAITING_FOR_PLAYER_STATE) {
                    regionText + if (snapshot.teleportMode == TELEPORT_MODE_SEMI_AUTOMATIC) {
                        "等待确认前往  X=${target.x}  Z=${target.z}\n$distance"
                    } else {
                        "请前往  X=${target.x}  Z=${target.z}\n$distance · 到达后自动继续"
                    }
                } else {
                    regionText + "当前区域  X=${target.x}  Z=${target.z}\n$distance · 正在加载"
                }
            } else {
                navigation.visibility = View.GONE
                navigation.text = ""
            }
            val active = isActive(snapshot.state)
            val semiAutomatic = snapshot.teleportMode == TELEPORT_MODE_SEMI_AUTOMATIC
            actions.visibility = if (active && semiAutomatic) View.VISIBLE else View.GONE
            regionProgress.visibility = if (active && semiAutomatic) View.VISIBLE else View.GONE
            regionProgress.text = target?.let {
                if (it.batchIndex > 0 && it.batchCount > 0) {
                    "导出分区 ${it.batchIndex} / ${it.batchCount}"
                } else {
                    "导出分区准备中"
                }
            } ?: "导出分区准备中"
            nextRegion.isEnabled = active && semiAutomatic &&
                snapshot.state == WAITING_FOR_PLAYER_STATE && target != null &&
                !actionInFlight.get()
            cancelExport.isEnabled = active && semiAutomatic && !actionInFlight.get()
            detail.text = snapshot.status.ifBlank { defaultStatus(snapshot.state) }
        }

        fun runOverlayAction(label: String, action: () -> Boolean) {
            if (!actionInFlight.compareAndSet(false, true)) return
            nextRegion.isEnabled = false
            cancelExport.isEnabled = false
            detail.text = "$label…"
            Thread({
                val succeeded = runCatching { action() }.getOrDefault(false)
                actionInFlight.set(false)
                handler.post {
                    if (dialog !== overlay) return@post
                    val snapshot = readSnapshot()
                    if (snapshot != null) {
                        render(snapshot)
                        if (!succeeded) {
                            detail.text = snapshot.status.ifBlank { "${label}失败" }
                        }
                    } else if (!succeeded) {
                        detail.text = "${label}失败"
                    }
                }
            }, "build-export-overlay-action").start()
        }

        nextRegion.setOnClickListener {
            runOverlayAction("正在前往下一个区块") {
                runCatching { TpModule.requestBuildExportNextRegion() }.getOrDefault(false)
            }
        }
        cancelExport.setOnClickListener {
            runOverlayAction("正在取消建筑导出") {
                runCatching {
                    TpModule.cancelBuildExport()
                    true
                }.getOrDefault(false)
            }
        }

        val update = object : Runnable {
            override fun run() {
                if (dialog !== overlay) return
                if (!overlay.isShowing || activity.isFinishing || activity.isDestroyed) {
                    hide()
                    return
                }
                val snapshot = readSnapshot()
                if (snapshot == null) {
                    state.text = "导出状态不可用"
                    detail.text = "无法读取原生建筑导出状态"
                    scheduleHide(overlay)
                    return
                }
                render(snapshot)
                if (isTerminal(snapshot.state) || !isActive(snapshot.state)) {
                    scheduleHide(overlay)
                } else {
                    handler.postDelayed(this, POLL_INTERVAL_MS)
                }
            }
        }
        ticker = update
        render(initial)
        handler.postDelayed(update, POLL_INTERVAL_MS)
        return true
    }

    fun hide() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            handler.post { hide() }
            return
        }
        ticker?.let(handler::removeCallbacks)
        ticker = null
        pendingHide?.let(handler::removeCallbacks)
        pendingHide = null
        ownerActivity = null
        dialog?.let { current ->
            dialog = null
            if (current.isShowing) runCatching { current.dismiss() }
        }
    }

    private fun ensureLifecycleTracking(activity: Activity) {
        if (lifecycleCallbacks != null) return
        val callbacks = object : Application.ActivityLifecycleCallbacks {
            override fun onActivityCreated(target: Activity, savedInstanceState: Bundle?) = Unit
            override fun onActivityStarted(target: Activity) = Unit

            override fun onActivityResumed(target: Activity) {
                handler.post {
                    if (target.isFinishing || target.isDestroyed) return@post
                    if (ownerActivity === target && dialog?.isShowing == true) return@post
                    val snapshot = readSnapshot() ?: return@post
                    if (isActive(snapshot.state)) show(target)
                }
            }

            override fun onActivityPaused(target: Activity) = Unit
            override fun onActivityStopped(target: Activity) = Unit
            override fun onActivitySaveInstanceState(target: Activity, outState: Bundle) = Unit

            override fun onActivityDestroyed(target: Activity) {
                handler.post {
                    if (ownerActivity === target) hide()
                }
            }
        }
        activity.application.registerActivityLifecycleCallbacks(callbacks)
        lifecycleCallbacks = callbacks
    }

    private fun scheduleHide(expectedDialog: Dialog) {
        if (pendingHide != null) return
        val action = Runnable {
            pendingHide = null
            if (dialog === expectedDialog) hide()
        }
        pendingHide = action
        handler.postDelayed(action, TERMINAL_DISPLAY_MS)
    }

    private fun readSnapshot(): Snapshot? = try {
        val state = TpModule.getBuildExportState()
        val target = TpModule.getBuildExportTravelTarget()
            ?.takeIf { it.size >= 5 }
            ?.let {
                TravelTarget(
                    x = it[0],
                    z = it[2],
                    distanceBlocks = it[3],
                    waiting = it[4] != 0,
                    batchIndex = if (it.size > 5) it[5] else 0,
                    batchCount = if (it.size > 6) it[6] else 0
                )
            }
        val teleportMode = TpModule.getBuildExportTeleportMode()
            .takeIf { it in TELEPORT_MODE_AUTOMATIC..TELEPORT_MODE_DISABLED }
            ?: TELEPORT_MODE_AUTOMATIC
        Snapshot(
            state = state,
            status = TpModule.getBuildExportStatus()?.trim().orEmpty(),
            total = TpModule.getBuildExportTotalBlocks(),
            processed = TpModule.getBuildExportProcessedBlocks(),
            teleportMode = teleportMode,
            teleportAllowed = TpModule.isBuildExportTeleportAllowed(),
            target = target
        )
    } catch (_: Throwable) {
        null
    }

    private fun isActive(state: Int) = state in 1..4 ||
        state == WAITING_FOR_PLAYER_STATE || state == CAPTURING_CONTAINERS_STATE
    private fun isTerminal(state: Int) = state in 5..7

    private fun stateLabel(state: Int) = when (state) {
        1 -> "正在准备导出"
        2 -> "正在加载区域"
        3 -> "正在扫描方块"
        4 -> "正在写入文件"
        5 -> "导出完成"
        6 -> "导出失败"
        7 -> "导出已取消"
        WAITING_FOR_PLAYER_STATE -> "等待前往目标区域"
        CAPTURING_CONTAINERS_STATE -> "正在读取容器"
        else -> "等待导出"
    }

    private fun defaultStatus(state: Int) = when (state) {
        1 -> "正在验证导出参数"
        2 -> "正在加载导出范围所需区块"
        3 -> "正在读取方块数据"
        4 -> "正在生成建筑文件"
        5 -> "建筑文件已保存到 files/netease/"
        6 -> "建筑导出失败"
        7 -> "建筑导出已取消"
        WAITING_FOR_PLAYER_STATE -> "进入目标区域后将自动继续扫描"
        CAPTURING_CONTAINERS_STATE -> "正在逐个读取容器内物品"
        else -> "等待开始"
    }

    private fun rounded(fill: String, radius: Int, stroke: String? = null) = GradientDrawable().apply {
        setColor(Color.parseColor(fill))
        cornerRadius = radius.toFloat()
        if (stroke != null) setStroke(1, Color.parseColor(stroke))
    }

    private fun formatCount(value: Long) = String.format(Locale.ROOT, "%,d", value)
}
