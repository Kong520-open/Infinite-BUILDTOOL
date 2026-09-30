package com.vdl.kong520.ui

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
import android.text.InputType
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.view.WindowManager
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.SeekBar
import android.widget.TextView
import android.widget.Toast
import com.vdl.kong520.TpModule
import java.io.File
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference

/** Standalone native controls for loading and displaying a building projection. */
object BuildProjectionUi {
    private const val BG = "#FFFEFA"
    private const val SURFACE = "#FAF7F1"
    private const val ACCENT = "#CF6846"
    private const val ACCENT_DARK = "#AA4E31"
    private const val INK = "#302D29"
    private const val MUTED = "#77716A"
    private const val LINE = "#E7E0D7"
    private const val GREEN = "#3F765C"

    private const val PREFS = "build_projection_ui"
    private const val KEY_ENABLED = "enabled"
    private const val KEY_REACHABILITY_PREVIEW = "reachability_preview"
    private const val KEY_OUTLINE_ENABLED = "outline_enabled"
    private const val KEY_ALPHA_PERCENT = "alpha_percent"
    private const val KEY_RANGE_CHUNKS = "range_chunks"
    private const val KEY_SOURCE_PATH = "source_path"
    private const val KEY_BASE_X = "base_x"
    private const val KEY_BASE_Y = "base_y"
    private const val KEY_BASE_Z = "base_z"
    private const val KEY_ROTATION = "rotation"
    private const val KEY_PIXEL_WIDTH = "pixel_width"
    private const val KEY_LAYER_MODE = "layer_mode"
    private const val KEY_LAYER_WORLD_SPACE = "layer_world_space"
    private const val KEY_LAYER_START = "layer_start"
    private const val KEY_LAYER_END = "layer_end"

    private const val MIN_ALPHA_PERCENT = 5
    private const val MAX_ALPHA_PERCENT = 100
    private const val MIN_RANGE_CHUNKS = 1
    private const val MAX_RANGE_CHUNKS = 64
    private const val DEFAULT_PIXEL_WIDTH = 64
    private const val MAX_PIXEL_WIDTH = 4096

    private const val LAYER_MODE_ALL = 0
    private const val LAYER_MODE_ABOVE = 1
    private const val LAYER_MODE_BELOW = 2
    private const val LAYER_MODE_SINGLE = 3
    private const val LAYER_MODE_RANGE = 4

    private val pixelArtExtensions = setOf("png", "jpg", "jpeg")
    private val supportedExtensions =
        setOf("schematic", "schem", "litematic", "bdx", "mcworld", "infinity", "ibuild") +
            pixelArtExtensions
    private val operationInFlight = AtomicBoolean(false)
    private val operationGeneration = AtomicLong(0)
    private val activeLoadThread = AtomicReference<Thread?>(null)
    @Volatile private var activeDialog: Dialog? = null

