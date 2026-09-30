package com.vdl.kong520.ui

import android.R.attr.progressBarStyleHorizontal
import android.app.Activity
import android.app.Application
import android.app.Dialog
import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.os.Bundle
import android.os.Handler
import android.os.Looper
import android.text.TextUtils
import android.view.Gravity
import android.view.MotionEvent
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.view.WindowManager
import android.widget.CheckBox
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.SeekBar
import android.widget.TextView
import com.vdl.kong520.TpModule
import java.lang.ref.WeakReference
import java.util.Locale

/**
 * Small, touchable control for the projection printer.  It is built entirely
 * from platform Views so it remains safe when it is attached to Minecraft's
 * process through Xposed (module XML resources would resolve against the host).
 */
object BuildProjectionPrinterOverlay {
    private const val SURFACE = "#FAF7F1"
    private const val ACCENT = "#CF6846"
    private const val ACCENT_DARK = "#AA4E31"
    private const val INK = "#302D29"
    private const val MUTED = "#77716A"
    private const val LINE = "#E7E0D7"
    private const val GREEN = "#3F765C"
    private const val WARNING = "#A76728"
    private const val ERROR = "#B34C42"

    private const val PREFS = "build_projection_ui"
    private const val KEY_POSITION_X = "projection_printer_overlay_x"
    private const val KEY_POSITION_Y = "projection_printer_overlay_y"
    private const val POLL_INTERVAL_MS = 350L

    // Must remain in sync with the native projection-printer state enum.
    private const val STATE_OFF = 0
    private const val STATE_IDLE = 1
    private const val STATE_RUNNING = 2
    private const val STATE_WAITING_MATERIAL = 3
    private const val STATE_WAITING_SUPPORT = 4
    private const val STATE_COMPLETE = 5
    private const val STATE_ERROR = 6

    private data class Snapshot(
        val enabled: Boolean,
        val rate: Int,
        val state: Int,
        val status: String,
        val total: Long,
        val placed: Long,
        val skipped: Long
    )

    private val handler = Handler(Looper.getMainLooper())
    private var dialog: Dialog? = null
    private var ownerActivityRef: WeakReference<Activity>? = null
    private var ticker: Runnable? = null
    private var lifecycleCallbacks: Application.ActivityLifecycleCallbacks? = null
    @Volatile private var requestedVisible = false

