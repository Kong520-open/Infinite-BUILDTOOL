package com.vdl.kong520.ui

import android.R.attr.progressBarStyleHorizontal
import android.app.Activity
import android.app.Dialog
import android.content.Context
import android.content.res.ColorStateList
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.GradientDrawable
import android.os.Handler
import android.os.Looper
import android.text.Editable
import android.text.InputFilter
import android.text.InputType
import android.text.TextWatcher
import android.view.Gravity
import android.view.ViewGroup
import android.view.Window
import android.view.WindowManager
import android.widget.Button
import android.widget.CheckBox
import android.widget.EditText
import android.widget.LinearLayout
import android.widget.ProgressBar
import android.widget.RadioButton
import android.widget.RadioGroup
import android.widget.ScrollView
import android.widget.TextView
import android.widget.Toast
import android.view.View
import com.vdl.kong520.TpModule
import java.io.File
import java.util.Locale
import java.util.concurrent.atomic.AtomicBoolean
import java.util.concurrent.atomic.AtomicLong

/** Building-export controls rendered with native Android views inside the XP host. */
object BuildExportUi {
    private data class ExportBounds(
        val x1: Int,
        val y1: Int,
        val z1: Int,
        val x2: Int,
        val y2: Int,
        val z2: Int
    )

    private data class ExportSnapshot(
        val state: Int,
        val status: String,
        val total: Long,
        val processed: Long,
        val teleportMode: Int,
        val teleportAllowed: Boolean,
        val travelTarget: TravelTarget?
    )

    private data class TravelTarget(
        val x: Int,
        val y: Int,
        val z: Int,
        val distanceBlocks: Int,
        val waitingForPlayer: Boolean,
        val batchIndex: Int,
        val batchCount: Int
    )

    private data class DetectedWorldContext(
        val worldId: String,
        val dimensionId: Int
    )

    private const val PREFS = "build_export_ui"
    private const val KEY_FILENAME = "filename"
    private const val KEY_X1 = "x1"
    private const val KEY_Y1 = "y1"
    private const val KEY_Z1 = "z1"
    private const val KEY_X2 = "x2"
    private const val KEY_Y2 = "y2"
    private const val KEY_Z2 = "z2"
    private const val KEY_SIMULATION_CHUNK_RANGE = "simulation_chunk_range"
    private const val KEY_EXPORT_CONTAINER_ITEMS = "export_container_items"
    // The native exporter owns the canonical .infinity suffix. The UI persists
    // and passes only this basename to JNI.
    private const val DEFAULT_FILENAME = "building"
    private val MIGRATABLE_OUTPUT_EXTENSIONS =
        arrayOf(".schem", ".schematic", ".bdx", ".infinity", ".IBuild")
    private const val MAX_BASE_NAME_LENGTH = 80
    private const val MAX_DIMENSION = 32767L
    private const val MAX_BLOCK_COUNT = 16L * 1024L * 1024L
    private const val MAX_COORDINATE_INPUT_LENGTH = 11
    private const val MIN_SIMULATION_CHUNK_RANGE = 4
    private const val MAX_SIMULATION_CHUNK_RANGE = 8
    private const val DEFAULT_SIMULATION_CHUNK_RANGE = 4

    private const val FIRST_ACTIVE_STATE = 1
    private const val LAST_ACTIVE_STATE = 4
    // Manual navigation may expose a dedicated native waiting state.
    private const val WAITING_FOR_PLAYER_STATE = 8
    private const val CAPTURING_CONTAINERS_STATE = 9

    private const val TELEPORT_MODE_AUTOMATIC = 1
    private const val TELEPORT_MODE_SEMI_AUTOMATIC = 2
    private const val TELEPORT_MODE_DISABLED = 3

    private val uiHandler = Handler(Looper.getMainLooper())
    private val nativeActionInFlight = AtomicBoolean(false)
    private val dialogGeneration = AtomicLong(0L)
    @Volatile private var activeDialog: Dialog? = null

    fun show(activity: Activity, context: Context) {
        if (Looper.myLooper() != Looper.getMainLooper()) {
            uiHandler.post { show(activity, context) }
            return
        }
        if (activity.isFinishing || activity.isDestroyed) return
        activeDialog?.let { current ->
            if (current.isShowing) return
            activeDialog = null
        }

        val appContext = context.applicationContext
        val prefs = appContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val generation = dialogGeneration.incrementAndGet()
        val captureInFlight = AtomicBoolean(false)
        val density = activity.resources.displayMetrics.density
        fun dp(value: Float) = (value * density).toInt()

        val screenWidth = activity.resources.displayMetrics.widthPixels
        val screenHeight = activity.resources.displayMetrics.heightPixels
        val useTwoPane = screenWidth / density >= 600f && screenWidth > screenHeight
        val compact = useTwoPane && screenHeight / density < 460f

        val dialog = Dialog(activity).apply {
            requestWindowFeature(Window.FEATURE_NO_TITLE)
            setCanceledOnTouchOutside(true)
        }
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(
                dp(if (compact) 16f else 22f),
                dp(if (compact) 12f else 18f),
                dp(if (compact) 16f else 22f),
                dp(if (compact) 12f else 20f)
            )
            background = rounded("#FFFFFEFA", dp(20f), "#FFE7E0D7")
        }

