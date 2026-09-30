package com.vdl.kong520.ui

import android.app.Activity
import android.app.Dialog
import android.content.Context
import android.content.SharedPreferences
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.graphics.drawable.StateListDrawable
import android.os.Handler
import android.os.Looper
import android.text.InputType
import android.text.method.ScrollingMovementMethod
import android.view.Gravity
import android.widget.FrameLayout
import android.view.ViewGroup
import android.view.Window
import android.view.WindowManager
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import com.vdl.kong520.TpModule
import java.io.File
import java.lang.ref.WeakReference
import java.util.Locale
import java.util.UUID
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong
import java.util.concurrent.atomic.AtomicReference

/** Shared resumable import surface rendered as separate structure and pixel-art screens. */
object BuildImportUi {
    private const val UI_BG = "#FFFEFA"
    private const val UI_SURFACE = "#FAF7F1"
    private const val UI_ACCENT = "#CF6846"
    private const val UI_ACCENT_DARK = "#AA4E31"
    private const val UI_ACCENT_SOFT = "#FAE9DF"
    private const val UI_INK = "#302D29"
    private const val UI_MUTED = "#77716A"
    private const val UI_LINE = "#E7E0D7"
    private const val UI_GREEN = "#3F765C"

    private data class DetectedWorldContext(
        val worldId: String,
        val dimensionId: Int,
        val checkpointRestoreSafe: Boolean
    )

    private const val PREFS = "build_import_ui"
    private const val KEY_LAST_SPOOL = "last_spool"
    private const val KEY_LAST_DIMENSION = "last_dimension"
    private const val KEY_SHOW_PROGRESS_OVERLAY = "show_progress_overlay"
    private const val KEY_VERIFY_AFTER_IMPORT = "verify_after_import"
    private const val KEY_VERIFICATION_PRECISION = "verification_precision"
    private const val KEY_PLACE_DENY_LAYER = "place_deny_layer"
    private const val KEY_BLOCKS_PER_SECOND = "blocks_per_second"
    private const val KEY_PIXEL_ART_WIDTH = "pixel_art_width"
    private const val KEY_CREATE_MAPS_AFTER_IMPORT = "create_maps_after_import"
    private const val KEY_SIMULATION_CHUNK_RANGE = "simulation_chunk_range"
    private const val KEY_SUPPRESS_COMMAND_FEEDBACK = "suppress_command_feedback"
    private const val UNDO_MANIFEST_FILE = "last_import.undo"
    private const val VERIFICATION_PRECISION_FAST = 0
    private const val VERIFICATION_PRECISION_THOROUGH = 2
    private const val MAX_BLOCKS_PER_SECOND = 200_000
    private const val LIMITED_MAX_BLOCKS_PER_SECOND = 500
    private const val MIN_SIMULATION_CHUNK_RANGE = 4
    private const val MAX_SIMULATION_CHUNK_RANGE = 8
    private const val DEFAULT_SIMULATION_CHUNK_RANGE = 4
    private const val TERMINAL_UNDO_REFRESH_ATTEMPTS = 5
    private const val TERMINAL_UNDO_REFRESH_DELAY_MS = 200L
    // The old default of 20 blocks/second meant a one-million-block build took
    // over half a day, and the value was not remembered between imports. The
    // native scheduler already backs off automatically when the server queue
    // slows down, so a much higher starting point is safe and is the single
    // biggest factor in perceived import speed.
    private const val DEFAULT_BLOCKS_PER_SECOND = 2_000
    private val terminalMonitorGeneration = AtomicLong(0L)
    private val operationGeneration = AtomicLong(0L)
    private val activeJobPath = AtomicReference<String?>(null)
    private val nativeActionLock = Any()
    private val metadataLock = Any()
    private val uiHandler = Handler(Looper.getMainLooper())

    /** The cleanup worker may outlive the panel, so it only keeps this weak binding. */
    private class TerminalUiBinding(
        dialog: Dialog,
        status: TextView,
        start: Button,
        pause: Button,
        resume: Button,
        restore: Button,
        undo: Button,
        cancel: Button,
        private val prefs: SharedPreferences
    ) {
        private val dialogRef = WeakReference(dialog)
        private val statusRef = WeakReference(status)
        private val startRef = WeakReference(start)
        private val pauseRef = WeakReference(pause)
        private val resumeRef = WeakReference(resume)
        private val restoreRef = WeakReference(restore)
        private val undoRef = WeakReference(undo)
        private val cancelRef = WeakReference(cancel)
        @Volatile private var detached = false

        fun detach() {
            detached = true
            dialogRef.clear()
            statusRef.clear()
            startRef.clear()
            pauseRef.clear()
            resumeRef.clear()
            restoreRef.clear()
            undoRef.clear()
            cancelRef.clear()
        }

        fun isAttached(): Boolean {
            if (detached) return false
            val dialog = dialogRef.get() ?: return false
            val activity = dialog.context as? Activity
            return dialog.isShowing && activity?.isFinishing != true && activity?.isDestroyed != true
        }

        fun postTerminal(terminalState: Int, expectedOperationGeneration: Long? = null) {
            if (detached) return
            uiHandler.post {
                if (detached) return@post
                if (expectedOperationGeneration != null &&
                    BuildImportUi.operationGeneration.get() != expectedOperationGeneration) {
                    return@post
                }
                val dialog = dialogRef.get() ?: run { detach(); return@post }
                val activity = dialog.context as? Activity
                if (!dialog.isShowing || activity?.isFinishing == true || activity?.isDestroyed == true) {
                    detach()
                    return@post
                }

                val nativeState = runCatching { TpModule.getBuildImportState() }
                    .getOrDefault(terminalState)
                startRef.get()?.isEnabled = nativeState == 0 || nativeState == 5 || nativeState == 6
                pauseRef.get()?.isEnabled = nativeState == 2 || nativeState == 7
                resumeRef.get()?.isEnabled = nativeState == 3 || nativeState == 4
                restoreRef.get()?.isEnabled =
                    (nativeState == 0 || nativeState == 5 || nativeState == 6) &&
                        hasRestorableArtifacts(readLastSpool(prefs))
                undoRef.get()?.isEnabled =
                    (nativeState == 0 || nativeState == 5 || nativeState == 6) &&
                        hasUndoManifest(dialog.context)
                cancelRef.get()?.isEnabled = nativeState in 1..4 || nativeState == 7

                val status = statusRef.get() ?: return@post
                when (terminalState) {
                    5 -> status.text = "导入完成"
                    6 -> status.text = runCatching { TpModule.getBuildImportStatus() }
                        .getOrNull()?.takeIf { it.isNotBlank() }
                        ?: if (hasRestorableArtifacts(readLastSpool(prefs))) {
                            "导入失败，重新打开面板可从断点继续"
                        } else {
                            "导入失败"
                        }
                    0 -> status.text = "导入任务已结束"
                    -1 -> status.text = "无法读取导入状态；请关闭后重新打开面板"
                }
                if (terminalState == 5 || terminalState == 6) {
                    refreshUndoAvailabilityAfterTerminal(
                        expectedOperationGeneration,
                        attempt = 0
                    )
                }
            }
        }

        /**
         * Native publishes Completed before the final undo manifest write is
         * necessarily visible. Recheck briefly so the button reflects the
         * completed operation instead of requiring the panel to be reopened.
         */
        private fun refreshUndoAvailabilityAfterTerminal(
            expectedOperationGeneration: Long?,
            attempt: Int
        ) {
            if (detached || attempt >= TERMINAL_UNDO_REFRESH_ATTEMPTS) return
            uiHandler.postDelayed({
                if (detached) return@postDelayed
                if (expectedOperationGeneration != null &&
                    BuildImportUi.operationGeneration.get() != expectedOperationGeneration) {
                    return@postDelayed
                }
                val dialog = dialogRef.get() ?: run { detach(); return@postDelayed }
                val activity = dialog.context as? Activity
                if (!dialog.isShowing || activity?.isFinishing == true || activity?.isDestroyed == true) {
                    detach()
                    return@postDelayed
                }
                val nativeState = runCatching { TpModule.getBuildImportState() }.getOrNull()
                if (nativeState !in setOf(0, 5, 6)) return@postDelayed
                val undoAvailable = hasUndoManifest(dialog.context)
                undoRef.get()?.isEnabled = undoAvailable
                // A normal import creates the manifest after Completed becomes
                // visible, while a completed undo removes it in that same
                // terminal window. Keep sampling for the whole short window so
                // both transitions settle without reopening the panel.
                if (nativeState == 5) {
                    refreshUndoAvailabilityAfterTerminal(
                        expectedOperationGeneration,
                        attempt + 1
                    )
                }
            }, TERMINAL_UNDO_REFRESH_DELAY_MS)
        }
    }

    /**
     * Opens the import panel.  [limited] is presentation-only; native startup
     * admission repeats the ticket, format, non-air-block, and rate checks so
     * direct Java calls cannot bypass the restricted-import policy.
     */
    fun show(activity: Activity, context: Context, limited: Boolean = false) {
        show(activity, context, limited, BuildImportSourceMode.STRUCTURE)
    }

