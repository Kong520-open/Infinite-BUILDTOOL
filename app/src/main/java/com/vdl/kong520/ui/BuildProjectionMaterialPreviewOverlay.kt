package com.vdl.kong520.ui

import android.app.Activity
import android.app.Application
import android.app.Dialog
import android.content.Context
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
import android.view.ViewConfiguration
import android.view.Window
import android.view.WindowManager
import android.widget.LinearLayout
import android.widget.TextView
import com.vdl.kong520.TpModule
import java.lang.ref.WeakReference
import java.util.Locale
import java.util.concurrent.atomic.AtomicLong

/**
 * Compact, activity-owned preview of every material used by the loaded
 * projection.  The native summary is intentionally paged: the floating panel
 * always owns exactly [ROWS_PER_PAGE] rows and never needs a scrolling body.
 *
 * This uses only platform Views.  It is therefore safe to attach inside the
 * Xposed game process, where module XML resources would resolve against the
 * host application instead of this module.
 */
object BuildProjectionMaterialPreviewOverlay {
    private const val BG = "#FFFEFA"
    private const val ACCENT = "#CF6846"
    private const val ACCENT_DARK = "#AA4E31"
    private const val INK = "#302D29"
    private const val MUTED = "#77716A"
    private const val LINE = "#E7E0D7"
    private const val GREEN = "#3F765C"
    private const val WARNING = "#A76728"
    private const val ERROR = "#B34C42"

    private const val PREFS = "build_projection_ui"
    private const val KEY_ENABLED = "projection_material_preview_enabled"
    private const val KEY_POSITION_X = "projection_material_preview_x"
    private const val KEY_POSITION_Y = "projection_material_preview_y"
    private const val ROWS_PER_PAGE = 8
    private const val POLL_INTERVAL_MS = 700L

    private enum class PageState {
        READY,
        LOADING,
        EMPTY,
        ERROR
    }

    private data class MaterialEntry(
        val name: String,
        val aux: Long,
        val count: Long,
        // Null means the game-thread inventory cache has not completed a safe
        // snapshot yet. It must never be presented as a missing material.
        val availableCount: Long?
    )

    private data class MaterialRow(
        val container: LinearLayout,
        val material: TextView,
        val count: TextView
    )

    /** Delegates every Activity callback except the raw-touch interception below. */
    private class HostTouchCallback(
        val delegate: Window.Callback,
        private val intercept: (MotionEvent) -> Boolean
    ) : Window.Callback by delegate {
        override fun dispatchTouchEvent(event: MotionEvent): Boolean =
            if (intercept(event)) true else delegate.dispatchTouchEvent(event)

        fun cancelDelegatedGesture(source: MotionEvent) {
            val cancel = MotionEvent.obtain(source)
            try {
                cancel.action = MotionEvent.ACTION_CANCEL
                delegate.dispatchTouchEvent(cancel)
            } finally {
                cancel.recycle()
            }
        }
    }

    /**
     * The visual Dialog is FLAG_NOT_TOUCHABLE, so ordinary game input always
     * passes through it.  The Activity callback observes a press inside the
     * panel; after a true long press it cancels the game's gesture and consumes
     * only the subsequent drag stream. The two visible pager buttons are the
     * deliberate exception: their compact bounds are intercepted from DOWN so
     * they remain usable without making the rest of the panel touchable.
     */
    private class LongPressDragPassthrough(
        private val activity: Activity,
        private val isActive: () -> Boolean,
        private val isInsidePanel: (Float, Float) -> Boolean,
        private val manualTapActionAt: (Float, Float) -> (() -> Unit)?,
        private val currentWindowPosition: () -> Pair<Int, Int>,
        private val moveWindow: (Int, Int, Boolean) -> Unit
    ) {
        private val handler = Handler(Looper.getMainLooper())
        private val touchSlop = ViewConfiguration.get(activity).scaledTouchSlop.toFloat()
        private var previousCallback: Window.Callback? = null
        private var installedCallback: HostTouchCallback? = null
        private var trackedDown: MotionEvent? = null
        private var tracking = false
        private var dragging = false
        private var downRawX = 0f
        private var downRawY = 0f
        private var startWindowX = 0
        private var startWindowY = 0
        private var manualTapAction: (() -> Unit)? = null
        private var manualTapCancelled = false
        private var manualTapDownRawX = 0f
        private var manualTapDownRawY = 0f

        private val beginDrag = Runnable {
            if (!tracking || dragging || !isActive()) return@Runnable
            dragging = true
            // The game already received DOWN while the panel was transparent to
            // input. Cancel it exactly when this becomes a drag, preventing a
            // held game interaction from continuing underneath the move.
            trackedDown?.let { installedCallback?.cancelDelegatedGesture(it) }
        }

        fun install() {
            val previous = activity.window.callback ?: return
            previousCallback = previous
            val callback = HostTouchCallback(previous) { event -> handleTouch(event) }
            installedCallback = callback
            activity.window.callback = callback
        }

        fun dispose() {
            handler.removeCallbacks(beginDrag)
            clearTrackedGesture()
            clearManualTap()
            val previous = previousCallback
            val callback = installedCallback
            if (previous != null && callback != null && activity.window.callback === callback) {
                activity.window.callback = previous
            }
            previousCallback = null
            installedCallback = null
        }

        private fun handleTouch(event: MotionEvent): Boolean {
            if (!isActive()) {
                clearTrackedGesture()
                clearManualTap()
                return false
            }
            if (manualTapAction != null) return handleManualTap(event)
            when (event.actionMasked) {
                MotionEvent.ACTION_DOWN -> {
                    clearTrackedGesture()
                    clearManualTap()
                    if (event.pointerCount == 1) {
                        val action = manualTapActionAt(event.rawX, event.rawY)
                        if (action != null) {
                            // Consume the complete button gesture from DOWN so
                            // Minecraft never receives a partial click beneath
                            // a manual page change.
                            manualTapAction = action
                            manualTapCancelled = false
                            manualTapDownRawX = event.rawX
                            manualTapDownRawY = event.rawY
                            return true
                        }
                    }
                    if (event.pointerCount != 1 || !isInsidePanel(event.rawX, event.rawY)) {
                        return false
                    }
                    tracking = true
                    dragging = false
                    downRawX = event.rawX
                    downRawY = event.rawY
                    val origin = currentWindowPosition()
                    startWindowX = origin.first
                    startWindowY = origin.second
                    trackedDown = MotionEvent.obtain(event)
                    handler.postDelayed(beginDrag, ViewConfiguration.getLongPressTimeout().toLong())
                    // Keep ordinary taps entirely in Minecraft.
                    return false
                }

                MotionEvent.ACTION_MOVE -> {
                    if (!tracking) return false
                    if (event.pointerCount != 1) {
                        val consume = dragging
                        if (dragging) moveWindow(startWindowX, startWindowY, true)
                        clearTrackedGesture()
                        return consume
                    }
                    val dx = event.rawX - downRawX
                    val dy = event.rawY - downRawY
                    if (!dragging && dx * dx + dy * dy > touchSlop * touchSlop) {
                        // It became a normal game swipe before the long-press
                        // timeout, so leave the complete stream untouched.
                        clearTrackedGesture()
                        return false
                    }
                    if (!dragging) return false
                    moveWindow(startWindowX + dx.toInt(), startWindowY + dy.toInt(), false)
                    return true
                }

                MotionEvent.ACTION_POINTER_DOWN, MotionEvent.ACTION_POINTER_UP -> {
                    // A two-finger gesture belongs entirely to the game.  If a
                    // drag had just started, finish it at its current position
                    // rather than letting a different pointer take over.
                    val consume = dragging
                    if (dragging) {
                        moveWindow(currentWindowPosition().first,
                            currentWindowPosition().second, true)
                    }
                    clearTrackedGesture()
                    return consume
                }

                MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                    if (!tracking) return false
                    val consume = dragging
                    if (dragging) moveWindow(currentWindowPosition().first,
                        currentWindowPosition().second, true)
                    clearTrackedGesture()
                    return consume
                }

                else -> return dragging
            }
        }