    /**
     * Configures the native printer and shows the in-game control after a
     * projection has been submitted successfully.
     */
    fun show(activity: Activity, context: Context, config: ProjectionPrinterSettings.Config) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            handler.post { show(activity, context, config) }
            return
        }
        if (activity.isFinishing || activity.isDestroyed) return
        requestedVisible = true
        applyConfig(context, config)
        ensureLifecycleTracking(activity.application)

        if (ownerActivityRef?.get() === activity && dialog?.isShowing == true) return
        dismissWindow(clearRequest = false)
        createWindow(activity, context, config)
    }

    /** Called from the Xposed host lifecycle to restore a surviving control. */
    fun attachToHostIfNeeded(activity: Activity) {
        if (!requestedVisible || activity.isFinishing || activity.isDestroyed) return
        show(activity, activity.applicationContext, ProjectionPrinterSettings.read(activity))
    }

    /** Removes the control because its projection has been cleared or failed. */
    fun hide() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            handler.post { hide() }
            return
        }
        dismissWindow(clearRequest = true)
    }

    private fun createWindow(
        activity: Activity,
        context: Context,
        initialConfig: ProjectionPrinterSettings.Config
    ) {
        val density = activity.resources.displayMetrics.density
        fun dp(value: Float) = (value * density).toInt()
        val prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val defaultX = dp(12f)
        val defaultY = dp(116f)
        var windowX = prefs.getInt(KEY_POSITION_X, defaultX).coerceAtLeast(0)
        var windowY = prefs.getInt(KEY_POSITION_Y, defaultY).coerceAtLeast(0)
        // This is an in-game control, not a settings window. Keep the default
        // footprint small; the rate slider is available from a disclosure row.
        val panelWidth = minOf(dp(188f), (activity.resources.displayMetrics.widthPixels * 0.72f).toInt())

        val overlay = Dialog(activity).apply {
            requestWindowFeature(Window.FEATURE_NO_TITLE)
            setCancelable(false)
            setCanceledOnTouchOutside(false)
        }
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            isClickable = true
            setPadding(dp(9f), dp(8f), dp(9f), dp(9f))
            background = rounded("#F2FFFEFA", dp(12f), LINE)
            elevation = dp(8f).toFloat()
        }
        val dragHeader = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(1f), 0, dp(1f), dp(2f))
        }
        dragHeader.addView(TextView(activity).apply {
            text = "投影打印机"
            textSize = 9f
            letterSpacing = 0.06f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(ACCENT_DARK))
        }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        dragHeader.addView(TextView(activity).apply {
            text = "拖动 ↕"
            textSize = 9f
            setTextColor(Color.parseColor(MUTED))
            background = rounded(SURFACE, dp(6f), LINE)
            setPadding(dp(5f), dp(2f), dp(5f), dp(2f))
        })
        root.addView(dragHeader)

        val switchRow = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(2f), 0, 0)
        }
        val enabled = CheckBox(activity).apply {
            text = "启用打印"
            textSize = 12f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(INK))
            buttonTintList = ColorStateList.valueOf(Color.parseColor(ACCENT))
            isChecked = initialConfig.enabled
            minHeight = 0
            minimumHeight = 0
            setPadding(0, 0, 0, 0)
        }
        val state = TextView(activity).apply {
            gravity = Gravity.END or Gravity.CENTER_VERTICAL
            textSize = 9f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(MUTED))
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
        }
        switchRow.addView(enabled, LinearLayout.LayoutParams(0, dp(32f), 1f))
        switchRow.addView(state, LinearLayout.LayoutParams(dp(66f), dp(32f)))
        root.addView(switchRow)

        val progress = ProgressBar(activity, null, progressBarStyleHorizontal).apply {
            max = 1000
            progressTintList = ColorStateList.valueOf(Color.parseColor(ACCENT))
            progressBackgroundTintList = ColorStateList.valueOf(Color.parseColor(LINE))
        }
        root.addView(progress, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            dp(4f)
        ).apply { topMargin = dp(1f) })
        val progressText = TextView(activity).apply {
            textSize = 9f
            setTextColor(Color.parseColor(MUTED))
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
            setPadding(0, dp(4f), 0, 0)
        }
        root.addView(progressText)
        val detail = TextView(activity).apply {
            textSize = 9f
            setTextColor(Color.parseColor(MUTED))
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
            setPadding(0, dp(2f), 0, 0)
        }
        root.addView(detail)

        val speedToggle = TextView(activity).apply {
            gravity = Gravity.CENTER_VERTICAL
            textSize = 9f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(ACCENT_DARK))
            background = rounded(SURFACE, dp(7f), LINE)
            setPadding(dp(7f), 0, dp(7f), 0)
            isClickable = true
            isFocusable = true
        }
        root.addView(speedToggle, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            dp(24f)
        ).apply { topMargin = dp(5f) })

        val speedPanel = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            visibility = View.GONE
            setPadding(0, dp(3f), 0, 0)
        }
        val rateLabel = TextView(activity).apply {
            textSize = 9f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(ACCENT_DARK))
            setPadding(0, dp(1f), 0, 0)
        }
        speedPanel.addView(rateLabel)
        val rate = SeekBar(activity).apply {
            this.max = ProjectionPrinterSettings.MAX_RATE - ProjectionPrinterSettings.MIN_RATE
            this.progress = (initialConfig.blocksPerSecond - ProjectionPrinterSettings.MIN_RATE)
                .coerceIn(0, this.max)
            progressTintList = ColorStateList.valueOf(Color.parseColor(ACCENT))
            thumbTintList = ColorStateList.valueOf(Color.parseColor(ACCENT))
        }
        speedPanel.addView(rate, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            dp(25f)
        ))
        speedPanel.addView(TextView(activity).apply {
            text = "仅处理当前显示范围内可合法放置的方块"
            textSize = 8f
            setTextColor(Color.parseColor(MUTED))
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
            setPadding(0, 0, 0, dp(1f))
        })
        root.addView(speedPanel)

        fun configuredRate(): Int = (rate.progress + ProjectionPrinterSettings.MIN_RATE)
            .coerceIn(ProjectionPrinterSettings.MIN_RATE, ProjectionPrinterSettings.MAX_RATE)

        var rateExpanded = false
        fun renderRateControls(value: Int) {
            rateLabel.text = "速度上限 · $value 方块/秒"
            speedToggle.text = "速度 · $value/秒  ${if (rateExpanded) "收起 ▲" else "调整 ▾"}"
        }
        speedToggle.setOnClickListener {
            rateExpanded = !rateExpanded
            speedPanel.visibility = if (rateExpanded) View.VISIBLE else View.GONE
            renderRateControls(configuredRate())
        }

        fun render(snapshot: Snapshot) {
            val nativeRate = snapshot.rate.coerceIn(
                ProjectionPrinterSettings.MIN_RATE,
                ProjectionPrinterSettings.MAX_RATE
            )
            if (!rate.isPressed && rate.progress != nativeRate - ProjectionPrinterSettings.MIN_RATE) {
                rate.progress = nativeRate - ProjectionPrinterSettings.MIN_RATE
            }
            renderRateControls(nativeRate)
            val total = snapshot.total.coerceAtLeast(0L)
            val placed = snapshot.placed.coerceAtLeast(0L).let {
                if (total > 0L) it.coerceAtMost(total) else it
            }
            val progressValue = if (total > 0L) {
                ((placed.toDouble() / total.toDouble()) * 1000.0).toInt().coerceIn(0, 1000)
            } else {
                0
            }
            progress.progress = progressValue
            progressText.text = if (total > 0L) {
                String.format(
                    Locale.ROOT,
                    "%,d / %,d · 跳过 %,d · %.1f%%",
                    placed,
                    total,
                    snapshot.skipped.coerceAtLeast(0L),
                    progressValue / 10f
                )
            } else if (snapshot.state == STATE_COMPLETE) {
                "当前显示范围内没有待打印方块"
            } else {
                "正在统计当前显示范围…"
            }
            state.text = stateLabel(snapshot.state, snapshot.enabled)
            state.setTextColor(stateColor(snapshot.state, snapshot.enabled))
            detail.text = snapshot.status.ifBlank { defaultStatus(snapshot.state, snapshot.enabled) }
        }

        enabled.setOnCheckedChangeListener { _, checked ->
            ProjectionPrinterSettings.setEnabled(context, checked)
            val applied = runCatching {
                TpModule.setBuildProjectionPrinterEnabled(checked)
            }.isSuccess
            if (!applied) {
                detail.text = "打印机开关暂时不可用"
            }
        }
        rate.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progressValue: Int, fromUser: Boolean) {
                val value = configuredRate()
                renderRateControls(value)
                if (fromUser) {
                    ProjectionPrinterSettings.setRate(context, value)
                    runCatching { TpModule.setBuildProjectionPrinterRate(value) }
                }
            }

            override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit
            override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
        })
        renderRateControls(configuredRate())

        fun moveWindow(nextX: Int, nextY: Int, save: Boolean) {
            val screenWidth = activity.resources.displayMetrics.widthPixels
            val screenHeight = activity.resources.displayMetrics.heightPixels
            val panelHeight = root.height.takeIf { it > 0 } ?: dp(128f)
            val boundedX = nextX.coerceIn(0, (screenWidth - panelWidth).coerceAtLeast(0))
            val boundedY = nextY.coerceIn(0, (screenHeight - panelHeight).coerceAtLeast(0))
            windowX = boundedX
            windowY = boundedY
            overlay.window?.attributes = overlay.window?.attributes?.apply {
                x = boundedX
                y = boundedY
            }
            if (save) {
                prefs.edit().putInt(KEY_POSITION_X, boundedX).putInt(KEY_POSITION_Y, boundedY).apply()
            }
        }
        dragHeader.setOnTouchListener(object : View.OnTouchListener {
            private var downRawX = 0f
            private var downRawY = 0f
            private var startX = 0
            private var startY = 0

            override fun onTouch(view: View, event: MotionEvent): Boolean = when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    downRawX = event.rawX
                    downRawY = event.rawY
                    startX = windowX
                    startY = windowY
                    true
                }

                MotionEvent.ACTION_MOVE -> {
                    moveWindow(
                        startX + (event.rawX - downRawX).toInt(),
                        startY + (event.rawY - downRawY).toInt(),
                        save = false
                    )
                    true
                }

                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    moveWindow(windowX, windowY, save = true)
                    view.performClick()
                    true
                }

                else -> true
            }
        })

        overlay.setContentView(root)
        overlay.window?.let { window ->
            // This Dialog is attached to the host Activity (not a system
            // overlay), so no draw-over-other-apps permission is needed.
            window.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            window.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
            window.addFlags(
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                    WindowManager.LayoutParams.FLAG_LAYOUT_IN_SCREEN
            )
            window.setGravity(Gravity.TOP or Gravity.START)
            window.attributes = window.attributes.apply {
                x = windowX
                y = windowY
            }
        }
        try {
            overlay.show()
        } catch (_: RuntimeException) {
            requestedVisible = false
            return
        }
        overlay.window?.setLayout(panelWidth, ViewGroup.LayoutParams.WRAP_CONTENT)
        dialog = overlay
        ownerActivityRef = WeakReference(activity)
        overlay.setOnDismissListener {
            if (dialog === overlay) {
                ticker?.let(handler::removeCallbacks)
                ticker = null
                dialog = null
                ownerActivityRef = null
            }
        }

        fun refresh() {
            val snapshot = readSnapshot()
            if (snapshot == null) {
                state.text = "状态不可用"
                state.setTextColor(Color.parseColor(ERROR))
                detail.text = "无法读取投影打印机状态"
                return
            }
            if (enabled.isChecked != snapshot.enabled) enabled.isChecked = snapshot.enabled
            render(snapshot)
        }
        val update = object : Runnable {
            override fun run() {
                if (dialog !== overlay || !overlay.isShowing ||
                    activity.isFinishing || activity.isDestroyed) {
                    return
                }
                refresh()
                handler.postDelayed(this, POLL_INTERVAL_MS)
            }
        }
        ticker = update
        refresh()
        handler.postDelayed(update, POLL_INTERVAL_MS)
    }

    private fun applyConfig(context: Context, config: ProjectionPrinterSettings.Config) {
        ProjectionPrinterSettings.setEnabled(context, config.enabled)
        ProjectionPrinterSettings.setRate(context, config.blocksPerSecond)
        runCatching { TpModule.setBuildProjectionPrinterRate(config.blocksPerSecond) }
        runCatching { TpModule.setBuildProjectionPrinterEnabled(config.enabled) }
    }

    private fun dismissWindow(clearRequest: Boolean) {
        ticker?.let(handler::removeCallbacks)
        ticker = null
        if (clearRequest) requestedVisible = false
        ownerActivityRef = null
        dialog?.let { current ->
            dialog = null
            if (current.isShowing) runCatching { current.dismiss() }
        }
    }

    private fun ensureLifecycleTracking(application: Application) {
        if (lifecycleCallbacks != null) return
        val callbacks = object : Application.ActivityLifecycleCallbacks {
            override fun onActivityCreated(activity: Activity, savedInstanceState: Bundle?) = Unit
            override fun onActivityStarted(activity: Activity) = Unit
            override fun onActivityResumed(activity: Activity) = Unit
            override fun onActivityPaused(activity: Activity) = Unit
            override fun onActivityStopped(activity: Activity) = Unit
            override fun onActivitySaveInstanceState(activity: Activity, outState: Bundle) = Unit

            override fun onActivityDestroyed(activity: Activity) {
                if (ownerActivityRef?.get() !== activity) return
                handler.post {
                    if (ownerActivityRef?.get() === activity) {
                        dismissWindow(clearRequest = false)
                    }
                }
            }
        }
        application.registerActivityLifecycleCallbacks(callbacks)
        lifecycleCallbacks = callbacks
    }

    private fun readSnapshot(): Snapshot? = try {
        Snapshot(
            enabled = TpModule.isBuildProjectionPrinterEnabled(),
            rate = TpModule.getBuildProjectionPrinterRate(),
            state = TpModule.getBuildProjectionPrinterState(),
            status = TpModule.getBuildProjectionPrinterStatus()?.trim().orEmpty(),
            total = TpModule.getBuildProjectionPrinterTotalBlocks(),
            placed = TpModule.getBuildProjectionPrinterPlacedBlocks(),
            skipped = TpModule.getBuildProjectionPrinterSkippedBlocks()
        )
    } catch (_: Throwable) {
        null
    }

    private fun stateLabel(state: Int, enabled: Boolean): String = when {
        !enabled || state == STATE_OFF -> "已关闭"
        state == STATE_IDLE -> "待命"
        state == STATE_RUNNING -> "打印中"
        state == STATE_WAITING_MATERIAL -> "等待材料"
        state == STATE_WAITING_SUPPORT -> "等待支撑"
        state == STATE_COMPLETE -> "已完成"
        state == STATE_ERROR -> "错误"
        else -> "等待中"
    }

    private fun stateColor(state: Int, enabled: Boolean): Int = when {
        !enabled || state == STATE_OFF -> Color.parseColor(MUTED)
        state == STATE_RUNNING -> Color.parseColor(ACCENT_DARK)
        state == STATE_WAITING_MATERIAL || state == STATE_WAITING_SUPPORT -> Color.parseColor(WARNING)
        state == STATE_COMPLETE -> Color.parseColor(GREEN)
        state == STATE_ERROR -> Color.parseColor(ERROR)
        else -> Color.parseColor(INK)
    }

    private fun defaultStatus(state: Int, enabled: Boolean): String = when {
        !enabled || state == STATE_OFF -> "打开后会按当前可见投影范围自动选择合法方块"
        state == STATE_IDLE -> "移动到投影附近后会选择可放置的方块"
        state == STATE_RUNNING -> "正在从当前热栏取材并等待方块确认"
        state == STATE_WAITING_MATERIAL -> "当前热栏没有可用材料，保留其他可放置目标"
        state == STATE_WAITING_SUPPORT -> "当前目标缺少可点击支撑面或不在交互范围内"
        state == STATE_COMPLETE -> "当前显示范围内的可打印方块已完成"
        state == STATE_ERROR -> "打印机已暂停，请检查游戏状态"
        else -> "正在等待投影打印机"
    }

    private fun rounded(fill: String, radius: Int, stroke: String? = null) = GradientDrawable().apply {
        setColor(Color.parseColor(fill))
        cornerRadius = radius.toFloat()
        if (stroke != null) setStroke(1, Color.parseColor(stroke))
    }
}