    internal fun showPixelArt(activity: Activity, context: Context) {
        show(activity, context, limited = false, BuildImportSourceMode.PIXEL_ART)
    }

    private fun showResumedProgressOverlay(
        activity: Activity,
        prefs: SharedPreferences,
        spool: String,
        sessionId: Long
    ) {
        if (!prefs.getBoolean(KEY_SHOW_PROGRESS_OVERLAY, true) ||
            activity.isFinishing || activity.isDestroyed) return
        val jobName = File(spool).name
        val eyebrow = if (jobName.startsWith("pixel_", ignoreCase = true)) {
            "IMAGE IMPORT"
        } else {
            "BUILD IMPORT"
        }
        BuildImportProgressOverlay.show(activity, jobName, eyebrow, sessionId)
        BuildImportProgressOverlay.bindSession(sessionId)
    }

    private fun show(
        activity: Activity,
        context: Context,
        limited: Boolean,
        sourceMode: BuildImportSourceMode
    ) {
        if (activity.isFinishing || activity.isDestroyed) return
        val pixelArtMode = sourceMode == BuildImportSourceMode.PIXEL_ART
        val overlayEyebrow = if (pixelArtMode) "IMAGE IMPORT" else "BUILD IMPORT"
        val appContext = context.applicationContext
        val prefs = appContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val maxBlocksPerSecond = if (limited) LIMITED_MAX_BLOCKS_PER_SECOND else MAX_BLOCKS_PER_SECOND
        val nativeState = runCatching { TpModule.getBuildImportState() }.getOrNull()
        discardMissingLastSpool(appContext, nativeState)
        val density = context.resources.displayMetrics.density
        fun dp(value: Float) = (value * density).toInt()
        val screenWidthPx = activity.resources.displayMetrics.widthPixels
        val screenHeightPx = activity.resources.displayMetrics.heightPixels
        // A split pane only remains legible when the game is in a genuinely wide layout.
        val useTwoPane = screenWidthPx / density >= 600f && screenWidthPx > screenHeightPx
        val compactRightPane = useTwoPane && screenHeightPx / density < 460f

        val dialog = Dialog(activity).apply {
            requestWindowFeature(Window.FEATURE_NO_TITLE)
            setCanceledOnTouchOutside(true)
        }
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(
                dp(if (compactRightPane) 12f else 18f),
                dp(if (compactRightPane) 10f else 16f),
                dp(if (compactRightPane) 12f else 18f),
                dp(if (compactRightPane) 10f else 14f)
            )
            background = rounded(UI_BG, dp(18f), UI_LINE)
        }
        dialog.setContentView(root)
        addHeader(
            root,
            { dp(it) },
            overlayEyebrow,
            if (pixelArtMode) "图片导入" else "建筑导入",
            if (pixelArtMode) "将 PNG / JPG 映射为方块色板，再放置到世界中。" else "选择建筑或 MIDI 文件，设置导入位置与校验策略。",
            dialog::dismiss,
            accountText = "本地可用"
        )