        private fun handleManualTap(event: MotionEvent): Boolean {
            when (event.actionMasked) {
                MotionEvent.ACTION_MOVE -> {
                    val dx = event.rawX - manualTapDownRawX
                    val dy = event.rawY - manualTapDownRawY
                    if (event.pointerCount != 1 || dx * dx + dy * dy > touchSlop * touchSlop) {
                        manualTapCancelled = true
                    }
                    return true
                }

                MotionEvent.ACTION_POINTER_DOWN, MotionEvent.ACTION_POINTER_UP -> {
                    manualTapCancelled = true
                    return true
                }

                MotionEvent.ACTION_UP -> {
                    val action = manualTapAction
                    val shouldInvoke = !manualTapCancelled && event.pointerCount == 1
                    clearManualTap()
                    if (shouldInvoke) action?.invoke()
                    return true
                }

                MotionEvent.ACTION_CANCEL -> {
                    clearManualTap()
                    return true
                }

                else -> return true
            }
        }

        private fun clearTrackedGesture() {
            handler.removeCallbacks(beginDrag)
            trackedDown?.recycle()
            trackedDown = null
            tracking = false
            dragging = false
        }

        private fun clearManualTap() {
            manualTapAction = null
            manualTapCancelled = false
            manualTapDownRawX = 0f
            manualTapDownRawY = 0f
        }
    }

    private data class MaterialPage(
        val state: PageState,
        val status: String,
        val generation: Long,
        val planIdentity: String,
        val totalBlocks: Long,
        val totalTypes: Long,
        val pageIndex: Int,
        val pageCount: Int,
        val rows: List<MaterialEntry>,
        val message: String = ""
    )

    private val handler = Handler(Looper.getMainLooper())
    private val requestSerial = AtomicLong(0L)
    private var dialog: Dialog? = null
    private var ownerActivityRef: WeakReference<Activity>? = null
    private var ticker: Runnable? = null
    private var touchPassthrough: LongPressDragPassthrough? = null
    private var lifecycleCallbacks: Application.ActivityLifecycleCallbacks? = null
    @Volatile private var requestedVisible = false
    @Volatile private var projectionAvailable = false

    /** Whether the switch in the projection settings currently requests this panel. */
    fun isEnabled(context: Context): Boolean = context.applicationContext
        .getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        .getBoolean(KEY_ENABLED, false)

    /**
     * Called by the projection settings switch.  Enabling it intentionally
     * opens the compact panel immediately, so its loading/no-projection state
     * is visible instead of looking like a failed tap.
     */
    fun setEnabled(activity: Activity, context: Context, enabled: Boolean) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            handler.post { setEnabled(activity, context, enabled) }
            return
        }
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_ENABLED, enabled)
            .apply()
        requestedVisible = enabled
        if (enabled) {
            show(activity, context.applicationContext)
        } else {
            dismissWindow(clearRequested = true)
        }
    }

    /** Hide while a replacement projection is parsing, preserving the switch. */
    fun onProjectionLoadStarted() {
        projectionAvailable = false
        hideForProjectionChange()
    }

    /** Hide after a load failure, preserving the switch for the next load. */
    fun onProjectionLoadFailed() {
        projectionAvailable = false
        hideForProjectionChange()
    }

    /** Hide after the active projection has been explicitly cleared. */
    fun onProjectionCleared() {
        projectionAvailable = false
        hideForProjectionChange()
    }

    /** Show after a successful projection submit only if the settings switch remains on. */
    fun onProjectionLoadSucceeded(activity: Activity, context: Context) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            handler.post { onProjectionLoadSucceeded(activity, context) }
            return
        }
        projectionAvailable = true
        requestedVisible = isEnabled(context)
        if (requestedVisible) show(activity, context.applicationContext)
    }

    /**
     * Reattach an activity-owned window after the Minecraft Activity is
     * recreated.  A panel is only restored for a projection that succeeded in
     * this process; clear/load-failure paths deliberately keep it hidden.
     */
    fun attachToHostIfNeeded(activity: Activity) {
        if (activity.isFinishing || activity.isDestroyed) return
        if (!requestedVisible) requestedVisible = isEnabled(activity)
        if (requestedVisible && projectionAvailable) {
            show(activity, activity.applicationContext)
        }
    }

    private fun hideForProjectionChange() {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            handler.post { hideForProjectionChange() }
            return
        }
        dismissWindow(clearRequested = false)
    }

    private fun show(activity: Activity, context: Context) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            handler.post { show(activity, context) }
            return
        }
        if (activity.isFinishing || activity.isDestroyed) return
        requestedVisible = true
        ensureLifecycleTracking(activity.application)
        if (ownerActivityRef?.get() === activity && dialog?.isShowing == true) return
        dismissWindow(clearRequested = false)
        createWindow(activity, context.applicationContext)
    }

    private fun createWindow(activity: Activity, context: Context) {
        val density = activity.resources.displayMetrics.density
        fun dp(value: Float): Int = (value * density + 0.5f).toInt()

        val screenWidth = activity.resources.displayMetrics.widthPixels
        val panelWidth = minOf(dp(272f), (screenWidth * 0.80f).toInt()).coerceAtLeast(dp(218f))
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val defaultX = (screenWidth - panelWidth - dp(12f)).coerceAtLeast(0)
        val defaultY = dp(28f)
        var windowX = prefs.getInt(KEY_POSITION_X, defaultX).coerceAtLeast(0)
        var windowY = prefs.getInt(KEY_POSITION_Y, defaultY).coerceAtLeast(0)

        val overlay = Dialog(activity).apply {
            requestWindowFeature(Window.FEATURE_NO_TITLE)
            setCancelable(false)
            setCanceledOnTouchOutside(false)
        }
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            isClickable = false
            setPadding(dp(9f), dp(8f), dp(9f), dp(9f))
            // Keep the list legible without hiding the game underneath it.
            // The window itself is made click-through below.
            background = rounded("#E8FFFEFA", dp(12f), LINE)
            elevation = dp(8f).toFloat()
        }
        val dragHeader = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(dp(1f), 0, 0, dp(3f))
        }
        val titleColumn = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        titleColumn.addView(TextView(activity).apply {
            text = "MATERIALS"
            textSize = 8f
            letterSpacing = 0.09f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(ACCENT))
        })
        titleColumn.addView(TextView(activity).apply {
            text = "完整建筑材料清单"
            textSize = 13f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(INK))
            setPadding(0, dp(1f), 0, 0)
        })
        val close = TextView(activity).apply {
            text = "设置中关闭"
            textSize = 9f
            gravity = Gravity.CENTER
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(ACCENT_DARK))
            background = rounded("#F7EBE3", dp(7f), "#EBD6C8")
            setPadding(dp(7f), 0, dp(7f), 0)
        }
        dragHeader.addView(titleColumn, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        dragHeader.addView(close, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(25f)))
        root.addView(dragHeader)

        val summary = TextView(activity).apply {
            text = "正在统计完整建筑材料…"
            textSize = 9f
            setTextColor(Color.parseColor(MUTED))
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
            setPadding(dp(2f), dp(2f), dp(2f), dp(4f))
        }
        root.addView(summary)

        val rows = ArrayList<MaterialRow>(ROWS_PER_PAGE)
        fun rowBackground(index: Int, enough: Boolean): GradientDrawable = when {
            enough -> rounded("#DDECE2", dp(5f))
            index % 2 == 0 -> rounded("#F6F1E9", dp(5f))
            else -> rounded("#FAF7F1", dp(5f))
        }
        repeat(ROWS_PER_PAGE) { index ->
            val row = LinearLayout(activity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
                background = rowBackground(index, enough = false)
                setPadding(dp(6f), 0, dp(6f), 0)
            }
            val material = TextView(activity).apply {
                textSize = 10f
                setTextColor(Color.parseColor(INK))
                maxLines = 1
                ellipsize = TextUtils.TruncateAt.END
            }
            val count = TextView(activity).apply {
                textSize = 10f
                typeface = Typeface.DEFAULT_BOLD
                gravity = Gravity.END or Gravity.CENTER_VERTICAL
                setTextColor(Color.parseColor(ACCENT_DARK))
                maxLines = 1
                ellipsize = TextUtils.TruncateAt.END
            }
            row.addView(material, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f).apply {
                marginEnd = dp(6f)
            })
            // Keep both required and available counts visible on normal phone
            // widths. The material column retains the flexible remainder.
            row.addView(count, LinearLayout.LayoutParams(dp(108f), ViewGroup.LayoutParams.MATCH_PARENT))
            root.addView(row, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                dp(24f)
            ).apply { if (index > 0) topMargin = dp(2f) })
            rows += MaterialRow(row, material, count)
        }

        val pager = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
            setPadding(0, dp(7f), 0, 0)
        }
        fun pageButton(label: String) = TextView(activity).apply {
            text = label
            textSize = 9f
            gravity = Gravity.CENTER
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(ACCENT_DARK))
            background = rounded(BG, dp(7f), LINE)
            // The Dialog itself remains NOT_TOUCHABLE. Touch delivery for these
            // two views is intentionally handled by the Activity callback.
            isClickable = false
            isFocusable = false
        }
        val previous = pageButton("‹ 上一页")
        val pageLabel = TextView(activity).apply {
            text = "第 1 / 1 页"
            textSize = 9f
            gravity = Gravity.CENTER
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(MUTED))
            maxLines = 1
        }
        val next = pageButton("下一页 ›")
        pager.addView(previous, LinearLayout.LayoutParams(dp(68f), dp(26f)))
        pager.addView(pageLabel, LinearLayout.LayoutParams(0, dp(26f), 1f))
        pager.addView(next, LinearLayout.LayoutParams(dp(68f), dp(26f)))
        root.addView(pager)

        fun renderRows(entries: List<MaterialEntry>) {
            rows.forEachIndexed { index, row ->
                val entry = entries.getOrNull(index)
                if (entry == null) {
                    row.material.text = ""
                    row.count.text = ""
                    row.material.setTextColor(Color.parseColor(INK))
                    row.count.setTextColor(Color.parseColor(ACCENT_DARK))
                    row.container.background = rowBackground(index, enough = false)
                } else {
                    val available = entry.availableCount
                    val enough = available != null && available >= entry.count
                    row.material.text = displayName(entry.name, entry.aux)
                    row.count.text = when (available) {
                        null -> "需 ${formatCount(entry.count)}"
                        else -> "需${formatCount(entry.count)} · 有${formatCount(available)}" +
                            if (enough) " ✓" else ""
                    }
                    row.material.setTextColor(Color.parseColor(
                        if (enough) GREEN else INK
                    ))
                    row.count.setTextColor(Color.parseColor(
                        if (enough) GREEN else ACCENT_DARK
                    ))
                    row.container.background = rowBackground(index, enough)
                }
            }
        }

        var currentPage = 0
        var currentPageCount = 1
        var latestGeneration = Long.MIN_VALUE
        var requestInFlight = false
        var queuedPage: Int? = null
        var closed = false

        fun setPagerEnabled(ready: Boolean) {
            previous.isEnabled = ready && currentPage > 0
            next.isEnabled = ready && currentPage + 1 < currentPageCount
            previous.alpha = if (previous.isEnabled) 1f else 0.42f
            next.alpha = if (next.isEnabled) 1f else 0.42f
        }

        fun render(page: MaterialPage) {
            if (page.generation != Long.MIN_VALUE && latestGeneration != Long.MIN_VALUE &&
                page.generation != latestGeneration && page.pageIndex != 0) {
                currentPage = 0
            }
            if (page.generation != Long.MIN_VALUE) latestGeneration = page.generation
            currentPage = page.pageIndex.coerceAtLeast(0)
            currentPageCount = page.pageCount.coerceAtLeast(1)
            when (page.state) {
                PageState.READY -> {
                    summary.text = "共 ${formatCount(page.totalTypes)} 种 · ${formatCount(page.totalBlocks)} 方块"
                    summary.setTextColor(Color.parseColor(MUTED))
                    renderRows(page.rows)
                    pageLabel.text = "第 ${currentPage + 1} / $currentPageCount 页"
                    setPagerEnabled(true)
                }

                PageState.EMPTY -> {
                    summary.text = "完整建筑中没有可统计的方块"
                    summary.setTextColor(Color.parseColor(MUTED))
                    renderRows(emptyList())
                    pageLabel.text = "第 1 / 1 页"
                    setPagerEnabled(false)
                }

                PageState.LOADING -> {
                    summary.text = page.message.ifBlank { "正在统计完整建筑材料…" }
                    summary.setTextColor(Color.parseColor(WARNING))
                    renderRows(emptyList())
                    pageLabel.text = "统计中"
                    setPagerEnabled(false)
                }

                PageState.ERROR -> {
                    summary.text = page.message.ifBlank { "材料清单暂不可用" }
                    summary.setTextColor(Color.parseColor(ERROR))
                    renderRows(emptyList())
                    pageLabel.text = "暂无数据"
                    setPagerEnabled(false)
                }
            }
            // The source summary is immutable once ready, but the player can
            // consume, pick up, or move materials at any time. Keep a compact
            // low-frequency poll so the green availability state remains live.
        }

        lateinit var requestPage: (Int) -> Unit
        requestPage = { requestedPage ->
            val normalizedPage = requestedPage.coerceAtLeast(0)
            if (closed) {
                Unit
            } else if (requestInFlight) {
                queuedPage = normalizedPage
            } else {
                requestInFlight = true
                val requestId = requestSerial.incrementAndGet()
                Thread({
                    val result = runCatching {
                        TpModule.getBuildProjectionMaterialSummaryPage(normalizedPage, ROWS_PER_PAGE)
                    }.fold(
                        onSuccess = { parsePage(it) },
                        onFailure = { errorPage(it.message) }
                    )
                    handler.post {
                        if (closed || dialog !== overlay || !overlay.isShowing) return@post
                        requestInFlight = false
                        if (requestId < requestSerial.get()) return@post
                        render(result)
                        val nextPage = queuedPage
                        queuedPage = null
                        if (nextPage != null && nextPage != currentPage) {
                            requestPage(nextPage)
                        }
                    }
                }, "projection-material-summary-$requestId").start()
            }
        }

        val pagerLocation = IntArray(2)
        fun containsPagerButton(button: View, rawX: Float, rawY: Float): Boolean {
            if (!button.isShown || !button.isEnabled) return false
            button.getLocationOnScreen(pagerLocation)
            return rawX >= pagerLocation[0] && rawX <= pagerLocation[0] + button.width &&
                rawY >= pagerLocation[1] && rawY <= pagerLocation[1] + button.height
        }

        fun pageRequestAction(targetPage: Int): () -> Unit = {
            requestPage(targetPage)
        }

        fun moveWindow(nextX: Int, nextY: Int, save: Boolean) {
            val maxX = (activity.resources.displayMetrics.widthPixels - panelWidth).coerceAtLeast(0)
            val rootHeight = root.height.takeIf { it > 0 } ?: dp(282f)
            val maxY = (activity.resources.displayMetrics.heightPixels - rootHeight).coerceAtLeast(0)
            windowX = nextX.coerceIn(0, maxX)
            windowY = nextY.coerceIn(0, maxY)
            overlay.window?.attributes = overlay.window?.attributes?.apply {
                x = windowX
                y = windowY
            }
            if (save) {
                prefs.edit().putInt(KEY_POSITION_X, windowX).putInt(KEY_POSITION_Y, windowY).apply()
            }
        }
        overlay.setContentView(root)
        overlay.window?.let { window ->
            window.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            window.clearFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
            window.addFlags(
                WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE or
                    WindowManager.LayoutParams.FLAG_NOT_TOUCH_MODAL or
                    WindowManager.LayoutParams.FLAG_NOT_TOUCHABLE or
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
            return
        }
        overlay.window?.setLayout(panelWidth, ViewGroup.LayoutParams.WRAP_CONTENT)
        dialog = overlay
        ownerActivityRef = WeakReference(activity)
        // The Dialog is intentionally not a touch target. The Activity callback
        // below leaves normal taps/swipes with Minecraft, except for the two
        // compact pager buttons; it also captures a confirmed long-press drag.
        val panelLocation = IntArray(2)
        val passthrough = LongPressDragPassthrough(
            activity = activity,
            isActive = { !closed && dialog === overlay && overlay.isShowing },
            isInsidePanel = { rawX, rawY ->
                if (!root.isShown) {
                    false
                } else {
                    root.getLocationOnScreen(panelLocation)
                    rawX >= panelLocation[0] && rawX <= panelLocation[0] + root.width &&
                        rawY >= panelLocation[1] && rawY <= panelLocation[1] + root.height
                }
            },
            manualTapActionAt = { rawX, rawY ->
                when {
                    containsPagerButton(previous, rawX, rawY) -> {
                        pageRequestAction((currentPage - 1).coerceAtLeast(0))
                    }
                    containsPagerButton(next, rawX, rawY) -> {
                        pageRequestAction((currentPage + 1)
                            .coerceAtMost((currentPageCount - 1).coerceAtLeast(0)))
                    }
                    else -> null
                }
            },
            currentWindowPosition = { windowX to windowY },
            moveWindow = { x, y, save -> moveWindow(x, y, save) }
        )
        touchPassthrough?.dispose()
        touchPassthrough = passthrough
        passthrough.install()
        overlay.setOnDismissListener {
            closed = true
            if (dialog === overlay) {
                ticker?.let(handler::removeCallbacks)
                ticker = null
                touchPassthrough?.dispose()
                touchPassthrough = null
                dialog = null
                ownerActivityRef = null
            }
        }

        val refresh = object : Runnable {
            override fun run() {
                if (closed || dialog !== overlay || !overlay.isShowing ||
                    activity.isFinishing || activity.isDestroyed) {
                    return
                }
                // Do not issue a parallel native query while the fixed current
                // page is being read. The material counts still refresh live.
                if (!requestInFlight) requestPage(currentPage)
                handler.postDelayed(this, POLL_INTERVAL_MS)
            }
        }
        ticker = refresh
        render(loadingPage())
        requestPage(0)
        handler.postDelayed(refresh, POLL_INTERVAL_MS)
    }

    private fun dismissWindow(clearRequested: Boolean) {
        ticker?.let(handler::removeCallbacks)
        ticker = null
        touchPassthrough?.dispose()
        touchPassthrough = null
        if (clearRequested) requestedVisible = false
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
                        dismissWindow(clearRequested = false)
                    }
                }
            }
        }
        application.registerActivityLifecycleCallbacks(callbacks)
        lifecycleCallbacks = callbacks
    }

    private fun parsePage(raw: String?): MaterialPage {
        val lines = raw.orEmpty().lineSequence().map { it.trimEnd('\r') }.toList()
        val header = lines.firstOrNull()?.split('\t') ?: return errorPage("材料清单响应为空")
        if (header.size < 8 || header[0].trim() != "v1") {
            return errorPage("材料清单响应格式无效")
        }
        val status = header[1].trim()
        val generation = header[2].trim().toLongOrNull() ?: Long.MIN_VALUE
        val planIdentity = header[3].trim()
        val totalBlocks = header[4].trim().toLongOrNull()?.coerceAtLeast(0L) ?: 0L
        val totalTypes = header[5].trim().toLongOrNull()?.coerceAtLeast(0L) ?: 0L
        val pageIndex = header[6].trim().toIntOrNull()?.coerceAtLeast(0) ?: 0
        val pageCount = header[7].trim().toIntOrNull()?.coerceAtLeast(1) ?: 1
        val rows = lines.drop(1).mapNotNull { line ->
            val fields = line.split('\t')
            if (fields.size < 3 || fields[0].isBlank()) null else {
                val aux = fields[1].trim().toLongOrNull() ?: 0L
                val count = fields[2].trim().toLongOrNull()?.coerceAtLeast(0L) ?: 0L
                val availableCount = fields.getOrNull(3)?.trim()?.toLongOrNull()
                    ?.takeIf { it >= 0L }
                MaterialEntry(fields[0].trim(), aux, count, availableCount)
            }
        }.take(ROWS_PER_PAGE)
        val normalizedStatus = status.lowercase(Locale.ROOT)
        val state = when {
            normalizedStatus in ERROR_STATUSES -> PageState.ERROR
            normalizedStatus in LOADING_STATUSES -> PageState.LOADING
            normalizedStatus in READY_STATUSES || rows.isNotEmpty() -> {
                if (totalTypes == 0L) PageState.EMPTY else PageState.READY
            }
            totalTypes > 0L -> PageState.READY
            else -> PageState.LOADING
        }
        val message = when (state) {
            PageState.LOADING -> "正在统计完整建筑材料…"
            PageState.ERROR -> statusMessage(normalizedStatus)
            else -> ""
        }
        return MaterialPage(
            state = state,
            status = status,
            generation = generation,
            planIdentity = planIdentity,
            totalBlocks = totalBlocks,
            totalTypes = totalTypes,
            pageIndex = pageIndex.coerceAtMost(pageCount - 1),
            pageCount = pageCount,
            rows = rows,
            message = message
        )
    }

    private fun loadingPage() = MaterialPage(
        state = PageState.LOADING,
        status = "loading",
        generation = Long.MIN_VALUE,
        planIdentity = "",
        totalBlocks = 0L,
        totalTypes = 0L,
        pageIndex = 0,
        pageCount = 1,
        rows = emptyList()
    )

    private fun errorPage(message: String?) = MaterialPage(
        state = PageState.ERROR,
        status = "error",
        generation = Long.MIN_VALUE,
        planIdentity = "",
        totalBlocks = 0L,
        totalTypes = 0L,
        pageIndex = 0,
        pageCount = 1,
        rows = emptyList(),
        message = message?.takeIf { it.isNotBlank() } ?: "材料清单暂不可用"
    )

    /**
     * Localizes only the text painted in the material panel.  [MaterialEntry]
     * deliberately keeps the native identifier and aux untouched: those two
     * values are also the page's stable identity and must never be changed by
     * a UI translation.
     */
    private fun displayName(name: String, aux: Long): String {
        val identifier = name.trim()
            .substringBefore('[')
            .substringBefore('{')
            .substringBefore('|')
        val key = identifier.lowercase(Locale.ROOT).removePrefix("minecraft:")
        val localized = localizedMaterialName(key)
            ?: if (identifier.isBlank()) "未命名方块" else "未翻译方块（$identifier）"
        return if (aux == 0L) localized else "$localized · ${auxDisplayName(key, aux)}"
    }

    private fun localizedMaterialName(key: String): String? =
        // The generated Bedrock zh_CN table is version-matched and also covers
        // state/legacy aliases which arrive from BDX imports.
        MinecraftZhCnTranslations.blockOrItemName(key)
            ?: MATERIAL_NAMES[key]
            ?: localizedColoredMaterialName(key)
            ?: localizedWoodMaterialName(key)
            ?: localizedCopperMaterialName(key)
            ?: localizedDerivedMaterialName(key)

    /** Colour-prefixed blocks share one item per colour in Bedrock. */
    private fun localizedColoredMaterialName(key: String): String? {
        for ((suffix, noun) in COLORED_SUFFIXES) {
            if (!key.endsWith(suffix)) continue
            val color = COLOR_NAMES[key.removeSuffix(suffix)] ?: continue
            return color + noun
        }
        return null
    }

    /**
     * Covers the modern wood families without flattening their identifiers.
     * For example, the raw `spruce_planks` identity remains unchanged while
     * the panel presents it as “云杉木板”.
     */
    private fun localizedWoodMaterialName(key: String): String? {
        if (key.startsWith("stripped_")) {
            return localizedWoodMaterialName(key.removePrefix("stripped_"))?.let { "去皮$it" }
        }
        for ((suffix, noun) in WOOD_SUFFIXES) {
            if (!key.endsWith(suffix)) continue
            val wood = WOOD_NAMES[key.removeSuffix(suffix)] ?: continue
            return wood + noun
        }
        return null
    }

    /** Weathered/waxed copper variants still use the same Chinese material family. */
    private fun localizedCopperMaterialName(key: String): String? = COPPER_NAMES[key]

    /**
     * A large part of the block registry follows a predictable construction
     * naming scheme (base + stairs/slab/wall).  Compose those labels only
     * when the base itself is known, otherwise leave a useful raw-id fallback
     * instead of inventing a possibly misleading material name.
     */
    private fun localizedDerivedMaterialName(key: String): String? {
        // Modern and legacy BDX files use both `foo_double_slab` and
        // `double_foo_slab`.  They frequently have no separate language-table
        // entry, but their base material does; preserve the double-slab meaning
        // instead of showing the raw identifier.
        val doubleSlabBase = when {
            key.endsWith("_double_slab") -> key.removeSuffix("_double_slab")
            key.startsWith("double_") && key.endsWith("_slab") ->
                key.removePrefix("double_").removeSuffix("_slab")
            key.contains("_double_") && key.endsWith("_slab") ->
                key.replaceFirst("_double_", "_").removeSuffix("_slab")
            else -> null
        }
        if (doubleSlabBase != null) {
            localizedMaterialName(doubleSlabBase)?.let { return "${it}双台阶" }
        }

        for ((suffix, noun) in DERIVED_SUFFIXES) {
            if (!key.endsWith(suffix)) continue
            val base = key.removeSuffix(suffix)
            // Go through the full name resolver: otherwise a translated base
            // that exists only in the generated Bedrock language table is
            // accidentally bypassed by this derived-name fallback.
            val localizedBase = localizedMaterialName(base) ?: continue
            return localizedBase + noun
        }
        return null
    }

    private fun auxDisplayName(key: String, aux: Long): String {
        MATERIAL_AUX_NAMES[key]?.get(aux)?.let { return it }
        // These are placement-state bits retained by a few old projection
        // records.  They are presentation-only here; the native aux remains
        // exactly as supplied by the material summary.
        if (key.endsWith("_slab") || key.startsWith("double_") && key.endsWith("slab")) {
            return when {
                (aux and 0x80L) != 0L -> "双层"
                (aux and 0x08L) != 0L -> "上半"
                else -> "变种 $aux"
            }
        }
        return "变种 $aux"
    }

    private fun formatCount(value: Long): String = String.format(Locale.ROOT, "%,d", value.coerceAtLeast(0L))

    private fun statusMessage(status: String): String = when (status) {
        "none", "cleared", "not_loaded", "no_projection", "unavailable" -> "尚未加载建筑投影"
        "failed", "error", "invalid" -> "完整建筑材料统计失败"
        else -> "材料清单暂不可用"
    }

    private fun rounded(fill: String, radius: Int, stroke: String? = null) = GradientDrawable().apply {
        setColor(Color.parseColor(fill))
        cornerRadius = radius.toFloat()
        if (stroke != null) setStroke(1, Color.parseColor(stroke))
    }

    // This dictionary is intentionally a UI-only layer.  Native projection
    // parsing, sorting, printer matching and the associated aux values retain
    // their canonical English identifiers.
    private val MATERIAL_NAMES = mapOf(
        // Natural terrain and ores.
        "air" to "空气",
        "stone" to "石头",
        "granite" to "花岗岩",
        "polished_granite" to "磨制花岗岩",
        "diorite" to "闪长岩",
        "polished_diorite" to "磨制闪长岩",
        "andesite" to "安山岩",
        "polished_andesite" to "磨制安山岩",
        "grass_block" to "草方块",
        "dirt" to "泥土",
        "coarse_dirt" to "砂土",
        "podzol" to "灰化土",
        "rooted_dirt" to "缠根泥土",
        "mud" to "泥巴",
        "packed_mud" to "泥坯",
        "mud_bricks" to "泥砖",
        "mud_brick" to "泥砖",
        "clay" to "黏土块",
        "gravel" to "沙砾",
        "sand" to "沙子",
        "red_sand" to "红沙",
        "sandstone" to "砂岩",
        "chiseled_sandstone" to "錾制砂岩",
        "cut_sandstone" to "切制砂岩",
        "smooth_sandstone" to "平滑砂岩",
        "red_sandstone" to "红砂岩",
        "chiseled_red_sandstone" to "錾制红砂岩",
        "cut_red_sandstone" to "切制红砂岩",
        "smooth_red_sandstone" to "平滑红砂岩",
        "cobblestone" to "圆石",
        "mossy_cobblestone" to "苔石",
        "bedrock" to "基岩",
        "ice" to "冰",
        "packed_ice" to "浮冰",
        "blue_ice" to "蓝冰",
        "snow_block" to "雪块",
        "snow_layer" to "雪",
        "obsidian" to "黑曜石",
        "crying_obsidian" to "哭泣的黑曜石",
        "netherrack" to "下界岩",
        "soul_sand" to "灵魂沙",
        "soul_soil" to "灵魂土",
        "magma_block" to "岩浆块",
        "basalt" to "玄武岩",
        "smooth_basalt" to "平滑玄武岩",
        "polished_basalt" to "磨制玄武岩",
        "blackstone" to "黑石",
        "gilded_blackstone" to "镶金黑石",
        "polished_blackstone" to "磨制黑石",
        "polished_blackstone_bricks" to "磨制黑石砖",
        "polished_blackstone_brick" to "磨制黑石砖",
        "cracked_polished_blackstone_bricks" to "裂纹磨制黑石砖",
        "chiseled_polished_blackstone" to "錾制磨制黑石",
        "deepslate" to "深板岩",
        "cobbled_deepslate" to "深板岩圆石",
        "polished_deepslate" to "磨制深板岩",
        "deepslate_bricks" to "深板岩砖",
        "deepslate_brick" to "深板岩砖",
        "cracked_deepslate_bricks" to "裂纹深板岩砖",
        "deepslate_tiles" to "深板岩瓦",
        "cracked_deepslate_tiles" to "裂纹深板岩瓦",
        "chiseled_deepslate" to "錾制深板岩",
        "reinforced_deepslate" to "强化深板岩",
        "tuff" to "凝灰岩",
        "polished_tuff" to "磨制凝灰岩",
        "tuff_bricks" to "凝灰岩砖",
        "tuff_brick" to "凝灰岩砖",
        "chiseled_tuff" to "錾制凝灰岩",
        "chiseled_tuff_bricks" to "錾制凝灰岩砖",
        "calcite" to "方解石",
        "dripstone_block" to "滴水石块",
        "pointed_dripstone" to "滴水石锥",
        "amethyst_block" to "紫水晶块",
        "budding_amethyst" to "紫水晶母岩",
        "small_amethyst_bud" to "小型紫水晶芽",
        "medium_amethyst_bud" to "中型紫水晶芽",
        "large_amethyst_bud" to "大型紫水晶芽",
        "amethyst_cluster" to "紫水晶簇",
        "coal_ore" to "煤矿石",
        "iron_ore" to "铁矿石",
        "copper_ore" to "铜矿石",
        "gold_ore" to "金矿石",
        "redstone_ore" to "红石矿石",
        "lapis_ore" to "青金石矿石",
        "diamond_ore" to "钻石矿石",
        "emerald_ore" to "绿宝石矿石",
        "nether_gold_ore" to "下界金矿石",
        "nether_quartz_ore" to "下界石英矿石",
        "ancient_debris" to "远古残骸",
        "deepslate_coal_ore" to "深板岩煤矿石",
        "deepslate_iron_ore" to "深板岩铁矿石",
        "deepslate_copper_ore" to "深板岩铜矿石",
        "deepslate_gold_ore" to "深板岩金矿石",
        "deepslate_redstone_ore" to "深板岩红石矿石",
        "deepslate_lapis_ore" to "深板岩青金石矿石",
        "deepslate_diamond_ore" to "深板岩钻石矿石",
        "deepslate_emerald_ore" to "深板岩绿宝石矿石",

        // Masonry, decorative blocks and light sources.
        "bricks" to "砖块",
        "brick_block" to "砖块",
        "stone_bricks" to "石砖",
        "stone_brick" to "石砖",
        "mossy_stone_bricks" to "苔石砖",
        "cracked_stone_bricks" to "裂纹石砖",
        "chiseled_stone_bricks" to "錾制石砖",
        "end_stone" to "末地石",
        "end_stone_bricks" to "末地石砖",
        "end_stone_brick" to "末地石砖",
        "purpur_block" to "紫珀块",
        "purpur_pillar" to "紫珀柱",
        "quartz_block" to "石英块",
        "quartz" to "石英",
        "chiseled_quartz_block" to "錾制石英块",
        "quartz_pillar" to "石英柱",
        "smooth_quartz" to "平滑石英块",
        "prismarine" to "海晶石",
        "prismarine_brick" to "海晶石砖",
        "prismarine_bricks" to "海晶石砖",
        "dark_prismarine" to "暗海晶石",
        "nether_bricks" to "下界砖块",
        "nether_brick" to "下界砖块",
        "red_nether_bricks" to "红色下界砖块",
        "red_nether_brick" to "红色下界砖块",
        "chiseled_nether_bricks" to "錾制下界砖块",
        "cracked_nether_bricks" to "裂纹下界砖块",
        "bone_block" to "骨块",
        "hay_block" to "干草块",
        "sponge" to "海绵",
        "wet_sponge" to "湿海绵",
        "terracotta" to "陶瓦",
        "hardened_clay" to "陶瓦",
        "glass" to "玻璃",
        "glass_pane" to "玻璃板",
        "tinted_glass" to "遮光玻璃",
        "glowstone" to "荧石",
        "sea_lantern" to "海晶灯",
        "shroomlight" to "菌光体",
        "ochre_froglight" to "赭黄蛙明灯",
        "verdant_froglight" to "青翠蛙明灯",
        "pearlescent_froglight" to "珠光蛙明灯",
        "lantern" to "灯笼",
        "soul_lantern" to "灵魂灯笼",
        "torch" to "火把",
        "soul_torch" to "灵魂火把",
        "redstone_torch" to "红石火把",
        "end_rod" to "末地烛",
        "lightning_rod" to "避雷针",
        "chain" to "锁链",
        "iron_bars" to "铁栏杆",
        "cobweb" to "蜘蛛网",
        "slime_block" to "黏液块",
        "honey_block" to "蜂蜜块",
        "honeycomb_block" to "蜜脾块",
        "resin_block" to "树脂块",
        "resin_bricks" to "树脂砖",
        "resin_brick" to "树脂砖",
        "chiseled_resin_bricks" to "錾制树脂砖",

        // Modern plant/wood-adjacent construction materials not covered by a
        // simple tree-family suffix.
        "bamboo_block" to "竹块",
        "stripped_bamboo_block" to "去皮竹块",
        "bamboo_mosaic" to "竹马赛克",
        "mangrove_roots" to "红树根",
        "muddy_mangrove_roots" to "沾泥的红树根",
        "crimson_stem" to "绯红菌柄",
        "warped_stem" to "诡异菌柄",
        "crimson_hyphae" to "绯红菌核",
        "warped_hyphae" to "诡异菌核",
        "crimson_nylium" to "绯红菌岩",
        "warped_nylium" to "诡异菌岩",
        "crimson_fungus" to "绯红菌",
        "warped_fungus" to "诡异菌",
        "crimson_roots" to "绯红菌索",
        "warped_roots" to "诡异菌索",
        "nether_sprouts" to "下界芽",
        "weeping_vines" to "垂泪藤",
        "twisting_vines" to "缠怨藤",
        "pale_moss_block" to "苍白苔藓块",
        "pale_moss_carpet" to "苍白苔藓地毯",
        "pale_hanging_moss" to "苍白垂苔",
        "moss_block" to "苔藓块",
        "moss_carpet" to "苔藓地毯",
        "azalea" to "杜鹃花丛",
        "flowering_azalea" to "盛开的杜鹃花丛",
        "azalea_leaves" to "杜鹃树叶",
        "azalea_leaves_flowered" to "盛开的杜鹃树叶",

        // Functional blocks, containers and redstone materials.
        "crafting_table" to "工作台",
        "furnace" to "熔炉",
        "blast_furnace" to "高炉",
        "smoker" to "烟熏炉",
        "chest" to "箱子",
        "trapped_chest" to "陷阱箱",
        "ender_chest" to "末影箱",
        "barrel" to "木桶",
        "shulker_box" to "潜影盒",
        "undyed_shulker_box" to "潜影盒",
        "hopper" to "漏斗",
        "dispenser" to "发射器",
        "dropper" to "投掷器",
        "crafter" to "合成器",
        "brewing_stand" to "酿造台",
        "enchanting_table" to "附魔台",
        "anvil" to "铁砧",
        "chipped_anvil" to "开裂的铁砧",
        "damaged_anvil" to "损坏的铁砧",
        "grindstone" to "砂轮",
        "smithing_table" to "锻造台",
        "stonecutter" to "切石机",
        "cartography_table" to "制图台",
        "loom" to "织布机",
        "lectern" to "讲台",
        "jukebox" to "唱片机",
        "note_block" to "音符盒",
        "beacon" to "信标",
        "conduit" to "潮涌核心",
        "lodestone" to "磁石",
        "respawn_anchor" to "重生锚",
        "daylight_detector" to "阳光探测器",
        "redstone_block" to "红石块",
        "redstone_lamp" to "红石灯",
        "redstone_wire" to "红石粉",
        "repeater" to "红石中继器",
        "comparator" to "红石比较器",
        "observer" to "侦测器",
        "piston" to "活塞",
        "sticky_piston" to "黏性活塞",
        "piston_head" to "活塞头",
        "lever" to "拉杆",
        "tripwire_hook" to "绊线钩",
        "target" to "标靶",
        "sculk_sensor" to "幽匿感测体",
        "calibrated_sculk_sensor" to "校频幽匿感测体",
        "sculk_shrieker" to "幽匿尖啸体",
        "sculk_catalyst" to "幽匿催发体",
        "heavy_weighted_pressure_plate" to "重质测重压力板",
        "light_weighted_pressure_plate" to "轻质测重压力板",
        "stone_pressure_plate" to "石质压力板",
        "polished_blackstone_pressure_plate" to "磨制黑石压力板",
        "stone_button" to "石质按钮",
        "rail" to "铁轨",
        "powered_rail" to "动力铁轨",
        "detector_rail" to "探测铁轨",
        "activator_rail" to "激活铁轨",
        "command_block" to "命令方块",
        "repeating_command_block" to "循环型命令方块",
        "chain_command_block" to "连锁型命令方块",
        "structure_block" to "结构方块",
        "jigsaw" to "拼图方块",
        // Doors, gates and special decoration whose identifier does not have
        // a usable base material label.
        "iron_door" to "铁门",
        "iron_trapdoor" to "铁活板门",
        "nether_brick_fence" to "下界砖栅栏",
        "cobblestone_wall" to "圆石墙",
        "mossy_cobblestone_wall" to "苔石墙",
        "vine" to "藤蔓",
        "ladder" to "梯子",
        "scaffolding" to "脚手架",
        "cake" to "蛋糕",
        "candle_cake" to "插有蜡烛的蛋糕",
        "pumpkin" to "雕刻过的南瓜",
        "carved_pumpkin" to "雕刻过的南瓜",
        "jack_o_lantern" to "南瓜灯",
        "melon" to "西瓜",
        "lily_pad" to "睡莲",
        "dirt_path" to "土径",
        "farmland" to "耕地",
        "water" to "水",
        "lava" to "岩浆",
        "fire" to "火",
        "soul_fire" to "灵魂火",

        // Flowers and small decorative blocks frequently present in imported
        // gardens.  The generic fallback below still preserves unfamiliar IDs.
        "dandelion" to "蒲公英",
        "poppy" to "虞美人",
        "blue_orchid" to "兰花",
        "allium" to "绒球葱",
        "azure_bluet" to "滨菊",
        "red_tulip" to "红色郁金香",
        "orange_tulip" to "橙色郁金香",
        "white_tulip" to "白色郁金香",
        "pink_tulip" to "粉红色郁金香",
        "oxeye_daisy" to "滨菊",
        "cornflower" to "矢车菊",
        "lily_of_the_valley" to "铃兰",
        "wither_rose" to "凋灵玫瑰",
        "torchflower" to "火把花",
        "pink_petals" to "粉红花瓣",
        "spore_blossom" to "孢子花",
        "brown_mushroom" to "棕色蘑菇",
        "red_mushroom" to "红色蘑菇",
        "cactus" to "仙人掌",
        "sugar_cane" to "甘蔗",
        "bamboo" to "竹子",
        "kelp" to "海带",
        "seagrass" to "海草",
        "dead_bush" to "枯萎的灌木",
        "short_grass" to "短草",
        "tall_grass" to "高草丛",
        "fern" to "蕨",
        "large_fern" to "大型蕨",
        "dead_tube_coral_block" to "失活的管珊瑚块",
        "tube_coral_block" to "管珊瑚块",
        "brain_coral_block" to "脑纹珊瑚块",
        "bubble_coral_block" to "气泡珊瑚块",
        "fire_coral_block" to "火珊瑚块",
        "horn_coral_block" to "鹿角珊瑚块",

        // Rare legacy/runtime-only material identities which do not have a
        // direct entry in the 1.21.120 language file.  These affect display
        // text only; native item ids and aux values remain untouched.
        "bamboo_sapling" to "竹笋",
        "petrified_oak_slab" to "石化橡木台阶",
        "petrified_oak_double_slab" to "石化橡木双台阶",
        "melon_stem" to "西瓜茎",
        "skull" to "头颅",
        "wall_banner" to "墙上的旗帜",
        "piston_arm_collision" to "活塞臂（碰撞方块）",
        "sticky_piston_arm_collision" to "黏性活塞臂（碰撞方块）",
        "stone_block_slab2" to "旧版台阶（第 2 组）",
        "stone_block_slab3" to "旧版台阶（第 3 组）",
        "stone_block_slab4" to "旧版台阶（第 4 组）",
        "double_stone_block_slab2" to "旧版双台阶（第 2 组）",
        "double_stone_block_slab3" to "旧版双台阶（第 3 组）",
        "double_stone_block_slab4" to "旧版双台阶（第 4 组）"
    )

    private val COLOR_NAMES = mapOf(
        "white" to "白色",
        "orange" to "橙色",
        "magenta" to "品红色",
        "light_blue" to "淡蓝色",
        "yellow" to "黄色",
        "lime" to "黄绿色",
        "pink" to "粉红色",
        "gray" to "灰色",
        "light_gray" to "淡灰色",
        // Some old BDX palettes still use Bedrock's former "silver" name.
        "silver" to "淡灰色",
        "cyan" to "青色",
        "purple" to "紫色",
        "blue" to "蓝色",
        "brown" to "棕色",
        "green" to "绿色",
        "red" to "红色",
        "black" to "黑色"
    )

    private val COLORED_SUFFIXES = listOf(
        "_stained_glass_pane" to "染色玻璃板",
        "_glazed_terracotta" to "带釉陶瓦",
        "_concrete_powder" to "混凝土粉末",
        "_stained_glass" to "染色玻璃",
        "_shulker_box" to "潜影盒",
        "_terracotta" to "陶瓦",
        "_concrete" to "混凝土",
        "_carpet" to "地毯",
        "_wool" to "羊毛",
        "_candle" to "蜡烛",
        "_bed" to "床",
        "_banner" to "旗帜"
    )

    private val WOOD_NAMES = mapOf(
        "oak" to "橡木",
        "spruce" to "云杉",
        "birch" to "白桦",
        "jungle" to "丛林",
        "acacia" to "金合欢",
        "dark_oak" to "深色橡木",
        "mangrove" to "红树",
        "cherry" to "樱花",
        "pale_oak" to "苍白橡木",
        "bamboo" to "竹",
        "crimson" to "绯红",
        "warped" to "诡异"
    )

    // Long suffixes have to be listed first: `oak_wall_hanging_sign` must
    // not be consumed by the shorter `_hanging_sign` rule.
    private val WOOD_SUFFIXES = listOf(
        "_wall_hanging_sign" to "墙上的悬挂式告示牌",
        "_hanging_sign" to "悬挂式告示牌",
        "_wall_sign" to "墙上的告示牌",
        "_fence_gate" to "栅栏门",
        "_pressure_plate" to "压力板",
        "_trapdoor" to "活板门",
        "_planks" to "木板",
        "_stairs" to "木楼梯",
        "_slab" to "木台阶",
        "_fence" to "栅栏",
        "_door" to "门",
        "_button" to "按钮",
        "_leaves" to "树叶",
        "_sapling" to "树苗",
        "_hyphae" to "菌核",
        "_stem" to "菌柄",
        "_wood" to "木",
        "_log" to "原木",
        "_sign" to "告示牌",
        "_chest_boat" to "运输船",
        "_boat" to "船"
    )

    private val COPPER_NAMES = mapOf(
        "copper_block" to "铜块",
        "exposed_copper" to "斑驳的铜块",
        "weathered_copper" to "锈蚀的铜块",
        "oxidized_copper" to "氧化的铜块",
        "waxed_copper" to "涂蜡铜块",
        "waxed_exposed_copper" to "涂蜡斑驳铜块",
        "waxed_weathered_copper" to "涂蜡锈蚀铜块",
        "waxed_oxidized_copper" to "涂蜡氧化铜块",
        "cut_copper" to "切制铜块",
        "exposed_cut_copper" to "斑驳的切制铜块",
        "weathered_cut_copper" to "锈蚀的切制铜块",
        "oxidized_cut_copper" to "氧化的切制铜块",
        "waxed_cut_copper" to "涂蜡切制铜块",
        "waxed_exposed_cut_copper" to "涂蜡斑驳切制铜块",
        "waxed_weathered_cut_copper" to "涂蜡锈蚀切制铜块",
        "waxed_oxidized_cut_copper" to "涂蜡氧化切制铜块",
        "chiseled_copper" to "錾制铜块",
        "exposed_chiseled_copper" to "斑驳的錾制铜块",
        "weathered_chiseled_copper" to "锈蚀的錾制铜块",
        "oxidized_chiseled_copper" to "氧化的錾制铜块",
        "waxed_chiseled_copper" to "涂蜡錾制铜块",
        "waxed_exposed_chiseled_copper" to "涂蜡斑驳錾制铜块",
        "waxed_weathered_chiseled_copper" to "涂蜡锈蚀錾制铜块",
        "waxed_oxidized_chiseled_copper" to "涂蜡氧化錾制铜块",
        "copper_grate" to "铜格栅",
        "exposed_copper_grate" to "斑驳的铜格栅",
        "weathered_copper_grate" to "锈蚀的铜格栅",
        "oxidized_copper_grate" to "氧化的铜格栅",
        "waxed_copper_grate" to "涂蜡铜格栅",
        "waxed_exposed_copper_grate" to "涂蜡斑驳铜格栅",
        "waxed_weathered_copper_grate" to "涂蜡锈蚀铜格栅",
        "waxed_oxidized_copper_grate" to "涂蜡氧化铜格栅",
        "copper_bulb" to "铜灯泡",
        "exposed_copper_bulb" to "斑驳的铜灯泡",
        "weathered_copper_bulb" to "锈蚀的铜灯泡",
        "oxidized_copper_bulb" to "氧化的铜灯泡",
        "waxed_copper_bulb" to "涂蜡铜灯泡",
        "waxed_exposed_copper_bulb" to "涂蜡斑驳铜灯泡",
        "waxed_weathered_copper_bulb" to "涂蜡锈蚀铜灯泡",
        "waxed_oxidized_copper_bulb" to "涂蜡氧化铜灯泡",
        "copper_door" to "铜门",
        "exposed_copper_door" to "斑驳的铜门",
        "weathered_copper_door" to "锈蚀的铜门",
        "oxidized_copper_door" to "氧化的铜门",
        "waxed_copper_door" to "涂蜡铜门",
        "waxed_exposed_copper_door" to "涂蜡斑驳铜门",
        "waxed_weathered_copper_door" to "涂蜡锈蚀铜门",
        "waxed_oxidized_copper_door" to "涂蜡氧化铜门",
        "copper_trapdoor" to "铜活板门",
        "exposed_copper_trapdoor" to "斑驳的铜活板门",
        "weathered_copper_trapdoor" to "锈蚀的铜活板门",
        "oxidized_copper_trapdoor" to "氧化的铜活板门",
        "waxed_copper_trapdoor" to "涂蜡铜活板门",
        "waxed_exposed_copper_trapdoor" to "涂蜡斑驳铜活板门",
        "waxed_weathered_copper_trapdoor" to "涂蜡锈蚀铜活板门",
        "waxed_oxidized_copper_trapdoor" to "涂蜡氧化铜活板门"
    )

    private val DERIVED_SUFFIXES = listOf(
        "_pressure_plate" to "压力板",
        "_trapdoor" to "活板门",
        "_fence_gate" to "栅栏门",
        "_stairs" to "楼梯",
        "_slab" to "台阶",
        "_wall" to "墙",
        "_fence" to "栅栏",
        "_door" to "门",
        "_button" to "按钮"
    )

    // Legacy source records can legitimately retain a non-zero material aux.
    // Translate verified classic variants rather than displaying an English ID.
    private val MATERIAL_AUX_NAMES: Map<String, Map<Long, String>> = mapOf(
        "stone" to mapOf(
            0L to "石头", 1L to "花岗岩", 2L to "磨制花岗岩", 3L to "闪长岩",
            4L to "磨制闪长岩", 5L to "安山岩", 6L to "磨制安山岩"
        ),
        "dirt" to mapOf(0L to "泥土", 1L to "砂土", 2L to "灰化土"),
        "sand" to mapOf(0L to "沙子", 1L to "红沙"),
        "planks" to mapOf(
            0L to "橡木木板", 1L to "云杉木板", 2L to "白桦木板",
            3L to "丛林木板", 4L to "金合欢木板", 5L to "深色橡木木板"
        ),
        "wooden_planks" to mapOf(
            0L to "橡木木板", 1L to "云杉木板", 2L to "白桦木板",
            3L to "丛林木板", 4L to "金合欢木板", 5L to "深色橡木木板"
        ),
        "sandstone" to mapOf(0L to "砂岩", 1L to "錾制砂岩", 2L to "平滑砂岩"),
        "red_sandstone" to mapOf(0L to "红砂岩", 1L to "錾制红砂岩", 2L to "平滑红砂岩"),
        "sponge" to mapOf(0L to "海绵", 1L to "湿海绵"),
        "quartz_block" to mapOf(
            0L to "石英块", 1L to "錾制石英块", 2L to "石英柱",
            3L to "平滑石英块", 6L to "石英柱", 10L to "石英柱"
        ),
        "prismarine" to mapOf(0L to "海晶石", 1L to "海晶石砖", 2L to "暗海晶石")
    )

    private val READY_STATUSES = setOf("ready", "ok", "loaded", "available", "complete", "success")
    private val LOADING_STATUSES = setOf("loading", "building", "parsing", "collecting", "pending", "not_ready")
    private val ERROR_STATUSES = setOf(
        "none", "cleared", "not_loaded", "no_projection", "unavailable", "failed", "error", "invalid"
    )
}