        val header = LinearLayout(activity).apply { gravity = Gravity.CENTER_VERTICAL }
        val titleColumn = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        titleColumn.addView(TextView(activity).apply {
            text = "BUILD EXPORT"
            textSize = 10f
            typeface = Typeface.DEFAULT_BOLD
            letterSpacing = 0.1f
            setTextColor(Color.parseColor("#FFAA4E31"))
        })
        titleColumn.addView(TextView(activity).apply {
            text = "建筑导出"
            textSize = 22f
            typeface = Typeface.create("serif", Typeface.BOLD)
            setTextColor(Color.parseColor("#FF302D29"))
        })
        titleColumn.addView(TextView(activity).apply {
            text = "选取两个角点，自动保存为 .infinity"
            textSize = 11f
            setTextColor(Color.parseColor("#FF77716A"))
        })
        val close = TextView(activity).apply {
            text = "×"
            textSize = 24f
            gravity = Gravity.CENTER
            setTextColor(Color.parseColor("#FF77716A"))
            background = rounded("#FFFAF7F1", dp(10f), "#FFE7E0D7")
            setOnClickListener { dialog.dismiss() }
        }
        header.addView(titleColumn, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        header.addView(close, LinearLayout.LayoutParams(dp(40f), dp(40f)))
        root.addView(header)
        root.addView(View(activity).apply {
            setBackgroundColor(Color.parseColor("#FFE7E0D7"))
        }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1f)).apply {
            topMargin = dp(if (compact) 9f else 13f)
        })

        val contentRow = LinearLayout(activity).apply {
            orientation = if (useTwoPane) LinearLayout.HORIZONTAL else LinearLayout.VERTICAL
            gravity = Gravity.TOP
        }
        val scroll = ScrollView(activity).apply { isVerticalScrollBarEnabled = false }
        val form = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        scroll.addView(form)
        val side = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            setPadding(dp(14f), dp(12f), dp(14f), dp(12f))
            background = rounded("#FFFAF7F1", dp(14f), "#FFE7E0D7")
        }
        val sideScroll = ScrollView(activity).apply {
            isVerticalScrollBarEnabled = false
            isFillViewport = false
        }
        val sideContent = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
        }
        sideScroll.addView(
            sideContent,
            ViewGroup.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                ViewGroup.LayoutParams.WRAP_CONTENT
            )
        )
        side.addView(
            sideScroll,
            LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                0,
                1f
            )
        )
        if (useTwoPane) {
            contentRow.addView(scroll, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1.25f).apply {
                marginEnd = dp(14f)
            })
            contentRow.addView(side, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 0.9f))
        } else {
            contentRow.addView(scroll, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
            contentRow.addView(side, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                minOf(dp(300f), (screenHeight * 0.42f).toInt())
            ).apply {
                topMargin = dp(10f)
            })
        }
        root.addView(contentRow, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f).apply {
            topMargin = dp(if (compact) 8f else 14f)
        })

        fun section(text: String) = TextView(activity).apply {
            this.text = text
            textSize = 11f
            typeface = Typeface.DEFAULT_BOLD
            letterSpacing = 0.06f
            setTextColor(Color.parseColor("#FFAA4E31"))
            setPadding(0, dp(10f), 0, dp(6f))
            form.addView(this)
        }

        fun input(text: String, numeric: Boolean): EditText = EditText(activity).apply {
            setText(text)
            textSize = 14f
            setTextColor(Color.parseColor("#FF302D29"))
            setHintTextColor(Color.parseColor("#FFAAA39A"))
            isSingleLine = true
            inputType = if (numeric) {
                InputType.TYPE_CLASS_NUMBER or InputType.TYPE_NUMBER_FLAG_SIGNED
            } else {
                InputType.TYPE_CLASS_TEXT or InputType.TYPE_TEXT_FLAG_NO_SUGGESTIONS
            }
            setPadding(dp(12f), 0, dp(12f), 0)
            background = rounded("#FFFFFFFF", dp(9f), "#FFDED7CE")
        }

        fun action(text: String, accent: Boolean = false): Button = Button(activity).apply {
            this.text = text
            isAllCaps = false
            textSize = if (compact) 11f else 13f
            minHeight = 0
            minimumHeight = 0
            minWidth = 0
            minimumWidth = 0
            setPadding(dp(7f), 0, dp(7f), 0)
            setTextColor(if (accent) Color.WHITE else Color.parseColor("#FF302D29"))
            background = rounded(
                if (accent) "#FFCF6846" else "#FFFFFEFA",
                dp(9f),
                if (accent) null else "#FFDED7CE"
            )
        }

        // Keep confirmations visually consistent with the injected light panel.
        fun styledConfirm(
            title: String,
            message: String,
            positiveLabel: String,
            onPositive: () -> Unit,
            negativeLabel: String? = null,
            onNegative: (() -> Unit)? = null,
            neutralLabel: String? = "暂不",
            onNeutral: (() -> Unit)? = null
        ) {
            val confirm = Dialog(activity).apply {
                requestWindowFeature(Window.FEATURE_NO_TITLE)
                setCanceledOnTouchOutside(true)
            }
            val body = LinearLayout(activity).apply {
                orientation = LinearLayout.VERTICAL
                setPadding(dp(22f), dp(20f), dp(22f), dp(16f))
                background = rounded("#FFFFFEFA", dp(20f), "#FFE7E0D7")
            }
            body.addView(TextView(activity).apply {
                text = title
                textSize = 17f
                typeface = Typeface.DEFAULT_BOLD
                setTextColor(Color.parseColor("#FF302D29"))
            })
            body.addView(TextView(activity).apply {
                text = message
                textSize = 12.5f
                setTextColor(Color.parseColor("#FF77716A"))
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
            neutralLabel?.let { addButton(it, false, onNeutral ?: {}) }
            if (negativeLabel != null && onNegative != null) addButton(negativeLabel, false, onNegative)
            addButton(positiveLabel, true, onPositive)
            body.addView(row, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            confirm.setContentView(body)
            confirm.show()
            confirm.window?.let { w ->
                w.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
                w.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
                w.attributes = w.attributes.apply { dimAmount = 0.72f }
                w.setGravity(Gravity.CENTER)
                w.setLayout(
                    minOf(dp(340f), (screenWidth * 0.9f).toInt()),
                    ViewGroup.LayoutParams.WRAP_CONTENT
                )
            }
        }

        section("01 · 输出文件")
        val storedFilename = normalizeStoredOutputName(
            prefs.getString(KEY_FILENAME, DEFAULT_FILENAME).orEmpty()
        ) ?: DEFAULT_FILENAME
        val filename = input(storedFilename, false).apply {
            hint = "文件名（自动添加 .infinity）"
            filters = arrayOf(InputFilter.LengthFilter(MAX_BASE_NAME_LENGTH))
        }
        form.addView(filename, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(48f)))
        val outputDirectory = runCatching {
            appContext.getExternalFilesDir(null)?.let { File(it, "netease") }
        }.getOrNull()
        val outputHint = TextView(activity).apply {
            text = outputDirectory?.absolutePath ?: "外部文件目录不可用"
            textSize = 10f
            setTextColor(Color.parseColor("#FF77716A"))
            setPadding(dp(4f), dp(5f), dp(4f), 0)
        }
        form.addView(outputHint)
        form.addView(TextView(activity).apply {
            text = "所有方块状态和方块实体统一保存为 .infinity"
            textSize = 10f
            setTextColor(Color.parseColor("#FF77716A"))
            setPadding(dp(4f), dp(3f), dp(4f), 0)
        })

        section("02 · 扫描分区")
        form.addView(TextView(activity).apply {
            text = "模拟区块范围（每边原版区块数，$MIN_SIMULATION_CHUNK_RANGE - $MAX_SIMULATION_CHUNK_RANGE）"
            textSize = 11f
            setTextColor(Color.parseColor("#FF77716A"))
            setPadding(dp(4f), 0, dp(4f), dp(5f))
        })
        val simulationChunkRange = input(
            prefs.getInt(KEY_SIMULATION_CHUNK_RANGE, DEFAULT_SIMULATION_CHUNK_RANGE)
                .coerceIn(MIN_SIMULATION_CHUNK_RANGE, MAX_SIMULATION_CHUNK_RANGE).toString(),
            true
        ).apply {
            hint = "$MIN_SIMULATION_CHUNK_RANGE - $MAX_SIMULATION_CHUNK_RANGE"
            filters = arrayOf(InputFilter.LengthFilter(2))
        }
        form.addView(simulationChunkRange, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT, dp(46f)
        ))
        form.addView(TextView(activity).apply {
            text = "4 表示单次扫描 4×4 区块；8 表示 8×8 区块。"
            textSize = 10f
            setTextColor(Color.parseColor("#FF77716A"))
            setPadding(dp(4f), dp(4f), dp(4f), 0)
        })

        fun coordinateInput(value: Int, hint: String) = input(value.toString(), true).apply {
            this.hint = hint
            filters = arrayOf(InputFilter.LengthFilter(MAX_COORDINATE_INPUT_LENGTH))
            // Replacing a saved coordinate should be one edit. This also keeps
            // accessibility/ADB input from having to send a burst of delete
            // key events through the game's activity before entering a value.
            setSelectAllOnFocus(true)
        }

        fun coordinateRow(fields: List<EditText>): LinearLayout = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            fields.forEachIndexed { index, field ->
                addView(field, LinearLayout.LayoutParams(0, dp(46f), 1f).apply {
                    if (index < fields.lastIndex) marginEnd = dp(6f)
                })
            }
        }

        section("03 · 角点 A")
        val x1 = coordinateInput(prefs.getInt(KEY_X1, 0), "X")
        val y1 = coordinateInput(prefs.getInt(KEY_Y1, 64), "Y")
        val z1 = coordinateInput(prefs.getInt(KEY_Z1, 0), "Z")
        val pointAFields = listOf(x1, y1, z1)
        form.addView(coordinateRow(pointAFields))
        val captureA = action("使用玩家当前位置")
        form.addView(captureA, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(38f)).apply {
            topMargin = dp(6f)
        })

        section("04 · 角点 B")
        val x2 = coordinateInput(prefs.getInt(KEY_X2, 0), "X")
        val y2 = coordinateInput(prefs.getInt(KEY_Y2, 64), "Y")
        val z2 = coordinateInput(prefs.getInt(KEY_Z2, 0), "Z")
        val pointBFields = listOf(x2, y2, z2)
        form.addView(coordinateRow(pointBFields))
        val captureB = action("使用玩家当前位置")
        form.addView(captureB, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(38f)).apply {
            topMargin = dp(6f)
            bottomMargin = dp(4f)
        })

        val rangeSummary = TextView(activity).apply {
            textSize = 12f
            setTextColor(Color.parseColor("#FF716258"))
            setPadding(dp(10f), dp(9f), dp(10f), dp(9f))
            background = rounded("#FFF6F0E8", dp(9f), "#FFEEE0D3")
        }
        form.addView(rangeSummary, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT).apply {
            topMargin = dp(10f)
        })

        sideContent.addView(TextView(activity).apply {
            text = "05 · 导出状态"
            textSize = 11f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor("#FFAA4E31"))
        })
        val stateView = TextView(activity).apply {
            textSize = 17f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor("#FF302D29"))
            setPadding(0, dp(6f), 0, dp(4f))
        }
        sideContent.addView(stateView)

        sideContent.addView(TextView(activity).apply {
            text = "06 · 传送方式"
            textSize = 11f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor("#FFAA4E31"))
            setPadding(0, dp(5f), 0, 0)
        })
        fun teleportOption(label: String) = RadioButton(activity).apply {
            text = label
            textSize = 11f
            gravity = Gravity.CENTER_VERTICAL
            maxLines = 1
            minWidth = 0
            minimumWidth = 0
            setTextColor(Color.parseColor("#FF302D29"))
            buttonTintList = ColorStateList.valueOf(Color.parseColor("#FFCF6846"))
            setPadding(0, 0, dp(3f), 0)
            id = View.generateViewId()
        }
        val automaticTeleport = teleportOption("自动传送")
        val semiAutomaticTeleport = teleportOption("半自动传送")
        val disabledTeleport = teleportOption("不传送")
        val teleportModeGroup = RadioGroup(activity).apply {
            orientation = if (useTwoPane) RadioGroup.HORIZONTAL else RadioGroup.VERTICAL
            fun optionParams() = if (useTwoPane) {
                RadioGroup.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
            } else {
                RadioGroup.LayoutParams(
                    ViewGroup.LayoutParams.MATCH_PARENT,
                    ViewGroup.LayoutParams.WRAP_CONTENT
                )
            }
            addView(automaticTeleport, optionParams())
            addView(semiAutomaticTeleport, optionParams())
            addView(disabledTeleport, optionParams())
            check(automaticTeleport.id)
        }
        sideContent.addView(teleportModeGroup, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = dp(3f) })
        val teleportModeHint = TextView(activity).apply {
            text = "自动前往每个分区；TP 验证失败时会切换为不传送。"
            textSize = 10f
            setTextColor(Color.parseColor("#FF77716A"))
            setPadding(dp(4f), 0, dp(4f), dp(5f))
        }
        sideContent.addView(teleportModeHint)
        teleportModeGroup.setOnCheckedChangeListener { _, checkedId ->
            teleportModeHint.text = when (checkedId) {
                semiAutomaticTeleport.id ->
                    "每个新分区等待确认；点击悬浮窗的“下一个分区”后传送。"
                disabledTeleport.id ->
                    "不发送传送请求；请按坐标手动前往目标分区。"
                else ->
                    "自动前往每个分区；TP 验证失败时会切换为不传送。"
            }
        }
        sideContent.addView(TextView(activity).apply {
            text = "07 · 容器物品"
            textSize = 11f
            typeface = Typeface.DEFAULT_BOLD
            setTextColor(Color.parseColor("#FFAA4E31"))
            setPadding(0, dp(5f), 0, 0)
        })
        val exportContainerItems = CheckBox(activity).apply {
            text = "导出容器内物品"
            textSize = 11f
            isChecked = prefs.getBoolean(KEY_EXPORT_CONTAINER_ITEMS, true)
            setTextColor(Color.parseColor("#FF302D29"))
            buttonTintList = ColorStateList.valueOf(Color.parseColor("#FFCF6846"))
            setPadding(0, 0, dp(3f), 0)
        }
        sideContent.addView(exportContainerItems, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ))
        sideContent.addView(TextView(activity).apply {
            text = "关闭后只导出容器方块，不读取其中的物品；可明显缩短导出时间。"
            textSize = 10f
            setTextColor(Color.parseColor("#FF77716A"))
            setPadding(dp(4f), 0, dp(4f), dp(5f))
        })
        val progress = ProgressBar(activity, null, progressBarStyleHorizontal).apply {
            max = 1000
            progressTintList = ColorStateList.valueOf(Color.parseColor("#FFCF6846"))
            progressBackgroundTintList = ColorStateList.valueOf(Color.parseColor("#FFE7E0D7"))
        }
        sideContent.addView(progress, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(8f)).apply {
            topMargin = dp(4f)
        })
        val count = TextView(activity).apply {
            textSize = 12f
            setTextColor(Color.parseColor("#FF77716A"))
            setPadding(0, dp(8f), 0, dp(4f))
        }
        sideContent.addView(count)
        val status = TextView(activity).apply {
            textSize = 11f
            setTextColor(Color.parseColor("#FF302D29"))
            setPadding(dp(10f), dp(9f), dp(10f), dp(9f))
            background = rounded("#FFFFFFFF", dp(9f), "#FFE7E0D7")
        }
        sideContent.addView(status, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT))

        val navigation = TextView(activity).apply {
            textSize = 12f
            setTextColor(Color.parseColor("#FF302D29"))
            setPadding(dp(10f), dp(9f), dp(10f), dp(9f))
            background = rounded("#FFF9EFDC", dp(9f), "#FFEED9B2")
            visibility = View.GONE
        }
        sideContent.addView(navigation, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            ViewGroup.LayoutParams.WRAP_CONTENT
        ).apply { topMargin = dp(8f) })

        val stackActions = !useTwoPane && screenWidth / density < 360f
        val actionHeight = dp(if (compact) 38f else 44f)
        val actionGap = dp(7f)
        val actions = LinearLayout(activity).apply {
            orientation = if (stackActions) LinearLayout.VERTICAL else LinearLayout.HORIZONTAL
        }
        val start = action("开始导出", true)
        val cancel = action("取消导出")
        if (stackActions) {
            actions.addView(start, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                actionHeight
            ).apply { bottomMargin = actionGap })
            actions.addView(cancel, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT,
                actionHeight
            ))
        } else {
            actions.addView(start, LinearLayout.LayoutParams(0, actionHeight, 1f).apply {
                marginEnd = actionGap
            })
            actions.addView(cancel, LinearLayout.LayoutParams(0, actionHeight, 1f))
        }
        val actionsHeight = if (stackActions) actionHeight * 2 + actionGap else actionHeight
        side.addView(actions, LinearLayout.LayoutParams(
            ViewGroup.LayoutParams.MATCH_PARENT,
            actionsHeight
        ).apply {
            topMargin = dp(10f)
        })
        side.addView(TextView(activity).apply {
            text = "关闭面板不会中断后台导出"
            textSize = 10f
            gravity = Gravity.CENTER
            setTextColor(Color.parseColor("#FF77716A"))
            setPadding(0, dp(7f), 0, 0)
        })

        val allCoordinateFields = pointAFields + pointBFields
        var latestState = 0

        fun isExportActive(state: Int): Boolean =
            state in FIRST_ACTIVE_STATE..LAST_ACTIVE_STATE ||
                state == WAITING_FOR_PLAYER_STATE || state == CAPTURING_CONTAINERS_STATE

        fun selectedTeleportMode(): Int = when (teleportModeGroup.checkedRadioButtonId) {
            semiAutomaticTeleport.id -> TELEPORT_MODE_SEMI_AUTOMATIC
            disabledTeleport.id -> TELEPORT_MODE_DISABLED
            else -> TELEPORT_MODE_AUTOMATIC
        }

        fun selectTeleportMode(mode: Int) {
            teleportModeGroup.check(
                when (mode) {
                    TELEPORT_MODE_SEMI_AUTOMATIC -> semiAutomaticTeleport.id
                    TELEPORT_MODE_DISABLED -> disabledTeleport.id
                    else -> automaticTeleport.id
                }
            )
        }

        fun readBounds(): ExportBounds? {
            val values = IntArray(allCoordinateFields.size)
            allCoordinateFields.forEachIndexed { index, field ->
                values[index] = field.text.toString().trim().toIntOrNull() ?: return null
            }
            return ExportBounds(
                values[0], values[1], values[2],
                values[3], values[4], values[5]
            )
        }

        fun updateRangeSummary() {
            val bounds = readBounds()
            if (bounds == null) {
                rangeSummary.text = "请输入有效的整数坐标"
                return
            }
            val width = inclusiveLength(bounds.x1, bounds.x2)
            val height = inclusiveLength(bounds.y1, bounds.y2)
            val depth = inclusiveLength(bounds.z1, bounds.z2)
            val volume = volumeOf(width, height, depth)
            rangeSummary.text = if (volume == null) {
                "范围：${formatCount(width)} × ${formatCount(height)} × ${formatCount(depth)}\n总体积超出可支持范围"
            } else {
                "范围：${formatCount(width)} × ${formatCount(height)} × ${formatCount(depth)}\n共 ${formatCount(volume)} 个方块位置"
            }
        }

        fun isAttached(): Boolean =
            dialogGeneration.get() == generation && activeDialog === dialog && dialog.isShowing &&
                !activity.isFinishing && !activity.isDestroyed

        var rangeSummaryUpdatePosted = false
        val rangeSummaryUpdater = Runnable {
            rangeSummaryUpdatePosted = false
            if (isAttached()) updateRangeSummary()
        }

        fun scheduleRangeSummaryUpdate() {
            if (rangeSummaryUpdatePosted) return
            rangeSummaryUpdatePosted = true
            // Let the EditText/IME finish its current mutation before reading
            // all six fields and changing layout text. Multiple edits from a
            // paste or captured position collapse into one update.
            uiHandler.post(rangeSummaryUpdater)
        }

        fun updateControls() {
            val active = isExportActive(latestState)
            val busy = nativeActionInFlight.get() || captureInFlight.get()
            start.isEnabled = !active && !busy && latestState != -1
            cancel.isEnabled = active && !busy
            val modeEnabled = !active && !busy
            teleportModeGroup.isEnabled = modeEnabled
            automaticTeleport.isEnabled = modeEnabled
            semiAutomaticTeleport.isEnabled = modeEnabled
            disabledTeleport.isEnabled = modeEnabled
            exportContainerItems.isEnabled = modeEnabled
            filename.isEnabled = !active && !busy
            simulationChunkRange.isEnabled = !active && !busy
            allCoordinateFields.forEach { it.isEnabled = !active && !busy }
            captureA.isEnabled = !active && !busy
            captureB.isEnabled = !active && !busy
            val formAlpha = if (active || busy) 0.65f else 1f
            filename.alpha = formAlpha
            simulationChunkRange.alpha = formAlpha
            allCoordinateFields.forEach { it.alpha = formAlpha }
            captureA.alpha = formAlpha
            captureB.alpha = formAlpha
            teleportModeGroup.alpha = if (modeEnabled) 1f else 0.65f
            exportContainerItems.alpha = if (modeEnabled) 1f else 0.65f
            start.alpha = if (start.isEnabled) 1f else 0.55f
            cancel.alpha = if (cancel.isEnabled) 1f else 0.55f
        }

        fun renderTravelTarget(snapshot: ExportSnapshot) {
            val target = snapshot.travelTarget
            if (!isExportActive(snapshot.state) || target == null ||
                (snapshot.teleportMode == TELEPORT_MODE_AUTOMATIC && snapshot.teleportAllowed)) {
                navigation.visibility = View.GONE
                navigation.text = ""
                return
            }
            navigation.visibility = View.VISIBLE
            val regionText = if (target.batchIndex > 0 && target.batchCount > 0) {
                "分区 ${target.batchIndex} / ${target.batchCount}\n"
            } else {
                ""
            }
            val distanceText = if (target.distanceBlocks >= 0) {
                "当前距离 ${formatCount(target.distanceBlocks.toLong())} 格"
            } else {
                "当前距离暂不可用"
            }
            navigation.text = when {
                snapshot.teleportMode == TELEPORT_MODE_SEMI_AUTOMATIC &&
                    (target.waitingForPlayer || snapshot.state == WAITING_FOR_PLAYER_STATE) ->
                    regionText + "等待确认前往下一扫描区域\n" +
                        "X=${target.x}  Z=${target.z} · $distanceText"
                target.waitingForPlayer || snapshot.state == WAITING_FOR_PLAYER_STATE ->
                    regionText + "请前往下一扫描区域\n" +
                        "X=${target.x}  Z=${target.z}，Y 保持当前安全高度\n" +
                        "$distanceText；进入水平范围后将自动继续扫描"
                else ->
                    regionText + "目标区域：X=${target.x}  Z=${target.z}，Y 保持当前安全高度\n" +
                        "$distanceText；正在加载区域，请稍候"
            }
        }

        fun renderSnapshot(snapshot: ExportSnapshot) {
            val wasActive = isExportActive(latestState)
            latestState = snapshot.state
            val active = isExportActive(snapshot.state)
            if (active) {
                selectTeleportMode(snapshot.teleportMode)
            } else if (wasActive) {
                selectTeleportMode(TELEPORT_MODE_AUTOMATIC)
            }
            stateView.text = stateLabel(snapshot.state)
            val total = snapshot.total.coerceAtLeast(0L)
            val processed = snapshot.processed.coerceAtLeast(0L).let {
                if (total > 0L) it.coerceAtMost(total) else it
            }
            val progressValue = if (total > 0L) {
                ((processed.toDouble() / total.toDouble()) * 1000.0).toInt().coerceIn(0, 1000)
            } else {
                0
            }
            progress.progress = progressValue
            val percent = progressValue / 10f
            count.text = if (total > 0L) {
                String.format(
                    Locale.ROOT,
                    "已处理 %,d / %,d · %.1f%%",
                    processed,
                    total,
                    percent
                )
            } else {
                "已处理 ${formatCount(processed)} 个方块位置"
            }
            status.text = snapshot.status.ifBlank { defaultStatus(snapshot.state) }
            renderTravelTarget(snapshot)
            updateControls()
        }

        fun refreshFromNative(): ExportSnapshot {
            val snapshot = readNativeSnapshot()
            renderSnapshot(snapshot)
            return snapshot
        }

        val coordinateWatcher = object : TextWatcher {
            override fun beforeTextChanged(s: CharSequence?, start: Int, count: Int, after: Int) = Unit
            override fun onTextChanged(s: CharSequence?, start: Int, before: Int, count: Int) = Unit
            override fun afterTextChanged(s: Editable?) = scheduleRangeSummaryUpdate()
        }
        allCoordinateFields.forEach { it.addTextChangedListener(coordinateWatcher) }
        updateRangeSummary()

        fun capturePosition(
            target: List<EditText>,
            label: String,
            keyX: String,
            keyY: String,
            keyZ: String
        ) {
            if (!captureInFlight.compareAndSet(false, true)) return
            updateControls()
            Thread({
                val position = runCatching { TpModule.getBuildExportPlayerBlockPosition() }
                    .getOrNull()
                    ?.takeIf { it.size >= 3 }
                captureInFlight.set(false)
                uiHandler.post {
                    if (!isAttached()) return@post
                    if (position == null) {
                        Toast.makeText(appContext, "无法读取玩家位置，请先进入已加载的世界", Toast.LENGTH_SHORT).show()
                    } else {
                        target[0].setText(position[0].toString())
                        target[1].setText(position[1].toString())
                        target[2].setText(position[2].toString())
                        // Persist the captured corner immediately so it survives
                        // closing the panel without submitting the export.
                        prefs.edit()
                            .putInt(keyX, position[0])
                            .putInt(keyY, position[1])
                            .putInt(keyZ, position[2])
                            .apply()
                        Toast.makeText(appContext, "已记录角点 $label", Toast.LENGTH_SHORT).show()
                    }
                    updateControls()
                }
            }, "build-export-capture-$label").start()
        }

        fun saveInputs(
            bounds: ExportBounds,
            outputName: String,
            simulationRange: Int,
            includeContainerItems: Boolean
        ) {
            prefs.edit()
                .putString(KEY_FILENAME, outputName)
                .putInt(KEY_X1, bounds.x1)
                .putInt(KEY_Y1, bounds.y1)
                .putInt(KEY_Z1, bounds.z1)
                .putInt(KEY_X2, bounds.x2)
                .putInt(KEY_Y2, bounds.y2)
                .putInt(KEY_Z2, bounds.z2)
                .putInt(KEY_SIMULATION_CHUNK_RANGE, simulationRange)
                .putBoolean(KEY_EXPORT_CONTAINER_ITEMS, includeContainerItems)
                .apply()
        }

        fun launchExport(
            outputFile: File,
            outputName: String,
            bounds: ExportBounds,
            simulationRange: Int,
            replaceCheckpoint: Boolean = false
        ) {
            val current = readNativeSnapshot()
            renderSnapshot(current)
            if (isExportActive(current.state)) {
                if (BuildExportProgressOverlay.show(activity)) dialog.dismiss()
                Toast.makeText(appContext, "已有建筑导出任务正在运行", Toast.LENGTH_SHORT).show()
                return
            }
            if (current.state == -1) {
                Toast.makeText(appContext, "原生建筑导出模块不可用", Toast.LENGTH_SHORT).show()
                return
            }
            if (!nativeActionInFlight.compareAndSet(false, true)) return
            val teleportMode = selectedTeleportMode()
            val includeContainerItems = exportContainerItems.isChecked
            status.text = when (teleportMode) {
                TELEPORT_MODE_SEMI_AUTOMATIC ->
                    "等待确认后传送到首个导出分区中心…"
                TELEPORT_MODE_DISABLED ->
                    "不传送；请按悬浮窗坐标前往导出分区…"
                else ->
                    "将传送到首个导出分区中心，并验证坐标…"
            }
            updateControls()
            Thread({
                var accepted = false
                var message = "建筑导出启动失败"
                try {
                    val worldContext = readNativeWorldContext()
                    if (worldContext == null) {
                        message = "无法确认稳定的世界身份，请进入目标世界后重试"
                    } else {
                        accepted = TpModule.startBuildExport(
                            outputFile.absolutePath,
                            bounds.x1,
                            bounds.y1,
                            bounds.z1,
                            bounds.x2,
                            bounds.y2,
                            bounds.z2,
                            teleportMode,
                            worldContext.worldId,
                            worldContext.dimensionId,
                            replaceCheckpoint,
                            simulationRange,
                            includeContainerItems
                        )
                    }
                    if (accepted) {
                        message = runCatching { TpModule.getBuildExportStatus()?.trim().orEmpty() }
                            .getOrDefault("")
                            .ifBlank { "建筑导出已开始" }
                    } else {
                        message = runCatching { TpModule.getBuildExportStatus()?.trim().orEmpty() }
                            .getOrDefault("")
                            .ifBlank { "原生导出器拒绝启动" }
                    }
                } catch (throwable: Throwable) {
                    message = throwable.message?.takeIf { it.isNotBlank() }
                        ?: "原生建筑导出接口异常"
                } finally {
                    nativeActionInFlight.set(false)
                }
                if (accepted) {
                    saveInputs(bounds, outputName, simulationRange, includeContainerItems)
                }
                uiHandler.post {
                    val overlayShown = accepted && !activity.isFinishing && !activity.isDestroyed &&
                        BuildExportProgressOverlay.show(activity)
                    if (!isAttached()) return@post
                    filename.setText(outputName)
                    val snapshot = refreshFromNative()
                    status.text = if (accepted) snapshot.status.ifBlank { message } else message
                    if (overlayShown) dialog.dismiss()
                    if (accepted && teleportMode == TELEPORT_MODE_AUTOMATIC &&
                        snapshot.teleportMode == TELEPORT_MODE_DISABLED) {
                        Toast.makeText(
                            appContext,
                            "未到达首个分区中心，已关闭自动传送；请按悬浮窗坐标移动",
                            Toast.LENGTH_LONG
                        ).show()
                    } else if (!accepted) {
                        Toast.makeText(appContext, message, Toast.LENGTH_LONG).show()
                    }
                }
            }, "build-export-start").start()
        }

        fun resumeExport(outputFile: File) {
            val current = readNativeSnapshot()
            renderSnapshot(current)
            if (isExportActive(current.state)) {
                if (BuildExportProgressOverlay.show(activity)) dialog.dismiss()
                Toast.makeText(appContext, "已有建筑导出任务正在运行", Toast.LENGTH_SHORT).show()
                return
            }
            if (!nativeActionInFlight.compareAndSet(false, true)) return
            val teleportMode = selectedTeleportMode()
            status.text = when (teleportMode) {
                TELEPORT_MODE_SEMI_AUTOMATIC ->
                    "等待确认后传送到恢复分区中心…"
                TELEPORT_MODE_DISABLED ->
                    "不传送；请按悬浮窗坐标前往恢复分区…"
                else ->
                    "将传送到恢复分区中心，并验证坐标…"
            }
            updateControls()
            Thread({
                var accepted = false
                var message = "建筑导出断点恢复失败"
                try {
                    val worldContext = readNativeWorldContext()
                    if (worldContext == null) {
                        message = "当前世界没有可用于断点恢复的稳定身份"
                    } else {
                        accepted = TpModule.resumeBuildExport(
                            outputFile.absolutePath,
                            teleportMode,
                            worldContext.worldId,
                            worldContext.dimensionId
                        )
                        message = if (accepted) {
                            runCatching { TpModule.getBuildExportStatus()?.trim().orEmpty() }
                                .getOrDefault("")
                                .ifBlank { "已从断点继续建筑导出" }
                        } else {
                            runCatching { TpModule.getBuildExportStatus()?.trim().orEmpty() }
                                .getOrDefault("")
                                .ifBlank { "断点损坏，或不属于当前世界与维度" }
                        }
                    }
                } catch (throwable: Throwable) {
                    message = throwable.message?.takeIf { it.isNotBlank() } ?: message
                } finally {
                    nativeActionInFlight.set(false)
                }
                uiHandler.post {
                    val overlayShown = accepted && !activity.isFinishing && !activity.isDestroyed &&
                        BuildExportProgressOverlay.show(activity)
                    if (!isAttached()) return@post
                    val snapshot = refreshFromNative()
                    status.text = if (accepted) snapshot.status.ifBlank { message } else message
                    if (overlayShown) dialog.dismiss()
                    if (accepted && teleportMode == TELEPORT_MODE_AUTOMATIC &&
                        snapshot.teleportMode == TELEPORT_MODE_DISABLED) {
                        Toast.makeText(
                            appContext,
                            "恢复时未到达分区中心，已改为手动导航",
                            Toast.LENGTH_LONG
                        ).show()
                    } else if (!accepted) {
                        Toast.makeText(appContext, message, Toast.LENGTH_LONG).show()
                    }
                }
            }, "build-export-resume").start()
        }

        fun discardCheckpoint(outputFile: File) {
            if (!nativeActionInFlight.compareAndSet(false, true)) return
            status.text = "正在删除建筑导出断点…"
            updateControls()
            Thread({
                var deleted = false
                var message = "建筑导出断点删除失败"
                try {
                    deleted = TpModule.discardBuildExportCheckpoint(outputFile.absolutePath)
                    message = if (deleted) {
                        "已删除断点，已导出的 .infinity 文件不会受影响"
                    } else {
                        runCatching { TpModule.getBuildExportStatus()?.trim().orEmpty() }
                            .getOrDefault("")
                            .ifBlank { message }
                    }
                } catch (throwable: Throwable) {
                    message = throwable.message?.takeIf { it.isNotBlank() } ?: message
                } finally {
                    nativeActionInFlight.set(false)
                }
                uiHandler.post {
                    if (!isAttached()) return@post
                    refreshFromNative()
                    status.text = message
                    Toast.makeText(appContext, message, Toast.LENGTH_LONG).show()
                }
            }, "build-export-discard").start()
        }

        captureA.setOnClickListener { capturePosition(pointAFields, "A", KEY_X1, KEY_Y1, KEY_Z1) }
        captureB.setOnClickListener { capturePosition(pointBFields, "B", KEY_X2, KEY_Y2, KEY_Z2) }

        start.setOnClickListener {
            val bounds = readBounds()
            if (bounds == null) {
                Toast.makeText(appContext, "请输入有效的整数坐标", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            val width = inclusiveLength(bounds.x1, bounds.x2)
            val height = inclusiveLength(bounds.y1, bounds.y2)
            val depth = inclusiveLength(bounds.z1, bounds.z2)
            if (volumeOf(width, height, depth) == null) {
                Toast.makeText(appContext, "导出范围过大", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            val simulationRange = simulationChunkRange.text.toString().trim().toIntOrNull()
            if (simulationRange == null ||
                simulationRange !in MIN_SIMULATION_CHUNK_RANGE..MAX_SIMULATION_CHUNK_RANGE) {
                Toast.makeText(
                    appContext,
                    "模拟区块范围必须在 $MIN_SIMULATION_CHUNK_RANGE 到 $MAX_SIMULATION_CHUNK_RANGE 之间",
                    Toast.LENGTH_SHORT
                ).show()
                return@setOnClickListener
            }
            val outputName = normalizeUserOutputName(filename.text.toString())
            if (outputName == null) {
                Toast.makeText(
                    appContext,
                    "文件名无效：请输入不带扩展名的普通文件名",
                    Toast.LENGTH_SHORT
                ).show()
                return@setOnClickListener
            }
            val directory = outputDirectory
            if (directory == null) {
                Toast.makeText(appContext, "外部文件目录不可用", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            val outputFile = runCatching {
                if (!directory.isDirectory && !directory.mkdirs() && !directory.isDirectory) {
                    throw IllegalStateException("无法创建 files/netease 目录")
                }
                val canonicalDirectory = directory.canonicalFile
                File(canonicalDirectory, outputName).canonicalFile.also { candidate ->
                    check(candidate.parentFile == canonicalDirectory) { "导出路径无效" }
                }
            }.getOrElse { throwable ->
                Toast.makeText(
                    appContext,
                    throwable.message?.takeIf { it.isNotBlank() } ?: "无法创建导出文件",
                    Toast.LENGTH_SHORT
                ).show()
                return@setOnClickListener
            }
            // Check the canonical destination and legacy publications before
            // offering an overwrite. Native always appends .infinity.
            val outputCandidates = listOf(
                outputFile,
                File(outputFile.absolutePath + ".infinity"),
                File(outputFile.absolutePath + ".schem"),
                File(outputFile.absolutePath + ".schematic"),
                File(outputFile.absolutePath + ".bdx"),
                File(outputFile.absolutePath + ".IBuild"),
                File(outputFile.absolutePath + ".ibuild")
            )
            val existingOutputs = outputCandidates.filter { it.exists() }
            if (existingOutputs.any { !it.isFile }) {
                Toast.makeText(appContext, "同名路径不是普通文件，无法覆盖", Toast.LENGTH_SHORT).show()
                return@setOnClickListener
            }
            val existingOutputNames = existingOutputs.joinToString("、") { it.name }
            val resumableCheckpoint = runCatching {
                TpModule.hasBuildExportCheckpoint(outputFile.absolutePath)
            }.getOrDefault(false)
            val hasCheckpointArtifacts =
                File(outputFile.absolutePath + ".export.checkpoint").isFile ||
                File(outputFile.absolutePath + ".export.blocks").isFile
            if (resumableCheckpoint) {
                val existingOutputWarning = if (existingOutputs.isNotEmpty()) {
                    "当前已有同名导出文件（$existingOutputNames）；新文件成功写入后会替换这些旧格式文件。"
                } else {
                    "重新开始会在新任务通过参数与内存校验后替换旧断点数据。"
                }
                styledConfirm(
                    title = "发现未完成的建筑导出数据",
                    message = "${outputFile.name} 存在断点或方块日志。继续时会先校验数据，" +
                        "校验通过后使用断点中保存的范围。$existingOutputWarning",
                    positiveLabel = "继续导出",
                    onPositive = { if (isAttached()) resumeExport(outputFile) },
                    negativeLabel = "重新开始",
                    onNegative = {
                        if (isAttached()) launchExport(outputFile, outputName, bounds, simulationRange, true)
                    },
                    neutralLabel = "删除断点",
                    onNeutral = {
                        if (isAttached()) styledConfirm(
                            title = "删除导出断点？",
                            message = "只删除未完成进度，不会删除已经生成的 .infinity 文件。",
                            positiveLabel = "删除",
                            onPositive = { if (isAttached()) discardCheckpoint(outputFile) },
                            neutralLabel = "返回"
                        )
                    }
                )
            } else if (hasCheckpointArtifacts) {
                styledConfirm(
                    title = "发现不完整的导出残留",
                    message = "${outputFile.name} 的断点快照不完整，无法安全续导。" +
                        "可重新开始或删除残留；已生成的 .infinity 文件不会被删除。",
                    positiveLabel = "重新开始",
                    onPositive = {
                        if (isAttached()) launchExport(outputFile, outputName, bounds, simulationRange, true)
                    },
                    negativeLabel = "删除残留",
                    onNegative = {
                        if (isAttached()) styledConfirm(
                            title = "删除导出残留？",
                            message = "只删除未完成进度，不会删除已经生成的 .infinity 文件。",
                            positiveLabel = "删除",
                            onPositive = { if (isAttached()) discardCheckpoint(outputFile) },
                            neutralLabel = "返回"
                        )
                    },
                    neutralLabel = "返回"
                )
            } else if (existingOutputs.isNotEmpty()) {
                styledConfirm(
                    title = "继续导出？",
                    message = "$existingOutputNames 已存在。新导出将生成 ${outputName}.infinity，" +
                        "并在成功写入后替换同名旧格式文件。",
                    positiveLabel = "覆盖并导出",
                    onPositive = {
                        if (isAttached()) launchExport(outputFile, outputName, bounds, simulationRange)
                    },
                    neutralLabel = "返回"
                )
            } else {
                launchExport(outputFile, outputName, bounds, simulationRange)
            }
        }

        cancel.setOnClickListener {
            if (!isExportActive(latestState) || !nativeActionInFlight.compareAndSet(false, true)) {
                return@setOnClickListener
            }
            status.text = "正在取消建筑导出…"
            updateControls()
            Thread({
                var cancelled = false
                var message = "建筑导出取消失败"
                try {
                    TpModule.cancelBuildExport()
                    cancelled = true
                    message = runCatching { TpModule.getBuildExportStatus()?.trim().orEmpty() }
                        .getOrDefault("")
                        .ifBlank { "已请求取消建筑导出" }
                } catch (throwable: Throwable) {
                    message = throwable.message?.takeIf { it.isNotBlank() } ?: message
                } finally {
                    nativeActionInFlight.set(false)
                }
                uiHandler.post {
                    if (!isAttached()) return@post
                    val snapshot = refreshFromNative()
                    status.text = snapshot.status.ifBlank { message }
                    if (!cancelled) Toast.makeText(appContext, message, Toast.LENGTH_LONG).show()
                }
            }, "build-export-cancel").start()
        }

        val ticker = object : Runnable {
            override fun run() {
                if (!isAttached()) return
                if (!isExportActive(latestState)) {
                    // An idle export cannot change state except through this
                    // panel's explicit start/resume paths, which refresh the
                    // snapshot themselves. Do not poll JNI while editing: an
                    // accessibility service can mutate EditText without giving
                    // it focus, so a hasFocus() guard still leaves that race.
                    uiHandler.postDelayed(this, 800L)
                    return
                }
                val snapshot = refreshFromNative()
                if (!isAttached()) return
                val delay = if (isExportActive(snapshot.state)) 350L else 800L
                uiHandler.postDelayed(this, delay)
            }
        }
        dialog.setOnDismissListener {
            rangeSummaryUpdatePosted = false
            uiHandler.removeCallbacks(rangeSummaryUpdater)
            uiHandler.removeCallbacks(ticker)
            if (activeDialog === dialog) activeDialog = null
            dialogGeneration.compareAndSet(generation, generation + 1L)
        }

        dialog.setContentView(root)
        dialog.show()
        activeDialog = dialog
        dialog.window?.let { window ->
            window.setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            window.addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
            window.attributes = window.attributes.apply { dimAmount = 0.72f }
            window.setGravity(Gravity.CENTER)
            window.setLayout(
                minOf(dp(if (useTwoPane) 760f else 480f), (screenWidth * 0.94f).toInt()),
                minOf(
                    dp(if (useTwoPane) 610f else 700f),
                    (screenHeight * (if (compact) 0.94f else 0.9f)).toInt()
                )
            )
        }
        val initialSnapshot = refreshFromNative()
        if (isExportActive(initialSnapshot.state)) {
            BuildExportProgressOverlay.show(activity)
        } else {
            // Proactively surface an interrupted export on panel open so the
            // user does not have to reproduce the exact filename + coordinates
            // and press 开始导出 just to discover a resumable checkpoint.
            val scanDir = outputDirectory
            if (scanDir != null) {
                Thread({
                    val resumable = runCatching {
                        scanDir.listFiles { f ->
                            f.isFile && f.name.endsWith(".export.checkpoint")
                        }?.filter { cp ->
                            runCatching {
                                TpModule.hasBuildExportCheckpoint(
                                    cp.absolutePath.removeSuffix(".export.checkpoint"))
                            }.getOrDefault(false)
                        }?.maxByOrNull { it.lastModified() }
                    }.getOrNull()?.let { cp ->
                        File(cp.absolutePath.removeSuffix(".export.checkpoint"))
                    }
                    if (resumable != null) uiHandler.post {
                        if (!isAttached()) return@post
                        if (isExportActive(readNativeSnapshot().state)) return@post
                        styledConfirm(
                            title = "发现未完成的建筑导出",
                            message = "${resumable.name} 有未完成的导出进度，可从断点继续" +
                                "（继续会先校验数据并使用断点中保存的范围）。",
                            positiveLabel = "继续导出",
                            onPositive = {
                                if (isAttached()) {
                                    filename.setText(
                                        normalizeStoredOutputName(resumable.name) ?: resumable.name
                                    )
                                    resumeExport(resumable)
                                }
                            },
                            negativeLabel = "放弃任务",
                            onNegative = {
                                if (isAttached()) styledConfirm(
                                    title = "放弃该导出任务？",
                                    message = "将删除未完成的断点进度，不会删除已经生成的 .infinity 文件。",
                                    positiveLabel = "放弃",
                                    onPositive = { if (isAttached()) discardCheckpoint(resumable) },
                                    neutralLabel = "返回"
                                )
                            }
                        )
                    }
                }, "build-export-resume-scan").start()
            }
        }
        uiHandler.postDelayed(ticker, 350L)
    }

    private fun readNativeSnapshot(): ExportSnapshot = try {
        val state = TpModule.getBuildExportState()
        val status = TpModule.getBuildExportStatus()?.trim().orEmpty()
        val total = TpModule.getBuildExportTotalBlocks()
        val processed = TpModule.getBuildExportProcessedBlocks()
        val teleportMode = runCatching { TpModule.getBuildExportTeleportMode() }
            .getOrDefault(TELEPORT_MODE_AUTOMATIC)
            .takeIf { it in TELEPORT_MODE_AUTOMATIC..TELEPORT_MODE_DISABLED }
            ?: TELEPORT_MODE_AUTOMATIC
        val teleportAllowed = TpModule.isBuildExportTeleportAllowed()
        val travelTarget = TpModule.getBuildExportTravelTarget()
            ?.takeIf { it.size >= 5 }
            ?.let {
                TravelTarget(
                    x = it[0],
                    y = it[1],
                    z = it[2],
                    distanceBlocks = it[3],
                    waitingForPlayer = it[4] != 0,
                    batchIndex = if (it.size > 5) it[5] else 0,
                    batchCount = if (it.size > 6) it[6] else 0
                )
            }
        ExportSnapshot(state, status, total, processed, teleportMode, teleportAllowed, travelTarget)
    } catch (_: Throwable) {
        ExportSnapshot(-1, "原生建筑导出模块不可用", 0L, 0L,
            TELEPORT_MODE_AUTOMATIC, false, null)
    }

    private fun readNativeWorldContext(): DetectedWorldContext? {
        // The stable identity and dimension component may become available a
        // few ticks apart while entering a world. This runs only on export
        // worker threads, so a short bounded retry does not block the UI.
        for (attempt in 0 until 3) {
            val worldId = runCatching { TpModule.getWorldId() }.getOrNull()?.trim().orEmpty()
            val separator = worldId.lastIndexOf('|')
            val dimensionId = if (separator > 0 && separator < worldId.lastIndex) {
                worldId.substring(separator + 1).toIntOrNull()
            } else {
                null
            }
            val identity = if (separator > 0) worldId.substring(0, separator) else ""
            if (dimensionId != null && identity.matches(Regex("^stable:v1:[0-9a-f]{64}$"))) {
                return DetectedWorldContext(worldId, dimensionId)
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

    private fun normalizeStoredOutputName(raw: String): String? =
        normalizeOutputName(raw, allowLegacyExtension = true)

    private fun normalizeUserOutputName(raw: String): String? =
        normalizeOutputName(raw, allowLegacyExtension = false)

    private fun normalizeOutputName(raw: String, allowLegacyExtension: Boolean): String? {
        val name = raw.trim()
        val maximumInputLength = MAX_BASE_NAME_LENGTH + if (allowLegacyExtension) {
            MIGRATABLE_OUTPUT_EXTENSIONS.maxOfOrNull { it.length } ?: 0
        } else {
            0
        }
        if (name.isEmpty() || name.length > maximumInputLength) return null
        if (name != name.trimEnd('.', ' ')) return null
        if (name.any { character ->
                character.code < 32 || character == '/' || character == '\\' ||
                    character == ':' || character == '*' || character == '?' ||
                    character == '"' || character == '<' || character == '>' || character == '|'
            }) return null
        // Strip managed extensions from saved preferences and checkpoint
        // names, but never persist or pass a user-selected extension to native.
        // Unknown suffixes remain invalid so `name.format` cannot accidentally
        // become a custom export type.
        val legacyExtension = if (allowLegacyExtension) {
            MIGRATABLE_OUTPUT_EXTENSIONS.firstOrNull { name.endsWith(it, ignoreCase = true) }
        } else {
            null
        }
        val baseName = if (legacyExtension != null) {
            name.dropLast(legacyExtension.length)
        } else {
            if (name.contains('.')) return null
            name
        }
        if (baseName.isBlank() || baseName == "." || baseName == ".." ||
            baseName.length > MAX_BASE_NAME_LENGTH || baseName.startsWith('.') ||
            baseName.contains('.')) return null
        return baseName
    }

    private fun inclusiveLength(first: Int, second: Int): Long =
        if (first <= second) second.toLong() - first.toLong() + 1L
        else first.toLong() - second.toLong() + 1L

    private fun volumeOf(width: Long, height: Long, depth: Long): Long? {
        if (width !in 1L..MAX_DIMENSION || height !in 1L..MAX_DIMENSION ||
            depth !in 1L..MAX_DIMENSION) return null
        return runCatching {
            Math.multiplyExact(Math.multiplyExact(width, height), depth)
        }.getOrNull()?.takeIf { it <= MAX_BLOCK_COUNT }
    }

    private fun formatCount(value: Long): String = String.format(Locale.ROOT, "%,d", value)

    private fun stateLabel(state: Int): String = when (state) {
        0 -> "等待导出"
        1 -> "正在准备导出"
        2 -> "正在加载区域"
        3 -> "正在扫描方块"
        4 -> "正在写入文件"
        5 -> "导出完成"
        6 -> "导出失败"
        7 -> "导出已取消"
        WAITING_FOR_PLAYER_STATE -> "等待前往目标区域"
        CAPTURING_CONTAINERS_STATE -> "正在读取容器"
        else -> "导出模块不可用"
    }

    private fun defaultStatus(state: Int): String = when (state) {
        0 -> "设置导出范围后开始"
        1 -> "正在验证导出参数"
        2 -> "正在加载导出范围所需区块"
        3 -> "正在读取方块数据"
        4 -> "正在生成建筑文件（.infinity）"
        5 -> "建筑文件已保存到 files/netease/"
        6 -> "建筑导出失败"
        7 -> "建筑导出已取消"
        WAITING_FOR_PLAYER_STATE -> "请按导航卡前往下一扫描区域"
        CAPTURING_CONTAINERS_STATE -> "正在逐个读取容器内物品"
        else -> "请确认 Hook 和原生模块已加载"
    }

    private fun rounded(fill: String, radius: Int, stroke: String? = null) = GradientDrawable().apply {
        setColor(Color.parseColor(fill))
        cornerRadius = radius.toFloat()
        if (stroke != null) setStroke(1, Color.parseColor(stroke))
    }
}