        val contentRow = LinearLayout(activity).apply {
            orientation = if (useTwoPane) LinearLayout.HORIZONTAL else LinearLayout.VERTICAL
            gravity = Gravity.TOP
        }
        val scroll = ScrollView(activity).apply { isVerticalScrollBarEnabled = false; isFillViewport = false }
        val form = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        scroll.addView(form)
        val rightPane = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(if (compactRightPane) 10f else 14f), dp(if (compactRightPane) 8f else 12f), dp(if (compactRightPane) 10f else 14f), dp(if (compactRightPane) 8f else 12f))
            background = rounded(UI_SURFACE, dp(12f), UI_LINE)
        }
        val rightContent = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        if (useTwoPane) {
            rightPane.addView(ScrollView(activity).apply {
                isVerticalScrollBarEnabled = false
                addView(rightContent)
            }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        }
        if (useTwoPane) {
            contentRow.addView(scroll, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1.08f).apply {
                marginEnd = dp(10f)
            })
            contentRow.addView(rightPane, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 0.92f))
        } else {
            // Phone portrait: the action strip joins the same scrollable column
            // so the import form never loses height to a fixed-height side pane.
            contentRow.addView(scroll, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.MATCH_PARENT
            ))
        }
        root.addView(contentRow, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f).apply { topMargin = dp(if (compactRightPane) 8f else 12f) })

        fun section(text: String, container: LinearLayout = form) = TextView(activity).apply {
            this.text = text
            textSize = 10f
            letterSpacing = 0.08f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(UI_ACCENT_DARK))
            setPadding(0, dp(10f), 0, dp(6f))
            container.addView(this)
        }
        fun input(text: String = "", numeric: Boolean = false) = EditText(activity).apply {
            setText(text)
            textSize = 12f
            setTextColor(Color.parseColor(UI_INK))
            isSingleLine = true
            inputType = if (numeric) InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED else InputType.TYPE_CLASS_TEXT
            setPadding(dp(10f), dp(4f), dp(10f), 0)
            background = rounded("#FFFFFF", dp(8f), UI_LINE)
        }
        fun labeledInput(label: String, text: String = "", numeric: Boolean = false): FrameLayout {
            val container = FrameLayout(activity)
            val edit = input(text, numeric)
            container.addView(edit, FrameLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(42f)).apply { topMargin = dp(7f) })
            container.addView(TextView(activity).apply {
                this.text = label
                textSize = 10f
                typeface = Typeface.DEFAULT_BOLD
                setTextColor(Color.parseColor(UI_MUTED))
                setPadding(dp(6f), 0, dp(6f), 0)
                setBackgroundColor(Color.parseColor(UI_BG))
            }, FrameLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(16f)).apply {
                leftMargin = dp(11f)
                gravity = Gravity.TOP or Gravity.START
            })
            return container
        }
        fun action(text: String, accent: Boolean = false) = Button(activity).apply {
            this.text = text
            isAllCaps = false
            textSize = if (compactRightPane) 10f else 11f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(ColorStateList(
                arrayOf(intArrayOf(-android.R.attr.state_enabled), intArrayOf()),
                intArrayOf(Color.parseColor(UI_MUTED), Color.parseColor(if (accent) "#FFFFFF" else UI_INK))
            ))
            background = StateListDrawable().apply {
                addState(intArrayOf(-android.R.attr.state_enabled), rounded(UI_SURFACE, dp(8f), UI_LINE))
                addState(intArrayOf(), rounded(if (accent) UI_ACCENT else UI_BG, dp(8f), if (accent) null else UI_LINE))
            }
            minHeight = 0
            minimumHeight = 0
            setPadding(dp(5f), 0, dp(5f), 0)
        }

        // Confirmations use the same warm surface as the parent injected panel.
        fun styledConfirm(
            title: String,
            message: String,
            positiveLabel: String,
            onPositive: () -> Unit,
            negativeLabel: String? = null,
            onNegative: (() -> Unit)? = null,
            neutralLabel: String? = "暂不"
        ) {
            val confirm = Dialog(activity).apply {
                requestWindowFeature(Window.FEATURE_NO_TITLE)
                setCanceledOnTouchOutside(true)
            }
            val body = LinearLayout(activity).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(22f), dp(20f), dp(22f), dp(16f))
                background = rounded(UI_BG, dp(16f), UI_LINE)
            }
            body.addView(TextView(activity).apply {
                text = title
                textSize = 17f
                typeface = Typeface.DEFAULT_BOLD
                setTextColor(Color.parseColor(UI_INK))
            })
            body.addView(TextView(activity).apply {
                text = message
                textSize = 12.5f
                setTextColor(Color.parseColor(UI_MUTED))
                setPadding(0, dp(10f), 0, dp(18f))
            })
            val row = LinearLayout(activity).apply { gravity = Gravity.END }
            fun addButton(label: String, accent: Boolean, onClick: () -> Unit) {
                row.addView(action(label, accent).apply {
                    setOnClickListener { confirm.dismiss(); onClick() }
                }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, dp(40f)).apply {
                    marginStart = dp(8f)
                })
            }
            neutralLabel?.let { addButton(it, false) {} }
            if (negativeLabel != null && onNegative != null) addButton(negativeLabel, false, onNegative)
            addButton(positiveLabel, true, onPositive)
            body.addView(row, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            confirm.setContentView(body)
            confirm.show()
            confirm.window?.let { w ->
                w.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
                w.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
                w.attributes = w.attributes.apply { dimAmount = 0.48f }
                w.setGravity(Gravity.CENTER)
                w.setLayout(
                    minOf(dp(340f), (screenWidthPx * 0.9f).toInt()),
                    ViewGroup.LayoutParams.WRAP_CONTENT
                )
            }
        }

        section(if (pixelArtMode) "01 · 图片与位置" else "01 · 来源与位置")
        val sourceContainer = labeledInput(
            when {
                pixelArtMode -> "选择 PNG/JPG 图片"
                limited -> "选择 Schematic 建筑文件（仅 .schem / .schematic）"
                else -> "选择建筑或 MIDI 文件（.mid / .midi）"
            }
        )
        val sourcePath = sourceContainer.getChildAt(0) as EditText
        sourcePath.isFocusable = false
        val chooseFile = action(if (pixelArtMode) "浏览图片" else "浏览文件")
        val fileRow = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL; gravity = Gravity.BOTTOM }
        fileRow.addView(sourceContainer, LinearLayout.LayoutParams(0, dp(49f), 1f))
        fileRow.addView(chooseFile, LinearLayout.LayoutParams(dp(92f), dp(35f)).apply { marginStart = dp(7f) })
        form.addView(fileRow, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(49f)))
        if (!pixelArtMode && !limited) {
            form.addView(TextView(activity).apply {
                text = "支持建筑文件和 MIDI（.mid / .midi）；MIDI 会解析多轨/多通道、音色、力度和延音，并生成多声部命令方块音乐。"
                textSize = 12f
                setTextColor(Color.parseColor(UI_MUTED))
                setPadding(dp(2f), dp(8f), dp(2f), 0)
            }, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ))
        }
        if (limited) {
            form.addView(TextView(activity).apply {
                text = "受限导入：仅支持 .schem / .schematic；解析后非空气方块不得超过 50,000，速度上限 500 方块/秒。"
                textSize = 12f
                setTextColor(Color.parseColor("#A87631"))
                setPadding(dp(2f), dp(8f), dp(2f), 0)
            }, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ))
        }

        section(if (pixelArtMode) "02 · 尺寸与放置" else "02 · 导入参数")
        val pixelWidthContainer = labeledInput(
            "像素画宽度 / 方块",
            prefs.getInt(KEY_PIXEL_ART_WIDTH, 64).coerceAtLeast(1).toString(),
            numeric = true
        )
        val pixelWidth = pixelWidthContainer.getChildAt(0) as EditText
        val blocksPerSecondContainer = labeledInput(
            "导入速度 / 秒",
            prefs.getInt(KEY_BLOCKS_PER_SECOND, DEFAULT_BLOCKS_PER_SECOND)
                .coerceIn(1, maxBlocksPerSecond).toString(),
            numeric = true
        )
        val blocksPerSecond = blocksPerSecondContainer.getChildAt(0) as EditText
        val simulationChunkRangeContainer = labeledInput(
            "模拟区块范围",
            prefs.getInt(KEY_SIMULATION_CHUNK_RANGE, DEFAULT_SIMULATION_CHUNK_RANGE)
                .coerceIn(MIN_SIMULATION_CHUNK_RANGE, MAX_SIMULATION_CHUNK_RANGE).toString(),
            numeric = true
        )
        val simulationChunkRange = simulationChunkRangeContainer.getChildAt(0) as EditText
        val parameterRow = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        if (pixelArtMode) {
            parameterRow.addView(pixelWidthContainer, LinearLayout.LayoutParams(0, dp(49f), 1f).apply { marginEnd = dp(7f) })
            parameterRow.addView(blocksPerSecondContainer, LinearLayout.LayoutParams(0, dp(49f), 1f))
            form.addView(parameterRow, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(49f)).apply { topMargin = dp(7f) })
            form.addView(simulationChunkRangeContainer, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(49f)).apply { topMargin = dp(7f) })
        } else {
            parameterRow.addView(blocksPerSecondContainer, LinearLayout.LayoutParams(0, dp(49f), 1f).apply { marginEnd = dp(7f) })
            parameterRow.addView(simulationChunkRangeContainer, LinearLayout.LayoutParams(0, dp(49f), 1f))
            form.addView(parameterRow, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(49f)).apply { topMargin = dp(7f) })
        }
        form.addView(TextView(activity).apply {
            text = "速度 1 - $maxBlocksPerSecond 方块/秒，服务器卡顿时自动降速；模拟范围 $MIN_SIMULATION_CHUNK_RANGE - $MAX_SIMULATION_CHUNK_RANGE 区块。"
            textSize = 10f
            setTextColor(Color.parseColor(UI_MUTED))
            setPadding(dp(2f), dp(4f), dp(2f), 0)
        })
        val coordinates = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        val xContainer = labeledInput("X 坐标", "0", true); val yContainer = labeledInput("Y 坐标", "64", true); val zContainer = labeledInput("Z 坐标", "0", true)
        val x = xContainer.getChildAt(0) as EditText; val y = yContainer.getChildAt(0) as EditText; val z = zContainer.getChildAt(0) as EditText
        listOf(xContainer, yContainer, zContainer).forEach { coordinates.addView(it, LinearLayout.LayoutParams(0, dp(49f), 1f).apply { marginEnd = dp(6f) }) }
        form.addView(coordinates, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(49f)).apply { topMargin = dp(8f) })
        val capturePositionInFlight = AtomicBoolean(false)
        val capturePosition = action("使用玩家当前位置")
        form.addView(capturePosition, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(34f)).apply {
            topMargin = dp(6f)
        })
        val optionsContainer = if (useTwoPane) rightContent else form
        section("03 · 导入选项", optionsContainer)
        val clearExisting = CheckBox(activity).apply {
            text = "覆盖导入范围内已有方块"
            textSize = 11f
            setTextColor(Color.parseColor(UI_INK))
            buttonTintList = ColorStateList.valueOf(Color.parseColor(UI_ACCENT))
            isChecked = false
            minHeight = dp(29f)
        }
        if (!limited) optionsContainer.addView(clearExisting)
        val placeDenyLayer = CheckBox(activity).apply {
            text = if (pixelArtMode) "在像素画底部放置拒绝方块" else "在建筑底部放置拒绝方块"
            textSize = 11f
            setTextColor(Color.parseColor(UI_INK))
            buttonTintList = ColorStateList.valueOf(Color.parseColor(UI_ACCENT))
            isChecked = !limited && prefs.getBoolean(KEY_PLACE_DENY_LAYER, false)
            minHeight = dp(29f)
        }
        placeDenyLayer.setOnCheckedChangeListener { _, checked ->
            if (!limited) prefs.edit().putBoolean(KEY_PLACE_DENY_LAYER, checked).apply()
        }
        if (!limited) optionsContainer.addView(placeDenyLayer)
        val createMapsAfterImport = CheckBox(activity).apply {
            text = "导入完成后自动制作地图"
            textSize = 11f
            setTextColor(Color.parseColor(UI_INK))
            buttonTintList = ColorStateList.valueOf(Color.parseColor(UI_ACCENT))
            isChecked = pixelArtMode && prefs.getBoolean(KEY_CREATE_MAPS_AFTER_IMPORT, false)
            minHeight = dp(29f)
        }
        if (pixelArtMode) {
            optionsContainer.addView(createMapsAfterImport)
            optionsContainer.addView(TextView(activity).apply {
                text = "会将像素画起点对齐到 128×128 地图网格；每格需要 1 张空白地图。方块导入后自动从热栏或背包取图并逐张制作。请确保像素画上方无遮挡；地图颜色由游戏地表配色决定，可能与原图有差异。"
                textSize = 10f
                setTextColor(Color.parseColor(UI_MUTED))
                setPadding(dp(4f), 0, dp(4f), dp(5f))
            })
        }
        val verifyAfterImport = CheckBox(activity).apply {
            text = "导入完成后校验区块并自动修复"
            textSize = 11f
            setTextColor(Color.parseColor(UI_INK))
            buttonTintList = ColorStateList.valueOf(Color.parseColor(UI_ACCENT))
            isChecked = !limited && prefs.getBoolean(KEY_VERIFY_AFTER_IMPORT, true)
            minHeight = dp(29f)
        }
        if (!limited) optionsContainer.addView(verifyAfterImport)
        val verificationPrecisionLabel = TextView(activity).apply {
            text = "校验精度 · 每区块最多抽样"
            textSize = 10f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor(UI_ACCENT_DARK))
            setPadding(dp(4f), dp(4f), 0, dp(5f))
        }
        var selectedVerificationPrecision = prefs.getInt(
            KEY_VERIFICATION_PRECISION,
            VERIFICATION_PRECISION_THOROUGH
        ).coerceIn(VERIFICATION_PRECISION_FAST, VERIFICATION_PRECISION_THOROUGH)
        val verificationPrecisionRow = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
        }
        val verificationPrecisionButtons = listOf(
            action("快速 10"),
            action("均衡 15"),
            action("精细 20")
        )
        fun refreshVerificationPrecisionControls() {
            verificationPrecisionButtons.forEachIndexed { index, button ->
                val selected = index == selectedVerificationPrecision
                button.setTextColor(Color.parseColor(if (selected) UI_ACCENT_DARK else UI_MUTED))
                button.background = rounded(
                    if (selected) UI_ACCENT_SOFT else UI_BG,
                    dp(7f),
                    if (selected) "#E9C8BA" else UI_LINE
                )
                button.isEnabled = verifyAfterImport.isChecked
                button.alpha = if (verifyAfterImport.isChecked) 1f else 0.45f
            }
        }
        verificationPrecisionButtons.forEachIndexed { index, button ->
            button.setOnClickListener {
                selectedVerificationPrecision = index
                prefs.edit().putInt(KEY_VERIFICATION_PRECISION, index).apply()
                refreshVerificationPrecisionControls()
            }
            verificationPrecisionRow.addView(
                button,
                LinearLayout.LayoutParams(0, dp(30f), 1f).apply {
                    if (index != verificationPrecisionButtons.lastIndex) marginEnd = dp(6f)
                }
            )
        }
        if (!limited) {
            optionsContainer.addView(verificationPrecisionLabel)
            optionsContainer.addView(
                verificationPrecisionRow,
                LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(30f))
            )
        } else {
            optionsContainer.addView(TextView(activity).apply {
                text = "受限模式已强制关闭：覆盖已有方块、拒绝方块层、导入后校验与自动修复。"
                textSize = 10f
                setTextColor(Color.parseColor("#A87631"))
                setPadding(dp(2f), dp(10f), dp(2f), dp(2f))
            })
        }
        verifyAfterImport.setOnCheckedChangeListener { _, checked ->
            if (!limited) prefs.edit().putBoolean(KEY_VERIFY_AFTER_IMPORT, checked).apply()
            refreshVerificationPrecisionControls()
        }
        refreshVerificationPrecisionControls()
        val showProgressOverlay = CheckBox(activity).apply {
            text = "显示悬浮导入进度（可点击穿透）"
            textSize = 11f
            setTextColor(Color.parseColor(UI_INK))
            buttonTintList = ColorStateList.valueOf(Color.parseColor(UI_ACCENT))
            isChecked = prefs.getBoolean(KEY_SHOW_PROGRESS_OVERLAY, true)
            minHeight = dp(29f)
        }
        optionsContainer.addView(showProgressOverlay)

        val footer = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(0, dp(8f), 0, 0)
            background = rounded(UI_BG, dp(8f))
        }
        root.addView(footer, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(7f)
        })
        val status = TextView(activity).apply {
            text = "准备就绪"
            textSize = if (compactRightPane) 10f else 11f
            gravity = Gravity.START or Gravity.CENTER_VERTICAL
            setTextColor(Color.parseColor(UI_INK))
            setPadding(dp(10f), dp(6f), dp(10f), dp(6f))
            minHeight = dp(30f)
            maxLines = if (useTwoPane) Int.MAX_VALUE else 3
            if (!useTwoPane) movementMethod = ScrollingMovementMethod.getInstance()
            isVerticalScrollBarEnabled = !useTwoPane
            setHorizontallyScrolling(false)
            background = rounded(if (useTwoPane) UI_BG else UI_SURFACE, dp(8f), UI_LINE)
        }
        if (useTwoPane) {
            section("当前任务", rightContent)
            rightContent.addView(status, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ))
        } else {
            footer.addView(status, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { bottomMargin = dp(7f) })
        }

        val fixedActions = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        val start = action("开始导入", true); val pause = action("暂停")
        val resume = action("继续"); val restore = action("恢复任务")
        val undo = action("撤销导入"); val cancel = action("取消")
        if (useTwoPane) {
            val row = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
            val visibleActions = if (limited) listOf(start, pause, resume, restore, cancel)
                else listOf(start, pause, resume, restore, undo, cancel)
            visibleActions.forEachIndexed { index, button ->
                row.addView(button, LinearLayout.LayoutParams(0, dp(35f), if (button === start) 1.35f else 1f).apply {
                    if (index != visibleActions.lastIndex) marginEnd = dp(6f)
                })
            }
            fixedActions.addView(row)
        } else {
            val topActions = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
            topActions.addView(start, LinearLayout.LayoutParams(0, dp(36f), 1.35f).apply { marginEnd = dp(6f) })
            topActions.addView(pause, LinearLayout.LayoutParams(0, dp(36f), 0.65f))
            fixedActions.addView(topActions)
            val bottomActions = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
            val visibleActions = if (limited) listOf(resume, restore, cancel) else listOf(resume, restore, undo, cancel)
            visibleActions.forEachIndexed { index, button ->
                bottomActions.addView(button, LinearLayout.LayoutParams(0, dp(34f), 1f).apply {
                    if (index != visibleActions.lastIndex) marginEnd = dp(6f)
                })
            }
            fixedActions.addView(bottomActions, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(34f)).apply { topMargin = dp(6f) })
        }
        footer.addView(fixedActions)

        fun updateActionState(operationInFlight: Boolean = false) {
            if (operationInFlight) {
                start.isEnabled = false
                capturePosition.isEnabled = false
                pause.isEnabled = false
                resume.isEnabled = false
                restore.isEnabled = false
                undo.isEnabled = false
                cancel.isEnabled = false
                return
            }
            val nativeState = runCatching { TpModule.getBuildImportState() }.getOrDefault(0)
            val capturingPosition = capturePositionInFlight.get()
            start.isEnabled = (nativeState == 0 || nativeState == 5 || nativeState == 6) &&
                !capturingPosition
            capturePosition.isEnabled = !capturingPosition
            pause.isEnabled = nativeState == 2 || nativeState == 7
            resume.isEnabled = nativeState == 3 || nativeState == 4
            restore.isEnabled = (nativeState == 0 || nativeState == 5 || nativeState == 6) &&
                hasRestorableArtifacts(readLastSpool(prefs))
            undo.isEnabled = !limited &&
                (nativeState == 0 || nativeState == 5 || nativeState == 6) &&
                hasUndoManifest(appContext)
            cancel.isEnabled = nativeState in 1..4 || nativeState == 7
        }

        val handler = Handler(Looper.getMainLooper())
        val terminalUiBinding = TerminalUiBinding(
            dialog, status, start, pause, resume, restore, undo, cancel, prefs
        )
        dialog.setOnDismissListener { terminalUiBinding.detach() }
        val dialogHeight = minOf(dp(if (useTwoPane) 620f else 700f), (screenHeightPx * if (compactRightPane) 0.94f else 0.88f).toInt())
        val dialogWidth = minOf(dp(if (useTwoPane) 720f else 480f), (screenWidthPx * 0.92f).toInt())
        showDialog(dialog, dialogWidth, dialogHeight)
        updateActionState()
        runCatching { TpModule.getBuildImportState() }.getOrNull()?.let { nativeState ->
            if (nativeState in 1..4 || nativeState == 7) {
                status.text = runCatching { TpModule.getBuildImportStatus() }
                    .getOrNull()?.takeIf { it.isNotBlank() } ?: "导入任务正在运行"
                (activeJobPath.get() ?: readLastSpool(prefs))?.let { spool ->
                    showResumedProgressOverlay(activity, prefs, spool, operationGeneration.get())
                }
            }
        }
        // Surface a restorable import on panel open. The visible restore action
        // shares this same implementation.
        run {
            val idleState = runCatching { TpModule.getBuildImportState() }.getOrDefault(0)
            val lastSpool = readLastSpool(prefs)
            if ((idleState == 0 || idleState == 5 || idleState == 6) &&
                hasRestorableArtifacts(lastSpool)) {
                val jobName = lastSpool?.let { File(it).name } ?: "上次导入"
                val stillShowing = { dialog.isShowing && !activity.isFinishing && !activity.isDestroyed }
                styledConfirm(
                    title = "发现未完成的导入任务",
                    message = "检测到上次导入任务（$jobName）尚未完成，可从断点继续（继续会重新加载并回放当前分区）。",
                    positiveLabel = "继续导入",
                    onPositive = { if (stillShowing()) restore.performClick() },
                    negativeLabel = "放弃任务",
                    onNegative = {
                        if (stillShowing()) styledConfirm(
                            title = "放弃该导入任务？",
                            message = "将删除该导入任务的断点与已生成的中间文件。",
                            positiveLabel = "放弃",
                            onPositive = {
                                val claimReleased = lastSpool?.let { spoolPath ->
                                    if (!File(spoolPath).name.startsWith("undo_")) true
                                    else runCatching {
                                        TpModule.discardBuildImportUndoClaim(
                                            undoStorageDirectory(appContext).absolutePath,
                                            spoolPath
                                        )
                                    }.getOrDefault(false)
                                } ?: true
                                if (claimReleased) {
                                    cleanupOwnedJob(appContext, lastSpool)
                                    lastSpool?.let { clearLastSpoolIfMatches(prefs, it) }
                                } else {
                                    Toast.makeText(
                                        appContext,
                                        "撤销记录仍在使用，暂时不能放弃该任务",
                                        Toast.LENGTH_SHORT
                                    ).show()
                                }
                                updateActionState()
                            },
                            neutralLabel = "返回"
                        )
                    }
                )
            }
        }

        showProgressOverlay.setOnCheckedChangeListener { _, checked ->
            prefs.edit().putBoolean(KEY_SHOW_PROGRESS_OVERLAY, checked).apply()
            if (checked) {
                BuildImportProgressOverlay.show(
                    activity,
                    eyebrow = overlayEyebrow,
                    sessionId = operationGeneration.get()
                )
            } else {
                BuildImportProgressOverlay.hide()
            }
        }

        chooseFile.setOnClickListener {
            showNeteaseFileList(activity, context, sourceMode, limited) {
                sourcePath.setText(it.absolutePath)
            }
        }
        capturePosition.setOnClickListener {
            if (!capturePositionInFlight.compareAndSet(false, true)) return@setOnClickListener
            capturePosition.text = "正在读取当前位置…"
            updateActionState()
            Thread({
                val position = runCatching { TpModule.getBuildImportPlayerBlockPosition() }
                    .getOrNull()
                    ?.takeIf { it.size >= 3 }
                capturePositionInFlight.set(false)
                handler.post {
                    if (!terminalUiBinding.isAttached()) return@post
                    capturePosition.text = "使用玩家当前位置"
                    updateActionState()
                    if (position == null) {
                        Toast.makeText(appContext, "无法读取玩家位置，请先进入已加载的世界", Toast.LENGTH_SHORT).show()
                    } else {
                        x.setText(position[0].toString())
                        y.setText(position[1].toString())
                        z.setText(position[2].toString())
                        Toast.makeText(appContext, "已填入玩家当前位置", Toast.LENGTH_SHORT).show()
                    }
                }
            }, "build-import-capture-position").start()
        }
        start.setOnClickListener {
            val source = sourcePath.text.toString().trim()
            val baseX = x.text.toString().toIntOrNull(); val baseY = y.text.toString().toIntOrNull(); val baseZ = z.text.toString().toIntOrNull()
            val imageWidth = pixelWidth.text.toString().toIntOrNull(); val speed = blocksPerSecond.text.toString().toIntOrNull()
            val simulationRange = simulationChunkRange.text.toString().toIntOrNull()
            if (source.isEmpty() || baseX == null || baseY == null || baseZ == null || speed == null || simulationRange == null) {
                Toast.makeText(context, "请选择文件并填写有效坐标、速度和模拟区块范围", Toast.LENGTH_SHORT).show(); return@setOnClickListener
            }
            val sourceFile = File(source)
            if (!sourceFile.isFile || !sourceFile.canRead()) { Toast.makeText(context, "导入文件不可读取", Toast.LENGTH_SHORT).show(); return@setOnClickListener }
            if (!BuildImportSourcePolicy.accepts(sourceFile.name, sourceMode, limited)) {
                val message = when {
                    pixelArtMode -> "图片导入仅支持 PNG、JPG 或 JPEG 图片"
                    limited -> "受限导入仅支持 .schem 或 .schematic 文件"
                    else -> "建筑导入不支持该文件格式；MIDI 仅支持 .mid / .midi，PNG/JPG 请使用图片导入"
                }
                Toast.makeText(context, message, Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            val isPixelArt = pixelArtMode
            val isJpeg = pixelArtMode && BuildImportSourcePolicy.isJpeg(sourceFile.name)
            if (isPixelArt && (imageWidth == null || imageWidth <= 0)) {
                Toast.makeText(context, "像素画宽度必须大于 0", Toast.LENGTH_SHORT).show(); return@setOnClickListener
            }
            val pixelArtWidth = imageWidth ?: 64
            if (speed !in 1..maxBlocksPerSecond) {
                Toast.makeText(
                    context,
                    "导入速度必须在 1 到 $maxBlocksPerSecond 方块/秒之间",
                    Toast.LENGTH_SHORT
                ).show()
                return@setOnClickListener
            }
            if (simulationRange !in MIN_SIMULATION_CHUNK_RANGE..MAX_SIMULATION_CHUNK_RANGE) {
                Toast.makeText(
                    context,
                    "模拟区块范围必须在 $MIN_SIMULATION_CHUNK_RANGE 到 $MAX_SIMULATION_CHUNK_RANGE 之间",
                    Toast.LENGTH_SHORT
                ).show()
                return@setOnClickListener
            }
            // Remember the chosen rate: retyping it for every import was the
            // reason most jobs ran at the conservative default.
            val preferencesEditor = prefs.edit()
                .putInt(KEY_BLOCKS_PER_SECOND, speed)
                .putInt(KEY_SIMULATION_CHUNK_RANGE, simulationRange)
            val shouldCreateMaps = isPixelArt && createMapsAfterImport.isChecked
            if (isPixelArt) preferencesEditor
                .putInt(KEY_PIXEL_ART_WIDTH, pixelArtWidth)
                .putBoolean(KEY_CREATE_MAPS_AFTER_IMPORT, shouldCreateMaps)
            preferencesEditor.apply()
            val shouldClear = clearExisting.isChecked
            val shouldPlaceDenyLayer = placeDenyLayer.isChecked
            val shouldVerify = verifyAfterImport.isChecked
            val verificationPrecision = selectedVerificationPrecision
            val launchImport = launchImport@{
                status.text = "正在验证世界并开始解析…"
                updateActionState(operationInFlight = true)
                val jobPrefix = if (isPixelArt) "pixel" else "build"
                val jobId = "${jobPrefix}_${System.currentTimeMillis()}_${UUID.randomUUID().toString().take(8)}"
                val spoolDirectory = File(appContext.filesDir, "build_import/jobs/$jobId")
                if (!spoolDirectory.mkdirs() && !spoolDirectory.isDirectory) {
                    updateActionState()
                    Toast.makeText(context, "无法创建导入工作目录", Toast.LENGTH_SHORT).show()
                    return@launchImport
                }
                val spoolPath = spoolDirectory.absolutePath
                val launchGeneration = operationGeneration.incrementAndGet()
                activeJobPath.set(spoolPath)
                val previousSpool = readLastSpool(prefs)
                Thread {
                    var launchError: String? = null
                    var launched = false
                    var accepted = false
                    try {
                        val nativeSource = if (isJpeg) {
                            prepareJpegSource(sourceFile, spoolDirectory, pixelArtWidth).also {
                                if (it == null) launchError = "JPG 解码失败：文件损坏或设备内存不足"
                            }
                        } else sourceFile
                        val worldContext = if (nativeSource != null &&
                            operationGeneration.get() == launchGeneration) {
                            readNativeWorldContext()
                        } else null
                        if (nativeSource != null && worldContext != null) {
                            synchronized(nativeActionLock) {
                                if (operationGeneration.get() == launchGeneration) {
                                    launched = if (isPixelArt) {
                                        TpModule.startPixelArtImport(
                                            nativeSource.absolutePath, spoolDirectory.absolutePath,
                                            jobId, baseX, baseY, baseZ, pixelArtWidth, speed,
                                            shouldClear, shouldPlaceDenyLayer, shouldVerify, verificationPrecision,
                                            simulationRange, worldContext.worldId, worldContext.dimensionId,
                                            shouldCreateMaps,
                                            prefs.getBoolean(KEY_SUPPRESS_COMMAND_FEEDBACK, true)
                                        )
                                    } else {
                                        TpModule.startBuildImport(
                                            nativeSource.absolutePath, spoolDirectory.absolutePath, jobId,
                                            baseX, baseY, baseZ, speed, shouldClear,
                                            shouldPlaceDenyLayer, shouldVerify, verificationPrecision,
                                            simulationRange, worldContext.worldId, worldContext.dimensionId,
                                            prefs.getBoolean(KEY_SUPPRESS_COMMAND_FEEDBACK, true)
                                        )
                                    }
                                    accepted = launched &&
                                        operationGeneration.get() == launchGeneration
                                    if (launched && !accepted) TpModule.cancelBuildImport()
                                }
                            }
                        }
                        if (nativeSource != null && worldContext == null &&
                            operationGeneration.get() == launchGeneration) {
                            launchError = "启动失败：当前世界上下文尚未就绪，请等待游戏地图加载完成后重试"
                        } else if (worldContext != null && !launched &&
                            operationGeneration.get() == launchGeneration) {
                            val nativeStatus = runCatching { TpModule.getBuildImportStatus() }
                                .getOrNull()?.trim().orEmpty()
                            launchError = nativeStatus.takeIf { it.isNotEmpty() }
                                ?.let { "启动失败：$it" }
                                ?: "启动失败：原生导入器拒绝启动，请查看游戏日志"
                        }
                        if (accepted && operationGeneration.get() == launchGeneration) {
                            if (!writeLastSpool(prefs, spoolPath, worldContext!!.dimensionId)) {
                                launchError = "启动失败：无法保存断点恢复信息"
                                synchronized(nativeActionLock) {
                                    if (operationGeneration.get() == launchGeneration) {
                                        TpModule.cancelBuildImport()
                                    }
                                }
                                accepted = false
                            } else if (operationGeneration.get() == launchGeneration) {
                                if (!previousSpool.isNullOrBlank() && previousSpool != spoolPath) {
                                    cleanupOwnedJob(appContext, previousSpool)
                                }
                                monitorTerminalCleanup(
                                    appContext, spoolDirectory, launchGeneration, terminalUiBinding
                                )
                            }
                        }
                        if (!accepted && operationGeneration.get() == launchGeneration) {
                            cleanupOwnedJob(appContext, spoolPath)
                        }
                    } catch (throwable: Throwable) {
                        launchError = throwable.message?.takeIf { it.isNotBlank() }
                            ?.let { "启动失败：$it" } ?: "启动失败：原生接口异常"
                        if (operationGeneration.get() == launchGeneration) {
                            var cancellationAttempted = !launched
                            if (launched) {
                                synchronized(nativeActionLock) {
                                    if (operationGeneration.get() == launchGeneration) {
                                        runCatching { TpModule.cancelBuildImport() }
                                        cancellationAttempted = true
                                    }
                                }
                            }
                            if (cancellationAttempted && operationGeneration.get() == launchGeneration) {
                                accepted = false
                                cleanupOwnedJob(appContext, spoolPath)
                            }
                        }
                    } finally {
                        if (!accepted || operationGeneration.get() != launchGeneration) {
                            clearActiveJobPath(spoolPath)
                        }
                        handler.post {
                            if (operationGeneration.get() != launchGeneration) return@post
                            if (accepted && prefs.getBoolean(KEY_SHOW_PROGRESS_OVERLAY, true) &&
                                !activity.isFinishing && !activity.isDestroyed) {
                                // A task can be accepted after the panel is dismissed.
                                // The progress window belongs to the Activity, not the panel.
                                BuildImportProgressOverlay.hide()
                                BuildImportProgressOverlay.show(
                                    activity, sourceFile.name, overlayEyebrow,
                                    launchGeneration
                                )
                            }
                            if (!terminalUiBinding.isAttached()) return@post
                            updateActionState()
                            if (!accepted) status.text = launchError ?: "启动失败：无法读取当前世界或维度，请确认已进入世界、拥有 OP 且 Hook 已加载"
                            else {
                                status.text = "已开始导入；进度将在悬浮窗口显示"
                            }
                        }
                    }
                }.start()
            }
            if (speed > 20) {
                styledConfirm(
                    title = "高频导入风险",
                    message = "高频导入后可能导致游戏账号禁止登录一小时。\n\n当前速度：$speed 方块/秒，是否仍要继续？",
                    positiveLabel = "我已了解，继续",
                    onPositive = launchImport,
                    negativeLabel = "返回修改",
                    onNegative = {},
                    neutralLabel = null
                )
            } else launchImport()
        }
        pause.setOnClickListener {
            val pauseGeneration = operationGeneration.incrementAndGet()
            terminalMonitorGeneration.incrementAndGet()
            updateActionState(operationInFlight = true)
            status.text = "正在暂停导入…"
            Thread {
                var paused = false
                try {
                    synchronized(nativeActionLock) {
                        if (operationGeneration.get() == pauseGeneration) {
                            TpModule.pauseBuildImport()
                            paused = TpModule.getBuildImportState() == 3
                        }
                    }
                } catch (_: Throwable) {
                    paused = false
                }
                handler.post {
                    if (operationGeneration.get() != pauseGeneration) return@post
                    if (!terminalUiBinding.isAttached()) return@post
                    updateActionState()
                    status.text = if (paused) "导入已暂停；继续时会重新加载并回放当前分区" else "暂停导入失败"
                }
            }.start()
        }
        undo.setOnClickListener {
            if (limited || !hasUndoManifest(appContext)) {
                Toast.makeText(context, "没有可撤销的成功导入", Toast.LENGTH_SHORT).show()
                updateActionState()
                return@setOnClickListener
            }
            val currentState = runCatching { TpModule.getBuildImportState() }.getOrDefault(-1)
            if (currentState !in setOf(0, 5, 6)) {
                Toast.makeText(context, "请先结束当前导入或导出任务", Toast.LENGTH_SHORT).show()
                updateActionState()
                return@setOnClickListener
            }
            styledConfirm(
                title = "撤销上次导入？",
                message = "将按区块把上一次导入的建筑包围范围填充为空气。范围内并非由本次导入放置的方块也会被清除，且无法还原；大型建筑会自动拆分执行。",
                positiveLabel = "开始撤销",
                onPositive = startUndo@{
                    val storageDirectory = undoStorageDirectory(appContext)
                    val jobId = "undo_${System.currentTimeMillis()}_${UUID.randomUUID().toString().take(8)}"
                    val spoolDirectory = File(storageDirectory, "jobs/$jobId")
                    if (!spoolDirectory.mkdirs() && !spoolDirectory.isDirectory) {
                        Toast.makeText(context, "无法创建撤销工作目录", Toast.LENGTH_SHORT).show()
                        return@startUndo
                    }
                    val spoolPath = spoolDirectory.absolutePath
                    val undoGeneration = operationGeneration.incrementAndGet()
                    val previousSpool = readLastSpool(prefs)
                    activeJobPath.set(spoolPath)
                    status.text = "正在验证世界并准备撤销…"
                    updateActionState(operationInFlight = true)
                    Thread {
                        var launched = false
                        var accepted = false
                        var launchError = "撤销启动失败，请确认已进入原世界并拥有 OP 权限"
                        try {
                            val worldContext = if (operationGeneration.get() == undoGeneration) {
                                readNativeWorldContext()
                            } else null
                            if (worldContext != null) {
                                synchronized(nativeActionLock) {
                                    if (operationGeneration.get() == undoGeneration) {
                                        launched = TpModule.undoLastBuildImport(
                                            storageDirectory.absolutePath,
                                            spoolDirectory.absolutePath,
                                            worldContext.worldId,
                                            worldContext.dimensionId
                                        )
                                        accepted = launched &&
                                            operationGeneration.get() == undoGeneration
                                        if (launched && !accepted) TpModule.cancelBuildImport()
                                    }
                                }
                            }
                            if (accepted && operationGeneration.get() == undoGeneration) {
                                if (!writeLastSpool(prefs, spoolPath, worldContext!!.dimensionId)) {
                                    launchError = "撤销启动失败：无法保存断点信息"
                                    synchronized(nativeActionLock) {
                                        if (operationGeneration.get() == undoGeneration) {
                                            TpModule.cancelBuildImport()
                                        }
                                    }
                                    accepted = false
                                } else {
                                    if (!previousSpool.isNullOrBlank() && previousSpool != spoolPath) {
                                        cleanupOwnedJob(appContext, previousSpool)
                                    }
                                    monitorTerminalCleanup(
                                        appContext, spoolDirectory, undoGeneration, terminalUiBinding
                                    )
                                }
                            }
                            if (!accepted && operationGeneration.get() == undoGeneration) {
                                cleanupOwnedJob(appContext, spoolPath)
                            }
                        } catch (throwable: Throwable) {
                            launchError = throwable.message?.takeIf { it.isNotBlank() }
                                ?.let { "撤销启动失败：$it" } ?: launchError
                            if (launched && operationGeneration.get() == undoGeneration) {
                                synchronized(nativeActionLock) {
                                    if (operationGeneration.get() == undoGeneration) {
                                        runCatching { TpModule.cancelBuildImport() }
                                    }
                                }
                            }
                            if (operationGeneration.get() == undoGeneration) {
                                accepted = false
                                cleanupOwnedJob(appContext, spoolPath)
                            }
                        } finally {
                            if (!accepted || operationGeneration.get() != undoGeneration) {
                                clearActiveJobPath(spoolPath)
                            }
                            handler.post {
                                if (operationGeneration.get() != undoGeneration ||
                                    !terminalUiBinding.isAttached()) return@post
                                updateActionState()
                                if (accepted) {
                                    status.text = "已开始撤销；将按区块清空上次导入区域"
                                    if (showProgressOverlay.isChecked) {
                                        BuildImportProgressOverlay.hide()
                                        BuildImportProgressOverlay.show(
                                            activity, "上次导入", "撤销导入", undoGeneration
                                        )
                                    }
                                    Toast.makeText(context, "已开始撤销导入", Toast.LENGTH_SHORT).show()
                                } else {
                                    status.text = launchError
                                    Toast.makeText(context, launchError, Toast.LENGTH_SHORT).show()
                                }
                            }
                        }
                    }.start()
                },
                neutralLabel = "暂不"
            )
        }
        cancel.setOnClickListener {
            val cancelGeneration = operationGeneration.incrementAndGet()
            val spool = readLastSpool(prefs)
            val launchingSpool = activeJobPath.getAndSet(null)
            terminalMonitorGeneration.incrementAndGet()
            BuildImportProgressOverlay.hide()
            status.text = "正在取消导入…"
            updateActionState(operationInFlight = true)
            Thread {
                var cancelled = false
                try {
                    synchronized(nativeActionLock) {
                        if (operationGeneration.get() == cancelGeneration) {
                            TpModule.cancelBuildImport()
                            cancelled = true
                        }
                    }
                } catch (_: Throwable) {
                    cancelled = false
                } finally {
                    if (cancelled && operationGeneration.get() == cancelGeneration) {
                        cleanupOwnedJob(appContext, spool)
                        if (launchingSpool != spool) cleanupOwnedJob(appContext, launchingSpool)
                    }
                }
                handler.post {
                    if (operationGeneration.get() != cancelGeneration) return@post
                    if (!terminalUiBinding.isAttached()) return@post
                    updateActionState()
                    status.text = if (cancelled) "已取消导入" else "取消导入失败"
                }
            }.start()
        }
        resume.setOnClickListener {
            resumeImport(activity, appContext, handler, status, terminalUiBinding,
                updateActions = { updateActionState(it) })
        }
        restore.setOnClickListener {
            val spool = readLastSpool(prefs)
            if (spool.isNullOrBlank() || !hasRestorableArtifacts(spool)) {
                spool?.let { clearLastSpoolIfMatches(prefs, it) }
                Toast.makeText(context, "没有可恢复的导入记录", Toast.LENGTH_SHORT).show()
                updateActionState()
                return@setOnClickListener
            }
            val restoreGeneration = operationGeneration.incrementAndGet()
            activeJobPath.set(spool)
            status.text = "正在恢复 checkpoint…"
            updateActionState(operationInFlight = true)
            Thread {
                var resumed = false
                var volatileIdentityRejected = false
                try {
                    val worldContext = if (operationGeneration.get() == restoreGeneration) {
                        readNativeWorldContext()
                    } else null
                    volatileIdentityRejected = worldContext != null &&
                        !worldContext.checkpointRestoreSafe
                    synchronized(nativeActionLock) {
                        if (operationGeneration.get() == restoreGeneration &&
                            worldContext?.checkpointRestoreSafe == true) {
                            val restored = TpModule.restoreBuildImport(spool, worldContext.worldId, worldContext.dimensionId)
                            resumed = restored && operationGeneration.get() == restoreGeneration &&
                                TpModule.resumeBuildImport(worldContext.worldId, worldContext.dimensionId)
                        }
                    }
                } catch (_: Throwable) {
                    resumed = false
                }
                if (resumed && operationGeneration.get() == restoreGeneration) {
                    monitorTerminalCleanup(
                        appContext, File(spool), restoreGeneration, terminalUiBinding
                    )
                }
                else clearActiveJobPath(spool)
                handler.post {
                    if (operationGeneration.get() != restoreGeneration) return@post
                    if (resumed) {
                        showResumedProgressOverlay(activity, prefs, spool, restoreGeneration)
                    }
                    if (!dialog.isShowing || activity.isFinishing || activity.isDestroyed) return@post
                    updateActionState()
                    status.text = when {
                        resumed -> "已恢复导入"
                        volatileIdentityRejected -> "恢复已拒绝：当前世界没有可证明跨会话稳定的标识，请重新开始导入"
                        else -> "恢复失败：无法读取当前维度，或世界、维度与 spool 不匹配"
                    }
                }
            }.start()
        }
    }

    private fun resumeImport(
        activity: Activity,
        context: Context,
        handler: Handler,
        status: TextView,
        terminalUiBinding: TerminalUiBinding,
        updateActions: (Boolean) -> Unit
    ) {
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val spool = readLastSpool(prefs)
        if (spool.isNullOrBlank()) {
            status.text = "没有可继续的导入记录"
            updateActions(false)
            return
        }
        val resumeGeneration = operationGeneration.incrementAndGet()
        activeJobPath.set(spool)
        terminalMonitorGeneration.incrementAndGet()
        status.text = "正在继续导入…"
        updateActions(true)
        Thread {
            var ok = false
            try {
                val worldContext = if (operationGeneration.get() == resumeGeneration) {
                    readNativeWorldContext()
                } else null
                synchronized(nativeActionLock) {
                    if (operationGeneration.get() == resumeGeneration &&
                        worldContext != null) {
                        ok = TpModule.resumeBuildImport(worldContext.worldId, worldContext.dimensionId)
                    }
                }
            } catch (_: Throwable) {
                ok = false
            }
            if (ok) {
                monitorTerminalCleanup(
                    context.applicationContext, File(spool), resumeGeneration, terminalUiBinding
                )
            } else {
                clearActiveJobPath(spool)
            }
            handler.post {
                if (operationGeneration.get() != resumeGeneration) return@post
                if (ok) {
                    showResumedProgressOverlay(activity, prefs, spool, resumeGeneration)
                }
                if (!terminalUiBinding.isAttached()) return@post
                updateActions(false)
                status.text = if (ok) "已继续导入" else "无法继续：无法读取当前维度，或世界与维度已变化"
            }
        }.start()
    }

    private fun showNeteaseFileList(
        activity: Activity,
        context: Context,
        sourceMode: BuildImportSourceMode,
        limited: Boolean,
        onSelected: (File) -> Unit
    ) {
        val directory = context.getExternalFilesDir(null)?.let { File(it, "netease") }
        val files = directory?.listFiles()?.filter {
            it.isFile && BuildImportSourcePolicy.accepts(it.name, sourceMode, limited)
        }?.sortedBy { it.name.lowercase(Locale.ROOT) }.orEmpty()
        if (files.isEmpty()) {
            Toast.makeText(
                context,
                if (sourceMode == BuildImportSourceMode.PIXEL_ART) {
                    "files/netease/ 中没有 PNG/JPG 图片"
                } else {
                    "files/netease/ 中没有可导入的建筑或 MIDI 文件"
                },
                Toast.LENGTH_SHORT
            ).show()
            return
        }
        val listDialog = Dialog(activity).apply { requestWindowFeature(Window.FEATURE_NO_TITLE) }
        val root = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL; setPadding(dp(activity, 16f), dp(activity, 14f), dp(activity, 16f), dp(activity, 16f)); background = rounded(UI_BG, dp(activity, 18f), UI_LINE) }
        addHeader(
            root,
            { dp(activity, it) },
            if (sourceMode == BuildImportSourceMode.PIXEL_ART) "IMAGE IMPORT" else "BUILD IMPORT",
            if (sourceMode == BuildImportSourceMode.PIXEL_ART) "选择图片" else "选择建筑或 MIDI 文件",
            when {
                sourceMode == BuildImportSourceMode.PIXEL_ART -> "来自 files/netease/ · PNG/JPG/JPEG"
                limited -> "受限模式：仅 .schem / .schematic"
                else -> "来自 files/netease/ · 建筑 / MIDI（.mid / .midi）"
            },
            listDialog::dismiss
        )
        val scroll = ScrollView(activity); val list = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }; scroll.addView(list)
        files.forEach { file ->
            val row = TextView(activity).apply {
                text = "${file.name}\n${formatSize(file.length())}"
                textSize = 12f; setTextColor(Color.parseColor(UI_INK)); setPadding(dp(activity, 12f), dp(activity, 10f), dp(activity, 12f), dp(activity, 10f))
                background = rounded(UI_SURFACE, dp(activity, 9f), UI_LINE)
                setOnClickListener { onSelected(file); listDialog.dismiss() }
            }
            list.addView(row, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply { bottomMargin = dp(activity, 8f) })
        }
        root.addView(scroll, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            buildImportListViewportHeight(activity, 380f)
        ).apply { topMargin = dp(activity, 12f) })
        listDialog.setContentView(root); showDialog(listDialog, dp(activity, 342f))
    }

    private fun buildImportListViewportHeight(activity: Activity, preferredDp: Float): Int {
        val screenHeight = activity.resources.displayMetrics.heightPixels
        val maxDialogHeight = (screenHeight * 0.86f).toInt()
        val headerAndPaddingHeight = dp(activity, 132f)
        val availableHeight = (maxDialogHeight - headerAndPaddingHeight)
            .coerceAtLeast(dp(activity, 96f))
        return minOf(dp(activity, preferredDp), availableHeight)
    }

    private fun prepareJpegSource(source: File, spoolDirectory: File, targetWidth: Int): File? {
        if (targetWidth <= 0) return null
        return try {
            TpModule.preparePixelArtSource(
                source.absolutePath,
                spoolDirectory.absolutePath,
                targetWidth
            )?.let(::File)?.takeIf { it.isFile && it.length() > 0L }
        } catch (_: OutOfMemoryError) {
            null
        } catch (_: RuntimeException) {
            null
        }
    }

    private fun readNativeWorldContext(): DetectedWorldContext? {
        // Level/player IDs may be ready one or two ticks before the dimension
        // component. This method runs on the background launch worker, so a
        // short retry avoids rejecting an otherwise ready game session.
        for (attempt in 0 until 3) {
            val worldId = runCatching { TpModule.getWorldId() }.getOrNull()?.trim().orEmpty()
            val separator = worldId.lastIndexOf('|')
            val dimensionId = if (separator > 0 && separator < worldId.lastIndex) {
                worldId.substring(separator + 1).trim().toIntOrNull()
            } else null
            if (worldId.isNotEmpty() && worldId != "unknown" && dimensionId != null) {
                val identity = worldId.substring(0, separator)
                // Legacy or volatile identities remain usable for a live start/resume,
                // but never authorize loading a checkpoint from a previous session.
                val checkpointRestoreSafe = identity.matches(Regex("^stable:v1:[0-9a-f]{64}$"))
                return DetectedWorldContext(worldId, dimensionId, checkpointRestoreSafe)
            }
            if (attempt != 2) {
                try {
                    Thread.sleep(150L)
                } catch (_: InterruptedException) {
                    Thread.currentThread().interrupt()
                    return null
                }
            }
        }
        return null
    }

    private fun monitorTerminalCleanup(
        context: Context,
        spoolDirectory: File,
        expectedOperationGeneration: Long,
        uiBinding: TerminalUiBinding? = null
    ) {
        if (operationGeneration.get() != expectedOperationGeneration) return
        val appContext = context.applicationContext
        val spoolPath = spoolDirectory.absolutePath
        val generation = terminalMonitorGeneration.incrementAndGet()
        val uiBindingRef = uiBinding?.let(::WeakReference)
        Thread({
            var observedActiveState = false
            var consecutiveQueryFailures = 0
            while (terminalMonitorGeneration.get() == generation &&
                operationGeneration.get() == expectedOperationGeneration) {
                val nativeState = runCatching { TpModule.getBuildImportState() }.getOrNull()
                if (nativeState == null) {
                    if (++consecutiveQueryFailures >= 5) {
                        clearActiveJobPath(spoolPath)
                        uiBindingRef?.get()?.postTerminal(
                            -1,
                            expectedOperationGeneration
                        )
                        return@Thread
                    }
                } else {
                    consecutiveQueryFailures = 0
                    if (nativeState in 1..4 || nativeState == 7) observedActiveState = true
                    when (nativeState) {
                        5 -> {
                            if (operationGeneration.get() != expectedOperationGeneration) return@Thread
                            clearActiveJobPath(spoolPath)
                            // Native owns the job directory until its terminal
                            // cleanup has returned. The Completed state becomes
                            // observable slightly before that cleanup finishes,
                            // so only clear Java metadata here.
                            clearLastSpoolIfMatches(
                                appContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE),
                                spoolPath
                            )
                            BuildImportProgressOverlay.notifyTerminal(
                                expectedOperationGeneration, nativeState
                            )
                            uiBindingRef?.get()?.postTerminal(
                                nativeState,
                                expectedOperationGeneration
                            )
                            return@Thread
                        }
                        6 -> {
                            // Execution/verification failures retain a complete
                            // plan. Planning failures have already removed it and
                            // must not expose a restore action that cannot work.
                            clearActiveJobPath(spoolPath)
                            if (!hasRestorableArtifacts(spoolPath)) {
                                clearLastSpoolIfMatches(
                                    appContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE),
                                    spoolPath
                                )
                            }
                            BuildImportProgressOverlay.notifyTerminal(
                                expectedOperationGeneration, nativeState
                            )
                            uiBindingRef?.get()?.postTerminal(
                                nativeState,
                                expectedOperationGeneration
                            )
                            return@Thread
                        }
                        0 -> if (observedActiveState) {
                            clearActiveJobPath(spoolPath)
                            // Planning can be cancelled internally for a world
                            // change or permission loss while the parser worker
                            // is still unwinding. It cleans its own files; a
                            // recursive Java delete here would race its writes.
                            clearLastSpoolIfMatches(
                                appContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE),
                                spoolPath
                            )
                            uiBindingRef?.get()?.postTerminal(
                                nativeState,
                                expectedOperationGeneration
                            )
                            return@Thread
                        }
                    }
                }
                try {
                    Thread.sleep(500L)
                } catch (_: InterruptedException) {
                    Thread.currentThread().interrupt()
                    return@Thread
                }
            }
        }, "BuildImportCleanup").apply {
            isDaemon = true
            start()
        }
    }

    private fun clearActiveJobPath(expected: String?): Boolean {
        while (true) {
            val current = activeJobPath.get()
            if (current != expected) return false
            if (activeJobPath.compareAndSet(current, null)) return true
        }
    }

    private fun readLastSpool(prefs: SharedPreferences): String? = synchronized(metadataLock) {
        prefs.getString(KEY_LAST_SPOOL, null)
    }

    private fun writeLastSpool(
        prefs: SharedPreferences,
        spoolPath: String,
        dimensionId: Int
    ): Boolean = synchronized(metadataLock) {
        prefs.edit()
            .putString(KEY_LAST_SPOOL, spoolPath)
            .putInt(KEY_LAST_DIMENSION, dimensionId)
            .commit()
    }

    private fun clearLastSpoolIfMatches(prefs: SharedPreferences, spoolPath: String) {
        synchronized(metadataLock) {
            if (prefs.getString(KEY_LAST_SPOOL, null) == spoolPath) {
                prefs.edit().remove(KEY_LAST_SPOOL).commit()
            }
        }
    }

    private fun hasRestorableArtifacts(spoolPath: String?): Boolean {
        if (spoolPath.isNullOrBlank()) return false
        val directory = File(spoolPath)
        val checkpoint = File(directory, "checkpoint.bin")
        val plan = File(directory, "verification.plan")
        return directory.isDirectory && checkpoint.isFile && plan.isFile
    }

    private fun undoStorageDirectory(context: Context): File =
        File(context.applicationContext.filesDir, "build_import")

    private fun hasUndoManifest(context: Context): Boolean =
        File(undoStorageDirectory(context), UNDO_MANIFEST_FILE).isFile

    private fun discardMissingLastSpool(context: Context, nativeState: Int?) {
        // Planning writes the checkpoint before verification.plan. Keep the Java
        // task identity while any native operation is active (or state lookup is
        // unavailable), otherwise reopening the panel can make pause/resume lose
        // the only path to the in-flight job.
        if (nativeState !in setOf(0, 5, 6)) return
        val prefs = context.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        synchronized(metadataLock) {
            val spool = prefs.getString(KEY_LAST_SPOOL, null) ?: return
            if (!hasRestorableArtifacts(spool)) prefs.edit().remove(KEY_LAST_SPOOL).commit()
        }
    }

    private fun cleanupOwnedJob(context: Context, spoolPath: String?) {
        if (spoolPath.isNullOrBlank()) return
        val directory = File(spoolPath)
        val owned = try {
            directory.canonicalFile.parentFile ==
                File(context.filesDir, "build_import/jobs").canonicalFile
        } catch (_: Exception) {
            false
        }
        if (owned) runCatching { directory.deleteRecursively() }

        clearLastSpoolIfMatches(context.getSharedPreferences(PREFS, Context.MODE_PRIVATE), spoolPath)
    }

    private fun rounded(fill: String, radius: Int, stroke: String? = null) = GradientDrawable().apply { setColor(Color.parseColor(fill)); cornerRadius = radius.toFloat(); if (stroke != null) setStroke(1, Color.parseColor(stroke)) }
    private fun addHeader(
        root: LinearLayout,
        dp: (Float) -> Int,
        eyebrow: String,
        title: String,
        subtitle: String,
        close: () -> Unit,
        accountText: String? = null
    ) {
        val row = LinearLayout(root.context).apply { gravity = Gravity.TOP }
        val textColumn = LinearLayout(root.context).apply { orientation = LinearLayout.VERTICAL }
        textColumn.addView(TextView(root.context).apply { text = eyebrow; textSize = 9f; letterSpacing = 0.14f; typeface = Typeface.DEFAULT_BOLD; setTextColor(Color.parseColor(UI_ACCENT_DARK)) })
        textColumn.addView(TextView(root.context).apply { text = title; textSize = 20f; typeface = Typeface.create(Typeface.SERIF, Typeface.NORMAL); setTextColor(Color.parseColor(UI_INK)); setPadding(0, dp(3f), 0, 0) })
        textColumn.addView(TextView(root.context).apply { text = subtitle; textSize = 10f; setTextColor(Color.parseColor(UI_MUTED)); setPadding(0, dp(3f), 0, 0) })
        row.addView(textColumn, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        accountText?.takeIf { it.isNotBlank() }?.let { value ->
            row.addView(TextView(root.context).apply {
                text = value
                textSize = 9f
                maxLines = 2
                gravity = Gravity.CENTER
                setTextColor(Color.parseColor(UI_GREEN))
                setPadding(dp(5f), dp(5f), dp(5f), dp(5f))
                background = rounded("#E5F2E8", dp(8f), "#D5E8D8")
            }, LinearLayout.LayoutParams(dp(94f), ViewGroup.LayoutParams.WRAP_CONTENT).apply {
                marginEnd = dp(6f)
            })
        }
        row.addView(TextView(root.context).apply { text = "×"; textSize = 20f; gravity = Gravity.CENTER; setTextColor(Color.parseColor(UI_MUTED)); background = rounded(UI_SURFACE, dp(8f), UI_LINE); setOnClickListener { close() } }, LinearLayout.LayoutParams(dp(30f), dp(30f)))
        root.addView(row)
        root.addView(TextView(root.context).apply { background = rounded(UI_LINE, dp(1f)) }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1f)).apply { topMargin = dp(10f) })
    }
    private fun showDialog(dialog: Dialog, width: Int, height: Int = ViewGroup.LayoutParams.WRAP_CONTENT) { dialog.show(); dialog.window?.let { window -> window.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT)); window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND); window.attributes = window.attributes.apply { dimAmount = 0.48f }; window.setGravity(Gravity.CENTER); window.setLayout(width, height) } }
    private fun dp(activity: Activity, value: Float) = (value * activity.resources.displayMetrics.density).toInt()
    private fun formatSize(bytes: Long) = if (bytes >= 1024 * 1024) String.format(Locale.ROOT, "%.1f MB", bytes / (1024f * 1024f)) else if (bytes >= 1024) String.format(Locale.ROOT, "%.1f KB", bytes / 1024f) else "$bytes B"
}