    fun show(activity: Activity, context: Context) {
        val appContext = context.applicationContext
        activeDialog?.let { dialog ->
            if (dialog.isShowing) return
            activeDialog = null
        }
        if (operationInFlight.get()) {
            Toast.makeText(appContext, "建筑投影正在处理，请稍候", Toast.LENGTH_SHORT).show()
            return
        }
        val prefs = appContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val enabled = prefs.getBoolean(KEY_ENABLED, true)
        val reachabilityPreviewEnabled = prefs.getBoolean(KEY_REACHABILITY_PREVIEW, false)
        val outlineEnabled = prefs.getBoolean(KEY_OUTLINE_ENABLED, true)
        val alphaPercent = prefs.getInt(KEY_ALPHA_PERCENT, 25)
            .coerceIn(MIN_ALPHA_PERCENT, MAX_ALPHA_PERCENT)
        val rangeChunks = prefs.getInt(KEY_RANGE_CHUNKS, 12)
            .coerceIn(MIN_RANGE_CHUNKS, MAX_RANGE_CHUNKS)
        val initialPrinterConfig = ProjectionPrinterSettings.read(appContext)
        val initialMaterialPreviewEnabled = BuildProjectionMaterialPreviewOverlay.isEnabled(appContext)
        var selectedRotation = prefs.getInt(KEY_ROTATION, 0).takeIf { it in ROTATIONS } ?: 0
        var selectedLayerMode = prefs.getInt(KEY_LAYER_MODE, LAYER_MODE_ALL)
            .coerceIn(LAYER_MODE_ALL, LAYER_MODE_RANGE)
        val density = activity.resources.displayMetrics.density
        val screenWidthPx = activity.resources.displayMetrics.widthPixels
        val screenHeightPx = activity.resources.displayMetrics.heightPixels
        val useWideLayout = screenWidthPx / density >= 600f && screenWidthPx > screenHeightPx
        val compactWideLayout = useWideLayout && screenHeightPx / density < 460f
        val dialog = Dialog(activity).apply { requestWindowFeature(Window.FEATURE_NO_TITLE) }
        val cancelRequested = AtomicBoolean(false)
        val capturePositionInFlight = AtomicBoolean(false)
        val application = activity.application
        var lifecycleRegistered = false
        lateinit var lifecycleCallbacks: Application.ActivityLifecycleCallbacks
        lateinit var status: TextView
        val statusHandler = Handler(Looper.getMainLooper())
        var statusPollOperation = Long.MIN_VALUE
        var statusPollRunnable: Runnable? = null

        fun stopStatusPolling(operation: Long? = null) {
            if (operation != null && statusPollOperation != operation) return
            statusPollRunnable?.let(statusHandler::removeCallbacks)
            statusPollRunnable = null
            statusPollOperation = Long.MIN_VALUE
        }

        fun startStatusPolling(operation: Long) {
            stopStatusPolling()
            statusPollOperation = operation
            val poll = object : Runnable {
                override fun run() {
                    if (statusPollOperation != operation ||
                        operationGeneration.get() != operation ||
                        !operationInFlight.get() ||
                        activity.isFinishing || activity.isDestroyed || !dialog.isShowing) {
                        stopStatusPolling(operation)
                        return
                    }
                    val nativeStatus = readNativeStatus()
                    if (nativeStatus.isNotBlank()) status.text = nativeStatus
                    statusHandler.postDelayed(this, 300L)
                }
            }
            statusPollRunnable = poll
            statusHandler.postDelayed(poll, 300L)
        }

        fun unregisterLifecycleCallbacks() {
            if (!lifecycleRegistered) return
            lifecycleRegistered = false
            application.unregisterActivityLifecycleCallbacks(lifecycleCallbacks)
        }

        fun releaseDialogAndCancelOperation() {
            val ownsActiveDialog = activeDialog === dialog
            if (ownsActiveDialog) activeDialog = null
            ProjectionImagePicker.cancel(activity)
            unregisterLifecycleCallbacks()
            stopStatusPolling()
            if (ownsActiveDialog && operationInFlight.get() &&
                cancelRequested.compareAndSet(false, true)) {
                val cancelOperation = operationGeneration.incrementAndGet()
                activeLoadThread.getAndSet(null)?.interrupt()
                Thread({
                    BuildProjectionMaterialPreviewOverlay.onProjectionCleared()
                    runCatching { TpModule.clearBuildProjection() }
                    if (operationGeneration.get() == cancelOperation) {
                        cancelRequested.set(false)
                        operationInFlight.set(false)
                    }
                }, "build-projection-dismiss-cancel").start()
            }
        }

        lifecycleCallbacks = object : Application.ActivityLifecycleCallbacks {
            override fun onActivityCreated(target: Activity, savedInstanceState: Bundle?) = Unit
            override fun onActivityStarted(target: Activity) = Unit
            override fun onActivityResumed(target: Activity) = Unit
            override fun onActivityPaused(target: Activity) = Unit
            override fun onActivityStopped(target: Activity) = Unit
            override fun onActivitySaveInstanceState(target: Activity, outState: Bundle) = Unit

            override fun onActivityDestroyed(target: Activity) {
                if (target !== activity) return
                releaseDialogAndCancelOperation()
                if (dialog.isShowing) dialog.dismiss()
            }
        }
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(
                dp(activity, if (compactWideLayout) 14f else 20f),
                dp(activity, if (compactWideLayout) 10f else 16f),
                dp(activity, if (compactWideLayout) 14f else 20f),
                dp(activity, if (compactWideLayout) 10f else 16f)
            )
            background = rounded(BG, dp(activity, 20f), LINE)
        }
        val header = LinearLayout(activity).apply { gravity = Gravity.TOP }
        val titleColumn = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        titleColumn.addView(TextView(activity).apply {
            text = "BUILD PROJECTION"
            textSize = 10f
            letterSpacing = 0.12f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(ACCENT))
        })
        titleColumn.addView(TextView(activity).apply {
            text = "建筑投影"
            textSize = 21f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(INK))
            setPadding(0, dp(activity, 3f), 0, 0)
        })
        titleColumn.addView(TextView(activity).apply {
            text = "加载建筑文件，在当前世界中实时叠加预览"
            textSize = 11f
            setTextColor(Color.parseColor(MUTED))
            setPadding(0, dp(activity, 4f), 0, 0)
        })
        val close = TextView(activity).apply {
            text = "×"
            textSize = 24f
            gravity = Gravity.CENTER
            setTextColor(Color.parseColor(MUTED))
            background = rounded(SURFACE, dp(activity, 12f), LINE)
            setOnClickListener { dialog.dismiss() }
        }
        header.addView(titleColumn, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        header.addView(close, LinearLayout.LayoutParams(dp(activity, 40f), dp(activity, 40f)))
        root.addView(header)

        val primaryContent = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(activity, 6f), 0, 0)
        }
        val displayContent = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(activity, 6f), 0, 0)
        }

        primaryContent.addView(sectionLabel(activity, "01 · 投影文件"))
        val sourcePath = input(activity, prefs.getString(KEY_SOURCE_PATH, "").orEmpty(), false).apply {
            hint = "尚未选择文件"
            isFocusable = false
            isClickable = false
        }
        primaryContent.addView(sourcePath, fieldParams(activity))
        val sourceActions = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        val chooseFile = actionButton(activity, "选择建筑文件")
        val chooseImage = actionButton(activity, "选择 PNG/JPG 图片")
        sourceActions.addView(chooseFile, LinearLayout.LayoutParams(0, dp(activity, 38f), 1f).apply {
            marginEnd = dp(activity, 6f)
        })
        sourceActions.addView(chooseImage, LinearLayout.LayoutParams(0, dp(activity, 38f), 1f))
        primaryContent.addView(sourceActions, buttonParams(activity))

        primaryContent.addView(sectionLabel(activity, "02 · 投影锚点"))
        val coordinates = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        val x = coordinateInput(activity, "X", prefs.getInt(KEY_BASE_X, 0))
        val y = coordinateInput(activity, "Y", prefs.getInt(KEY_BASE_Y, 64))
        val z = coordinateInput(activity, "Z", prefs.getInt(KEY_BASE_Z, 0))
        listOf(x, y, z).forEachIndexed { index, field ->
            coordinates.addView(
                field,
                LinearLayout.LayoutParams(0, dp(activity, 48f), 1f).apply {
                    if (index < 2) marginEnd = dp(activity, 6f)
                }
            )
        }
        primaryContent.addView(coordinates)
        val capturePosition = actionButton(activity, "获取自身坐标")
        primaryContent.addView(capturePosition, buttonParams(activity))

        primaryContent.addView(sectionLabel(activity, "03 · 水平旋转"))
        val rotationRow = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        val rotationButtons = ROTATIONS.associateWith { degrees ->
            actionButton(activity, "$degrees\u00b0").also { button ->
                rotationRow.addView(
                    button,
                    LinearLayout.LayoutParams(0, dp(activity, 38f), 1f).apply {
                        if (degrees != ROTATIONS.last()) marginEnd = dp(activity, 5f)
                    }
                )
            }
        }
        primaryContent.addView(rotationRow)

        primaryContent.addView(sectionLabel(activity, "PNG/JPG 像素画宽度"))
        val pixelWidth = input(
            activity,
            prefs.getInt(KEY_PIXEL_WIDTH, DEFAULT_PIXEL_WIDTH).toString(),
            true
        )
        primaryContent.addView(pixelWidth, fieldParams(activity))

        status = TextView(activity).apply {
            textSize = 12f
            setTextColor(Color.parseColor(MUTED))
            setPadding(dp(activity, 12f), dp(activity, 10f), dp(activity, 12f), dp(activity, 10f))
            background = rounded(SURFACE, dp(activity, 10f), LINE)
            text = readNativeStatus()
            maxLines = if (compactWideLayout) 2 else 3
            ellipsize = TextUtils.TruncateAt.END
            setOnClickListener { Toast.makeText(appContext, this.text, Toast.LENGTH_LONG).show() }
        }

        val projectionActions = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        val load = actionButton(activity, "加载投影", accent = true)
        val clear = actionButton(activity, "清除投影")
        projectionActions.addView(load, LinearLayout.LayoutParams(0, dp(activity, 42f), 1f).apply {
            marginEnd = dp(activity, 6f)
        })
        projectionActions.addView(clear, LinearLayout.LayoutParams(0, dp(activity, 42f), 1f))

        displayContent.addView(sectionLabel(activity, "04 · 显示设置"))
        val showProjection = CheckBox(activity).apply {
            text = "显示建筑投影"
            textSize = 14f
            setTextColor(Color.parseColor(INK))
            buttonTintList = ColorStateList.valueOf(Color.parseColor(ACCENT))
            isChecked = enabled
        }
        displayContent.addView(showProjection)
        val showReachabilityPreview = CheckBox(activity).apply {
            text = "预览当前可放置位置"
            textSize = 14f
            setTextColor(Color.parseColor(INK))
            buttonTintList = ColorStateList.valueOf(Color.parseColor(GREEN))
            isChecked = reachabilityPreviewEnabled
        }
        displayContent.addView(showReachabilityPreview)
        displayContent.addView(TextView(activity).apply {
            text = "以亮绿色标出当前显示范围内、距离可达、目标为空且已有合法支撑面的投影方块；仅作预览，不会自动放置"
            textSize = 10f
            setTextColor(Color.parseColor(MUTED))
            setPadding(dp(activity, 4f), 0, dp(activity, 4f), dp(activity, 2f))
        })

        displayContent.addView(sectionLabel(activity, "完整材料预览"))
        val showMaterialPreview = CheckBox(activity).apply {
            text = "显示完整建筑材料清单"
            textSize = 14f
            setTextColor(Color.parseColor(INK))
            buttonTintList = ColorStateList.valueOf(Color.parseColor(ACCENT))
            isChecked = initialMaterialPreviewEnabled
        }
        displayContent.addView(showMaterialPreview)
        displayContent.addView(TextView(activity).apply {
            text = "统计整个建筑的全部材料，不受当前渲染范围或层筛选影响；材料较多时可翻页"
            textSize = 10f
            setTextColor(Color.parseColor(MUTED))
            setPadding(dp(activity, 4f), 0, dp(activity, 4f), dp(activity, 2f))
        })

        displayContent.addView(sectionLabel(activity, "投影打印机"))
        val enablePrinter = CheckBox(activity).apply {
            text = "启用投影打印机"
            textSize = 14f
            setTextColor(Color.parseColor(INK))
            buttonTintList = ColorStateList.valueOf(Color.parseColor(ACCENT))
            isChecked = initialPrinterConfig.enabled
        }
        displayContent.addView(enablePrinter)
        displayContent.addView(TextView(activity).apply {
            text = "仅处理当前投影显示范围内、可合法支撑且当前热栏有材料的普通方块"
            textSize = 10f
            setTextColor(Color.parseColor(MUTED))
            setPadding(dp(activity, 4f), 0, dp(activity, 4f), dp(activity, 2f))
        })
        val printerRateLabel = valueLabel(
            activity,
            "打印速度上限 · ${initialPrinterConfig.blocksPerSecond} 方块/秒"
        )
        printerRateLabel.setPadding(dp(activity, 4f), dp(activity, 6f), 0, 0)
        displayContent.addView(printerRateLabel)
        val printerRate = SeekBar(activity).apply {
            max = ProjectionPrinterSettings.MAX_RATE - ProjectionPrinterSettings.MIN_RATE
            progress = (initialPrinterConfig.blocksPerSecond - ProjectionPrinterSettings.MIN_RATE)
                .coerceIn(0, max)
            progressTintList = ColorStateList.valueOf(Color.parseColor(ACCENT))
            thumbTintList = ColorStateList.valueOf(Color.parseColor(ACCENT))
        }
        displayContent.addView(printerRate, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            dp(activity, 34f)
        ))

        val showOutline = CheckBox(activity).apply {
            text = "显示方块描边"
            textSize = 14f
            setTextColor(Color.parseColor(INK))
            buttonTintList = ColorStateList.valueOf(Color.parseColor(ACCENT))
            isChecked = outlineEnabled
        }
        displayContent.addView(showOutline)

        val alphaLabel = valueLabel(activity, "填充透明度 · $alphaPercent%")
        displayContent.addView(alphaLabel)
        val alpha = SeekBar(activity).apply {
            max = MAX_ALPHA_PERCENT - MIN_ALPHA_PERCENT
            progress = alphaPercent - MIN_ALPHA_PERCENT
            progressTintList = ColorStateList.valueOf(Color.parseColor(ACCENT))
            thumbTintList = ColorStateList.valueOf(Color.parseColor(ACCENT))
        }
        displayContent.addView(alpha, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(activity, 38f)))

        val rangeLabel = valueLabel(activity, "渲染距离 · $rangeChunks 区块")
        rangeLabel.setPadding(dp(activity, 4f), dp(activity, 8f), 0, 0)
        displayContent.addView(rangeLabel)
        val range = SeekBar(activity).apply {
            max = MAX_RANGE_CHUNKS - MIN_RANGE_CHUNKS
            progress = rangeChunks - MIN_RANGE_CHUNKS
            progressTintList = ColorStateList.valueOf(Color.parseColor(ACCENT))
            thumbTintList = ColorStateList.valueOf(Color.parseColor(ACCENT))
        }
        displayContent.addView(range, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(activity, 38f)))

        displayContent.addView(sectionLabel(activity, "渲染层"))
        val layerModeRow = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        val layerModes = linkedMapOf(
            LAYER_MODE_ALL to "全部",
            LAYER_MODE_ABOVE to "以上",
            LAYER_MODE_BELOW to "以下",
            LAYER_MODE_SINGLE to "单层",
            LAYER_MODE_RANGE to "范围"
        )
        val layerModeButtons = layerModes.mapValues { (mode, label) ->
            actionButton(activity, label).also { button ->
                layerModeRow.addView(
                    button,
                    LinearLayout.LayoutParams(0, dp(activity, 38f), 1f).apply {
                        if (mode != LAYER_MODE_RANGE) marginEnd = dp(activity, 4f)
                    }
                )
            }
        }
        displayContent.addView(layerModeRow)

        val worldLayerSpace = CheckBox(activity).apply {
            text = "使用世界 Y 坐标"
            textSize = 13f
            setTextColor(Color.parseColor(INK))
            buttonTintList = ColorStateList.valueOf(Color.parseColor(ACCENT))
            isChecked = prefs.getBoolean(KEY_LAYER_WORLD_SPACE, false)
        }
        displayContent.addView(worldLayerSpace)

        val layerFields = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        val layerStart = input(
            activity,
            prefs.getInt(KEY_LAYER_START, 1).toString(),
            true
        ).apply { hint = "起始层" }
        val layerEnd = input(
            activity,
            prefs.getInt(KEY_LAYER_END, 1).toString(),
            true
        ).apply { hint = "结束层" }
        layerFields.addView(layerStart, LinearLayout.LayoutParams(0, dp(activity, 46f), 1f).apply {
            marginEnd = dp(activity, 6f)
        })
        layerFields.addView(layerEnd, LinearLayout.LayoutParams(0, dp(activity, 46f), 1f))
        displayContent.addView(layerFields)
        val applyLayers = actionButton(activity, "应用层设置")
        displayContent.addView(applyLayers, buttonParams(activity))

        fun scrollPane(pane: LinearLayout) = ScrollView(activity).apply {
            isFillViewport = true
            isVerticalScrollBarEnabled = false
            background = rounded(SURFACE, dp(activity, 12f), LINE)
            setPadding(dp(activity, 12f), dp(activity, 5f), dp(activity, 12f), dp(activity, 12f))
            clipToPadding = false
            addView(pane)
        }
        if (useWideLayout) {
            val contentRow = LinearLayout(activity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.TOP
            }
            contentRow.addView(
                scrollPane(primaryContent),
                LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f).apply {
                    marginEnd = dp(activity, 12f)
                }
            )
            contentRow.addView(
                scrollPane(displayContent),
                LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f)
            )
            root.addView(contentRow, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1f
            ).apply { topMargin = dp(activity, 4f) })
        } else {
            val singlePane = LinearLayout(activity).apply {
                orientation = LinearLayout.VERTICAL
                addView(primaryContent)
                addView(displayContent)
            }
            root.addView(scrollPane(singlePane), LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1f
            ).apply { topMargin = dp(activity, 4f) })
        }

        root.addView(status, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = dp(activity, 8f) })
        root.addView(projectionActions, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            dp(activity, 42f)
        ).apply { topMargin = dp(activity, 7f) })

        fun updateRotationButtons() {
            rotationButtons.forEach { (degrees, button) ->
                val selected = degrees == selectedRotation
                button.setTextColor(Color.parseColor(if (selected) ACCENT_DARK else INK))
                button.background = rounded(
                    if (selected) "#F7EBE3" else BG,
                    dp(activity, 9f),
                    if (selected) ACCENT else LINE
                )
            }
        }

        fun updateLayerControls() {
            layerModeButtons.forEach { (mode, button) ->
                val selected = mode == selectedLayerMode
                button.setTextColor(Color.parseColor(if (selected) ACCENT_DARK else INK))
                button.background = rounded(
                    if (selected) "#F7EBE3" else BG,
                    dp(activity, 9f),
                    if (selected) ACCENT else LINE
                )
            }
            val needsValue = selectedLayerMode != LAYER_MODE_ALL
            val needsEnd = selectedLayerMode == LAYER_MODE_RANGE
            layerStart.isEnabled = needsValue
            layerEnd.isEnabled = needsEnd
            layerStart.alpha = if (needsValue) 1f else 0.45f
            layerEnd.alpha = if (needsEnd) 1f else 0.45f
            layerStart.hint = when (selectedLayerMode) {
                LAYER_MODE_BELOW -> "截止层"
                LAYER_MODE_SINGLE -> "指定层"
                else -> "起始层"
            }
            applyLayers.isEnabled = true
        }

        fun applyLayerFilter(showError: Boolean): Boolean {
            val worldSpace = worldLayerSpace.isChecked
            val firstUi = layerStart.text.toString().toIntOrNull()
            val secondUi = layerEnd.text.toString().toIntOrNull()
            if (selectedLayerMode != LAYER_MODE_ALL &&
                (firstUi == null || (!worldSpace && firstUi < 1))) {
                if (showError) Toast.makeText(appContext, "请输入有效的层数", Toast.LENGTH_SHORT).show()
                return false
            }
            if (selectedLayerMode == LAYER_MODE_RANGE &&
                (secondUi == null || (!worldSpace && secondUi < 1) || firstUi!! > secondUi)) {
                if (showError) Toast.makeText(appContext, "结束层必须不小于起始层", Toast.LENGTH_SHORT).show()
                return false
            }
            val firstNative = when {
                firstUi == null -> 0
                worldSpace -> firstUi
                else -> firstUi - 1
            }
            val secondNative = when {
                secondUi == null -> firstNative
                worldSpace -> secondUi
                else -> secondUi - 1
            }
            prefs.edit()
                .putInt(KEY_LAYER_MODE, selectedLayerMode)
                .putBoolean(KEY_LAYER_WORLD_SPACE, worldSpace)
                .putInt(KEY_LAYER_START, firstUi ?: 1)
                .putInt(KEY_LAYER_END, secondUi ?: firstUi ?: 1)
                .apply()
            runCatching {
                TpModule.setBuildProjectionLayerFilter(
                    selectedLayerMode,
                    worldSpace,
                    firstNative,
                    if (selectedLayerMode == LAYER_MODE_RANGE) secondNative else firstNative
                )
            }.onFailure {
                if (showError) Toast.makeText(appContext, "无法应用渲染层设置", Toast.LENGTH_SHORT).show()
                return false
            }
            return true
        }

        fun setOperationState(inFlight: Boolean) {
            val captureBusy = capturePositionInFlight.get()
            val formBusy = inFlight || captureBusy
            load.isEnabled = !formBusy
            clear.isEnabled = if (inFlight) !cancelRequested.get() else !captureBusy
            clear.text = if (inFlight && !cancelRequested.get()) "取消并清除" else "清除投影"
            chooseFile.isEnabled = !inFlight
            chooseImage.isEnabled = !inFlight
            coordinates.isEnabled = !formBusy
            x.isEnabled = !formBusy
            y.isEnabled = !formBusy
            z.isEnabled = !formBusy
            capturePosition.isEnabled = !formBusy
            capturePosition.text = if (captureBusy) "正在获取坐标…" else "获取自身坐标"
            pixelWidth.isEnabled = !inFlight
            rotationButtons.values.forEach { it.isEnabled = !inFlight }
            enablePrinter.isEnabled = !inFlight
            printerRate.isEnabled = !inFlight
            val opacity = if (inFlight) 0.55f else 1f
            val formOpacity = if (formBusy) 0.55f else 1f
            load.alpha = formOpacity
            clear.alpha = if (clear.isEnabled) 1f else 0.55f
            chooseFile.alpha = opacity
            chooseImage.alpha = opacity
            enablePrinter.alpha = opacity
            printerRate.alpha = opacity
            coordinates.alpha = formOpacity
            capturePosition.alpha = formOpacity
            close.isEnabled = !inFlight
            close.alpha = opacity
            dialog.setCancelable(!inFlight)
            dialog.setCanceledOnTouchOutside(!inFlight)
        }

        fun updatePngState(path: String) {
            val extension = File(path).extension.lowercase(Locale.ROOT)
            val isPixelArt = extension in pixelArtExtensions
            pixelWidth.isEnabled = isPixelArt && !operationInFlight.get()
            pixelWidth.alpha = if (isPixelArt) 1f else 0.45f
        }

        rotationButtons.forEach { (degrees, button) ->
            button.setOnClickListener {
                selectedRotation = degrees
                prefs.edit().putInt(KEY_ROTATION, degrees).apply()
                updateRotationButtons()
            }
        }
        layerModeButtons.forEach { (mode, button) ->
            button.setOnClickListener {
                selectedLayerMode = mode
                updateLayerControls()
                if (mode == LAYER_MODE_ALL) applyLayerFilter(false)
            }
        }
        worldLayerSpace.setOnCheckedChangeListener { _, _ ->
            applyLayerFilter(false)
        }
        applyLayers.setOnClickListener { applyLayerFilter(true) }
        updateRotationButtons()
        updateLayerControls()
        updatePngState(sourcePath.text.toString())

        chooseFile.setOnClickListener {
            showNeteaseFileList(activity, appContext, useWideLayout) { selected ->
                sourcePath.setText(selected.absolutePath)
                prefs.edit().putString(KEY_SOURCE_PATH, selected.absolutePath).apply()
                updatePngState(selected.absolutePath)
            }
        }

        chooseImage.setOnClickListener {
            if (operationInFlight.get()) return@setOnClickListener
            ProjectionImagePicker.launch(
                activity,
                appContext,
                onSelected = { selected ->
                    if (activeDialog !== dialog || !dialog.isShowing) return@launch
                    sourcePath.setText(selected.absolutePath)
                    prefs.edit().putString(KEY_SOURCE_PATH, selected.absolutePath).apply()
                    updatePngState(selected.absolutePath)
                    Toast.makeText(appContext, "已选择 ${selected.name}", Toast.LENGTH_SHORT).show()
                },
                onError = { message ->
                    if (activeDialog === dialog && dialog.isShowing) {
                        Toast.makeText(appContext, message, Toast.LENGTH_LONG).show()
                    }
                }
            )
        }

        capturePosition.setOnClickListener {
            if (operationInFlight.get() ||
                !capturePositionInFlight.compareAndSet(false, true)) {
                return@setOnClickListener
            }
            setOperationState(false)
            updatePngState(sourcePath.text.toString())
            Thread({
                val position = runCatching { TpModule.getBuildExportPlayerBlockPosition() }
                    .getOrNull()
                    ?.takeIf { it.size >= 3 }
                capturePositionInFlight.set(false)
                statusHandler.post {
                    if (activeDialog !== dialog || activity.isFinishing || activity.isDestroyed ||
                        !dialog.isShowing) {
                        return@post
                    }
                    setOperationState(operationInFlight.get())
                    updatePngState(sourcePath.text.toString())
                    if (position == null) {
                        Toast.makeText(
                            appContext,
                            "无法读取玩家位置，请先进入已加载的世界",
                            Toast.LENGTH_SHORT
                        ).show()
                    } else {
                        x.setText(position[0].toString())
                        y.setText(position[1].toString())
                        z.setText(position[2].toString())
                        Toast.makeText(appContext, "已填入玩家当前位置", Toast.LENGTH_SHORT).show()
                    }
                }
            }, "build-projection-capture-position").start()
        }

        load.setOnClickListener {
            val source = sourcePath.text.toString().trim()
            val baseX = x.text.toString().toIntOrNull()
            val baseY = y.text.toString().toIntOrNull()
            val baseZ = z.text.toString().toIntOrNull()
            val width = pixelWidth.text.toString().toIntOrNull()
            val sourceFile = File(source)
            val extension = sourceFile.extension.lowercase(Locale.ROOT)
            if (!sourceFile.isFile || !sourceFile.canRead() || extension !in supportedExtensions) {
                Toast.makeText(appContext, "请选择可读取的建筑投影文件", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            if (baseX == null || baseY == null || baseZ == null) {
                Toast.makeText(appContext, "请输入有效的投影锚点坐标", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            if (extension in pixelArtExtensions && (width == null || width !in 1..MAX_PIXEL_WIDTH)) {
                Toast.makeText(appContext, "像素画宽度必须在 1 到 $MAX_PIXEL_WIDTH 之间", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            val requestedPrinterConfig = ProjectionPrinterSettings.Config(
                enabled = enablePrinter.isChecked,
                blocksPerSecond = (printerRate.progress + ProjectionPrinterSettings.MIN_RATE)
                    .coerceIn(ProjectionPrinterSettings.MIN_RATE, ProjectionPrinterSettings.MAX_RATE)
            )
            ProjectionPrinterSettings.setEnabled(appContext, requestedPrinterConfig.enabled)
            ProjectionPrinterSettings.setRate(appContext, requestedPrinterConfig.blocksPerSecond)
            if (!operationInFlight.compareAndSet(false, true)) return@setOnClickListener
            val operation = operationGeneration.incrementAndGet()
            // Do not leave a prior projection running while a new source is
            // being parsed.  The new configuration is applied only after the
            // new projection becomes available.
            BuildProjectionPrinterOverlay.hide()
            BuildProjectionMaterialPreviewOverlay.onProjectionLoadStarted()
            runCatching { TpModule.setBuildProjectionPrinterEnabled(false) }

            val projectionDirectory = File(appContext.filesDir, "build_projection")
            prefs.edit()
                .putString(KEY_SOURCE_PATH, sourceFile.absolutePath)
                .putInt(KEY_BASE_X, baseX)
                .putInt(KEY_BASE_Y, baseY)
                .putInt(KEY_BASE_Z, baseZ)
                .putInt(KEY_ROTATION, selectedRotation)
                .putInt(KEY_PIXEL_WIDTH, width ?: DEFAULT_PIXEL_WIDTH)
                .apply()
            status.text = "正在解析并生成投影…"
            setOperationState(true)

            val loadThread = Thread({
                try {
                    var ok = false
                    var message = "投影加载失败"
                    try {
                        if (operationGeneration.get() != operation) {
                            message = "投影加载已取消"
                        } else if (!projectionDirectory.isDirectory &&
                            !projectionDirectory.mkdirs() && !projectionDirectory.isDirectory) {
                            message = "无法创建投影工作目录"
                        } else if (operationGeneration.get() != operation) {
                            message = "投影加载已取消"
                        } else {
                            var generatedJpegPng: File? = null
                            val sourceForNative = if (extension == "jpg" || extension == "jpeg") {
                                activity.runOnUiThread {
                                    if (operationGeneration.get() == operation && dialog.isShowing) {
                                        status.text = "正在转换 JPG 像素画…"
                                    }
                                }
                                val generatedPath = TpModule.preparePixelArtSource(
                                    sourceFile.absolutePath,
                                    projectionDirectory.absolutePath,
                                    width ?: DEFAULT_PIXEL_WIDTH
                                ) ?: throw IllegalStateException("JPG 解码失败：文件损坏或设备内存不足")
                                File(generatedPath).also { generatedJpegPng = it }
                            } else sourceFile
                            if (operationGeneration.get() != operation) {
                                generatedJpegPng?.delete()
                                ok = false
                                message = "投影加载已取消"
                            } else {
                                try {
                                    ok = TpModule.loadBuildProjection(
                                        sourceForNative.absolutePath,
                                        projectionDirectory.absolutePath,
                                        baseX,
                                        baseY,
                                        baseZ,
                                        selectedRotation,
                                        width ?: DEFAULT_PIXEL_WIDTH
                                    )
                                } finally {
                                    generatedJpegPng?.delete()
                                }
                                activity.runOnUiThread { stopStatusPolling(operation) }
                                var textureMessage = ""
                                if (ok && operationGeneration.get() == operation) {
                                    val requests = runCatching {
                                        TpModule.getBuildProjectionTextureRequests()
                                    }.getOrNull().orEmpty()
                                    if (requests.isNotBlank()) {
                                        activity.runOnUiThread {
                                            if (operationGeneration.get() == operation && dialog.isShowing) {
                                                status.text = "正在读取原版方块贴图…"
                                            }
                                        }
                                        val textureResult = ProjectionTextureLoader.load(appContext, requests)
                                        if (operationGeneration.get() == operation) {
                                            run {
                                                val installed = runCatching {
                                                    TpModule.installBuildProjectionTexturePack(
                                                        textureResult.materialKeys,
                                                        textureResult.faceLayers,
                                                        textureResult.tileSize,
                                                        textureResult.layerCount,
                                                        textureResult.layerPixels
                                                    )
                                                }.getOrDefault(false)
                                                textureMessage = if (installed) {
                                                    "；已加载原版方块贴图"
                                                } else {
                                                    "；原版贴图提交失败，使用备用材质"
                                                }
                                            }
                                        }
                                    }
                                }
                                var printerMessage = ""
                                if (ok && operationGeneration.get() == operation) {
                                    val printerApplied = runCatching {
                                        TpModule.setBuildProjectionPrinterRate(
                                            requestedPrinterConfig.blocksPerSecond
                                        )
                                        TpModule.setBuildProjectionPrinterEnabled(
                                            requestedPrinterConfig.enabled
                                        )
                                    }.isSuccess
                                    if (!printerApplied) {
                                        printerMessage = "；投影打印机暂不可用"
                                    }
                                }
                                message = readNativeStatus().ifBlank {
                                    if (ok) "投影已加载" else "投影加载失败"
                                } + textureMessage + printerMessage
                                if (operationGeneration.get() != operation) {
                                    ok = false
                                    message = "投影加载已取消"
                                }
                            }
                        }
                    } catch (throwable: Throwable) {
                        message = throwable.message?.takeIf { it.isNotBlank() } ?: "原生投影模块不可用"
                    }
                    activity.runOnUiThread {
                        stopStatusPolling(operation)
                        if (operationGeneration.get() != operation) return@runOnUiThread
                        operationInFlight.set(false)
                        if (!activity.isFinishing && !activity.isDestroyed && dialog.isShowing) {
                            setOperationState(false)
                            updatePngState(sourcePath.text.toString())
                            status.text = message
                            if (ok) {
                                showProjection.isChecked = true
                                BuildProjectionMaterialPreviewOverlay.onProjectionLoadSucceeded(
                                    activity,
                                    appContext
                                )
                                BuildProjectionPrinterOverlay.show(
                                    activity,
                                    appContext,
                                    requestedPrinterConfig
                                )
                                Toast.makeText(
                                    appContext,
                                    if (requestedPrinterConfig.enabled) {
                                        "建筑投影已加载，打印机已开启"
                                    } else {
                                        "建筑投影已加载，打印机控制已打开"
                                    },
                                    Toast.LENGTH_SHORT
                                ).show()
                            } else {
                                BuildProjectionMaterialPreviewOverlay.onProjectionLoadFailed()
                                BuildProjectionPrinterOverlay.hide()
                                Toast.makeText(appContext, message, Toast.LENGTH_LONG).show()
                            }
                        }
                    }
                } finally {
                    activeLoadThread.compareAndSet(Thread.currentThread(), null)
                }
            }, "build-projection-load")
            activeLoadThread.getAndSet(loadThread)?.interrupt()
            loadThread.start()
            startStatusPolling(operation)
        }

        clear.setOnClickListener {
            val cancellingLoad = operationInFlight.get()
            if (cancellingLoad) {
                if (!cancelRequested.compareAndSet(false, true)) return@setOnClickListener
            } else if (!operationInFlight.compareAndSet(false, true)) {
                return@setOnClickListener
            } else {
                cancelRequested.set(true)
            }
            val clearOperation = operationGeneration.incrementAndGet()
            stopStatusPolling()
            activeLoadThread.getAndSet(null)?.interrupt()
            BuildProjectionPrinterOverlay.hide()
            BuildProjectionMaterialPreviewOverlay.onProjectionCleared()
            status.text = if (cancellingLoad) "正在取消并清除投影…" else "正在清除投影…"
            setOperationState(true)
            Thread({
                val message = try {
                    TpModule.clearBuildProjection()
                    readNativeStatus().ifBlank { "当前没有加载投影" }
                } catch (throwable: Throwable) {
                    throwable.message?.takeIf { it.isNotBlank() } ?: "无法清除投影"
                }
                activity.runOnUiThread {
                    if (operationGeneration.get() != clearOperation) return@runOnUiThread
                    cancelRequested.set(false)
                    operationInFlight.set(false)
                    if (!activity.isFinishing && !activity.isDestroyed && dialog.isShowing) {
                        setOperationState(false)
                        updatePngState(sourcePath.text.toString())
                        status.text = message
                    }
                }
            }, "build-projection-clear").start()
        }

        if (!applySettings(enabled, outlineEnabled, alphaPercent, rangeChunks)) {
            status.text = "无法应用投影显示设置，请确认当前安装包完整"
        }
        runCatching { TpModule.setBuildProjectionReachabilityPreviewEnabled(reachabilityPreviewEnabled) }
        applyLayerFilter(false)
        showProjection.setOnCheckedChangeListener { _, checked ->
            prefs.edit().putBoolean(KEY_ENABLED, checked).apply()
            runCatching { TpModule.setBuildProjectionEnabled(checked) }
        }
        showReachabilityPreview.setOnCheckedChangeListener { _, checked ->
            prefs.edit().putBoolean(KEY_REACHABILITY_PREVIEW, checked).apply()
            runCatching { TpModule.setBuildProjectionReachabilityPreviewEnabled(checked) }
        }
        showMaterialPreview.setOnCheckedChangeListener { _, checked ->
            BuildProjectionMaterialPreviewOverlay.setEnabled(activity, appContext, checked)
        }
        enablePrinter.setOnCheckedChangeListener { _, checked ->
            ProjectionPrinterSettings.setEnabled(appContext, checked)
            runCatching { TpModule.setBuildProjectionPrinterEnabled(checked) }
        }
        printerRate.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                val value = (progress + ProjectionPrinterSettings.MIN_RATE)
                    .coerceIn(ProjectionPrinterSettings.MIN_RATE, ProjectionPrinterSettings.MAX_RATE)
                printerRateLabel.text = "打印速度上限 · $value 方块/秒"
                if (fromUser) {
                    ProjectionPrinterSettings.setRate(appContext, value)
                    runCatching { TpModule.setBuildProjectionPrinterRate(value) }
                }
            }

            override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit
            override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
        })
        var suppressOutlineCallback = false
        showOutline.setOnCheckedChangeListener { _, checked ->
            if (suppressOutlineCallback) return@setOnCheckedChangeListener
            val previous = prefs.getBoolean(KEY_OUTLINE_ENABLED, true)
            val applied = runCatching {
                TpModule.setBuildProjectionOutlineEnabled(checked)
            }.getOrDefault(false)
            if (applied) {
                prefs.edit().putBoolean(KEY_OUTLINE_ENABLED, checked).apply()
            } else {
                suppressOutlineCallback = true
                showOutline.isChecked = previous
                suppressOutlineCallback = false
                status.text = "描边开关应用失败，请确认当前安装包完整"
            }
        }
        alpha.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                val value = (progress + MIN_ALPHA_PERCENT).coerceIn(MIN_ALPHA_PERCENT, MAX_ALPHA_PERCENT)
                alphaLabel.text = "填充透明度 · $value%"
                if (fromUser) {
                    prefs.edit().putInt(KEY_ALPHA_PERCENT, value).apply()
                    runCatching { TpModule.setBuildProjectionAlpha(value / 100f) }
                }
            }

            override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit
            override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
        })
        range.setOnSeekBarChangeListener(object : SeekBar.OnSeekBarChangeListener {
            override fun onProgressChanged(seekBar: SeekBar?, progress: Int, fromUser: Boolean) {
                val value = (progress + MIN_RANGE_CHUNKS).coerceIn(MIN_RANGE_CHUNKS, MAX_RANGE_CHUNKS)
                rangeLabel.text = "渲染距离 · $value 区块"
                if (fromUser) {
                    prefs.edit().putInt(KEY_RANGE_CHUNKS, value).apply()
                    runCatching { TpModule.setBuildProjectionRange(value) }
                }
            }

            override fun onStartTrackingTouch(seekBar: SeekBar?) = Unit
            override fun onStopTrackingTouch(seekBar: SeekBar?) = Unit
        })

        dialog.setContentView(root)
        dialog.window?.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
        dialog.setOnDismissListener { releaseDialogAndCancelOperation() }
        dialog.show()
        activeDialog = dialog
        application.registerActivityLifecycleCallbacks(lifecycleCallbacks)
        lifecycleRegistered = true
        dialog.window?.setLayout(
            minOf(
                dp(activity, if (useWideLayout) 760f else 460f),
                (activity.resources.displayMetrics.widthPixels * 0.96f).toInt()
            ),
            minOf(
                dp(activity, if (useWideLayout) 540f else 780f),
                (activity.resources.displayMetrics.heightPixels * 0.94f).toInt()
            )
        )
    }

    private fun showNeteaseFileList(
        activity: Activity,
        context: Context,
        wideLayout: Boolean,
        onSelected: (File) -> Unit
    ) {
        val directory = context.getExternalFilesDir(null)?.let { File(it, "netease") }
        val files = directory?.listFiles()?.filter {
            it.isFile && it.extension.lowercase(Locale.ROOT) in supportedExtensions
        }?.sortedBy { it.name.lowercase(Locale.ROOT) }.orEmpty()
        if (files.isEmpty()) {
            Toast.makeText(context, "files/netease/ 中没有可投影文件", Toast.LENGTH_SHORT).show()
            return
        }

        val listDialog = Dialog(activity).apply { requestWindowFeature(Window.FEATURE_NO_TITLE) }
        val listRoot = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(activity, 20f), dp(activity, 18f), dp(activity, 20f), dp(activity, 20f))
            background = rounded(BG, dp(activity, 18f), LINE)
        }
        val title = TextView(activity).apply {
            text = "选择投影文件"
            textSize = 17f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(INK))
        }
        listRoot.addView(title)
        listRoot.addView(TextView(activity).apply {
            text = "来自 files/netease/"
            textSize = 11f
            setTextColor(Color.parseColor(MUTED))
            setPadding(0, dp(activity, 2f), 0, dp(activity, 10f))
        })
        val list = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        files.forEach { file ->
            val row = TextView(activity).apply {
                text = "${file.name}\n${formatSize(file.length())}"
                textSize = 14f
                setTextColor(Color.parseColor(INK))
                setPadding(dp(activity, 14f), dp(activity, 11f), dp(activity, 14f), dp(activity, 11f))
                background = rounded(SURFACE, dp(activity, 10f), LINE)
                setOnClickListener {
                    onSelected(file)
                    listDialog.dismiss()
                }
            }
            list.addView(row, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                bottomMargin = dp(activity, 7f)
            })
        }
        val scroll = ScrollView(activity).apply {
            isVerticalScrollBarEnabled = false
            addView(list)
        }
        listRoot.addView(scroll, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            projectionListViewportHeight(activity, if (wideLayout) 500f else 380f)
        ))
        listDialog.setContentView(listRoot)
        listDialog.window?.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
        listDialog.show()
        listDialog.window?.setLayout(
            minOf(
                dp(activity, if (wideLayout) 560f else 400f),
                (activity.resources.displayMetrics.widthPixels * 0.92f).toInt()
            ),
            WindowManager.LayoutParams.WRAP_CONTENT
        )
    }

    private fun projectionListViewportHeight(activity: Activity, preferredDp: Float): Int {
        val screenHeight = activity.resources.displayMetrics.heightPixels
        val maxDialogHeight = (screenHeight * 0.86f).toInt()
        val titleAndPaddingHeight = dp(activity, 104f)
        val availableHeight = (maxDialogHeight - titleAndPaddingHeight)
            .coerceAtLeast(dp(activity, 96f))
        return minOf(dp(activity, preferredDp), availableHeight)
    }

    private fun readNativeStatus(): String = runCatching { TpModule.getBuildProjectionStatus() }
        .getOrNull()?.trim().orEmpty().ifBlank { "当前没有加载投影" }

    private fun applySettings(
        enabled: Boolean,
        outlineEnabled: Boolean,
        alphaPercent: Int,
        rangeChunks: Int
    ): Boolean = runCatching {
        TpModule.setBuildProjectionEnabled(enabled)
        check(TpModule.setBuildProjectionOutlineEnabled(outlineEnabled))
        TpModule.setBuildProjectionAlpha(alphaPercent / 100f)
        TpModule.setBuildProjectionRange(rangeChunks)
    }.isSuccess

    private fun input(activity: Activity, text: String, numeric: Boolean) = EditText(activity).apply {
        setText(text)
        textSize = 13f
        setTextColor(Color.parseColor(INK))
        setHintTextColor(Color.parseColor(MUTED))
        isSingleLine = true
        inputType = if (numeric) {
            InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED
        } else {
            InputType.TYPE_CLASS_TEXT
        }
        setPadding(dp(activity, 12f), 0, dp(activity, 12f), 0)
        background = rounded(BG, dp(activity, 9f), LINE)
    }

    private fun coordinateInput(activity: Activity, hintText: String, value: Int) =
        input(activity, value.toString(), true).apply { hint = hintText }

    private fun sectionLabel(activity: Activity, text: String) = TextView(activity).apply {
        this.text = text
        textSize = 11f
        typeface = Typeface.DEFAULT_BOLD
        setTextColor(Color.parseColor(ACCENT_DARK))
        setPadding(dp(activity, 4f), dp(activity, 11f), 0, dp(activity, 6f))
    }

    private fun valueLabel(activity: Activity, text: String) = TextView(activity).apply {
        this.text = text
        textSize = 12f
        typeface = Typeface.DEFAULT_BOLD
        setTextColor(Color.parseColor(ACCENT_DARK))
        setPadding(dp(activity, 4f), dp(activity, 4f), 0, 0)
    }

    private fun actionButton(activity: Activity, text: String, accent: Boolean = false) = Button(activity).apply {
        this.text = text
        isAllCaps = false
        textSize = 12f
        setTextColor(Color.parseColor(if (accent) BG else INK))
        background = rounded(
            if (accent) ACCENT else BG,
            dp(activity, 9f),
            if (accent) ACCENT else LINE
        )
        minHeight = 0
        minimumHeight = 0
        setPadding(dp(activity, 8f), 0, dp(activity, 8f), 0)
    }

    private fun fieldParams(activity: Activity) =
        LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(activity, 48f))

    private fun buttonParams(activity: Activity) =
        LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(activity, 40f)).apply {
            topMargin = dp(activity, 7f)
        }

    private fun formatSize(bytes: Long): String = when {
        bytes >= 1024L * 1024L -> String.format(Locale.ROOT, "%.1f MB", bytes / (1024.0 * 1024.0))
        bytes >= 1024L -> String.format(Locale.ROOT, "%.1f KB", bytes / 1024.0)
        else -> "$bytes B"
    }

    private fun rounded(fill: String, radius: Int, stroke: String? = null) = GradientDrawable().apply {
        setColor(Color.parseColor(fill))
        cornerRadius = radius.toFloat()
        if (stroke != null) setStroke(1, Color.parseColor(stroke))
    }

    private fun dp(activity: Activity, value: Float): Int =
        (value * activity.resources.displayMetrics.density).toInt()

    private val ROTATIONS = listOf(0, 90, 180, 270)
}
