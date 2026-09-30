package com.kong.buildtool

import android.app.Activity
import android.app.Dialog
import android.graphics.Canvas
import android.graphics.Color
import android.graphics.ColorFilter
import android.graphics.Paint
import android.graphics.Path
import android.graphics.PixelFormat
import android.graphics.RectF
import android.graphics.Typeface
import android.graphics.drawable.ColorDrawable
import android.graphics.drawable.Drawable
import android.graphics.drawable.GradientDrawable
import android.text.TextUtils
import android.view.Gravity
import android.view.View
import android.view.ViewGroup
import android.view.Window
import android.view.WindowManager
import android.widget.ImageView
import android.widget.LinearLayout
import android.widget.TextView

/** Warm, compact, resource-free panel shown inside the hooked game process. */
object BuildToolsPanelPopup {
    private const val PAPER = "#FFFEFA"
    private const val CREAM = "#FAF7F1"
    private const val INK = "#302D29"
    private const val MUTED = "#77716A"
    private const val LINE = "#E7E0D7"
    private const val ACCENT_DARK = "#AA4E31"
    private const val GREEN = "#3F765C"

    private var activeDialog: Dialog? = null

    fun show(activity: Activity) {
        if (activity.isFinishing || activity.isDestroyed) return
        activeDialog?.let { if (it.isShowing) return }

        val metrics = activity.resources.displayMetrics
        val density = metrics.density
        fun dp(value: Float) = (value * density + .5f).toInt()

        val panelWidth = minOf(dp(620f), (metrics.widthPixels * .92f).toInt())
        val panelHeight = minOf(dp(300f), (metrics.heightPixels * .86f).toInt())
        val narrow = panelWidth < dp(440f)
        val short = panelHeight < dp(250f)
        val veryShort = panelHeight < dp(205f)
        // The two-line, icon-above-text cards need about 65 dp each.  Short
        // viewports keep the same 2x2 grid but use horizontal cards instead.
        val stackedCards = narrow && panelHeight >= dp(295f)
        val pad = dp(if (veryShort) 10f else if (short) 14f else 20f)
        val gap = dp(if (veryShort) 5f else 8f)
        val iconSize = dp(if (narrow) 26f else if (veryShort) 26f else 40f)

        val dialog = Dialog(activity).apply { requestWindowFeature(Window.FEATURE_NO_TITLE) }
        val root = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            layoutParams = ViewGroup.LayoutParams(panelWidth, panelHeight)
            setPadding(pad, pad, pad, pad)
            background = rounded(PAPER, dp(19f), LINE)
            elevation = dp(10f).toFloat()
        }

        val header = LinearLayout(activity).apply {
            orientation = LinearLayout.HORIZONTAL
            gravity = Gravity.CENTER_VERTICAL
        }
        val headerText = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        headerText.addView(TextView(activity).apply {
            text = "BUILD TOOLS"
            textSize = if (veryShort) 8f else 9f
            letterSpacing = .14f
            typeface = Typeface.create("sans-serif-medium", Typeface.BOLD)
            setTextColor(Color.parseColor(ACCENT_DARK))
        })
        headerText.addView(TextView(activity).apply {
            text = "建筑工具"
            textSize = if (veryShort) 18f else if (short) 20f else 24f
            typeface = Typeface.create("serif", Typeface.BOLD)
            setTextColor(Color.parseColor(INK))
        })
        header.addView(headerText, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
        header.addView(TextView(activity).apply {
            text = "×"
            contentDescription = "关闭"
            gravity = Gravity.CENTER
            textSize = 20f
            setTextColor(Color.parseColor(MUTED))
            background = rounded(CREAM, dp(8f), LINE)
            isClickable = true
            setOnClickListener { dialog.dismiss() }
        }, LinearLayout.LayoutParams(dp(if (veryShort) 26f else 30f), dp(if (veryShort) 26f else 30f)))
        root.addView(header)

        if (!veryShort) {
            val summary = LinearLayout(activity).apply {
                orientation = LinearLayout.HORIZONTAL
                gravity = Gravity.CENTER_VERTICAL
            }
            summary.addView(TextView(activity).apply {
                text = "选择需要使用的功能"
                textSize = if (short) 10f else 11f
                setTextColor(Color.parseColor(MUTED))
            }, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f))
            summary.addView(TextView(activity).apply {
                text = "本地可用"
                textSize = 10f
                maxLines = 1
                ellipsize = TextUtils.TruncateAt.END
                setTextColor(Color.parseColor(GREEN))
                setPadding(dp(8f), dp(4f), dp(8f), dp(4f))
                background = rounded("#E5F2E8", dp(15f), "#D5E8D8")
            }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.WRAP_CONTENT, ViewGroup.LayoutParams.WRAP_CONTENT))
            root.addView(summary, LinearLayout.LayoutParams(
                ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT
            ).apply { topMargin = dp(if (short) 3f else 5f); bottomMargin = dp(if (short) 6f else 10f) })
        } else {
            root.addView(View(activity).apply { setBackgroundColor(Color.parseColor(LINE)) },
                LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(1f)).apply {
                    topMargin = dp(5f)
                    bottomMargin = dp(6f)
                })
        }

        val grid = LinearLayout(activity).apply { orientation = LinearLayout.VERTICAL }
        val firstRow = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        val secondRow = LinearLayout(activity).apply { orientation = LinearLayout.HORIZONTAL }
        addOption(activity, firstRow, dialog, "建筑导入", "导入建筑模型文件", BuildToolsLauncher.Tool.IMPORT,
            narrow, stackedCards, veryShort, iconSize, gap)
        addOption(activity, firstRow, dialog, "图片导入", "将 PNG/JPG 转换为方块画", BuildToolsLauncher.Tool.PIXEL_ART,
            narrow, stackedCards, veryShort, iconSize, 0)
        addOption(activity, secondRow, dialog, "建筑导出", "导出工程文件数据", BuildToolsLauncher.Tool.EXPORT,
            narrow, stackedCards, veryShort, iconSize, gap)
        addOption(activity, secondRow, dialog, "建筑投影", "生成建筑投影图纸", BuildToolsLauncher.Tool.PROJECTION,
            narrow, stackedCards, veryShort, iconSize, 0)
        grid.addView(firstRow, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f).apply {
            bottomMargin = gap
        })
        grid.addView(secondRow, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))
        root.addView(grid, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, 0, 1f))

        if (!short) {
            root.addView(TextView(activity).apply {
                text = "建筑导入 · 图片导入 · 建筑导出 · 建筑投影"
                textSize = 10f
                maxLines = 1
                ellipsize = TextUtils.TruncateAt.END
                setTextColor(Color.parseColor(MUTED))
                gravity = Gravity.CENTER_VERTICAL
            }, LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, dp(21f)).apply {
                topMargin = dp(6f)
            })
        }

        dialog.setContentView(root)
        dialog.setOnDismissListener { if (activeDialog === dialog) activeDialog = null }
        dialog.show()
        dialog.window?.apply {
            setBackgroundDrawable(ColorDrawable(Color.TRANSPARENT))
            addFlags(WindowManager.LayoutParams.FLAG_DIM_BEHIND)
            attributes = attributes.apply { dimAmount = .45f }
            setLayout(panelWidth, panelHeight)
        }
        activeDialog = dialog
    }

    private fun addOption(
        activity: Activity,
        row: LinearLayout,
        dialog: Dialog,
        title: String,
        subtitle: String,
        tool: BuildToolsLauncher.Tool,
        narrow: Boolean,
        stacked: Boolean,
        veryShort: Boolean,
        iconSize: Int,
        rightMargin: Int
    ) {
        val density = activity.resources.displayMetrics.density
        fun dp(value: Float) = (value * density + .5f).toInt()
        val option = LinearLayout(activity).apply {
            orientation = if (stacked) LinearLayout.VERTICAL else LinearLayout.HORIZONTAL
            gravity = if (stacked) Gravity.CENTER else Gravity.CENTER_VERTICAL
            isClickable = true
            isFocusable = true
            setPadding(dp(if (veryShort) 6f else if (narrow) 9f else 13f),
                dp(if (veryShort || stacked) 4f else 7f), dp(8f), dp(if (veryShort || stacked) 4f else 7f))
            background = rounded("#FFFFFF", dp(12f), LINE)
            setOnClickListener {
                dialog.dismiss()
                BuildToolsLauncher.open(activity, tool)
            }
        }
        option.addView(ImageView(activity).apply {
            contentDescription = title
            setImageDrawable(BuildToolIconDrawable(tool, Color.parseColor(ACCENT_DARK)))
            scaleType = ImageView.ScaleType.CENTER_INSIDE
            setPadding(dp(7f), dp(7f), dp(7f), dp(7f))
            background = rounded(CREAM, dp(10f), "#F0E9E0")
        }, LinearLayout.LayoutParams(iconSize, iconSize).apply {
            if (stacked) bottomMargin = dp(3f) else marginEnd = dp(if (veryShort) 7f else 11f)
        })
        val textColumn = LinearLayout(activity).apply {
            orientation = LinearLayout.VERTICAL
            gravity = if (stacked) Gravity.CENTER_HORIZONTAL else Gravity.CENTER_VERTICAL
        }
        textColumn.addView(TextView(activity).apply {
            text = title
            textSize = if (veryShort) 11f else if (narrow) 12f else 14f
            typeface = Typeface.create("sans-serif-medium", Typeface.BOLD)
            setTextColor(Color.parseColor(INK))
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
        })
        if (!veryShort) textColumn.addView(TextView(activity).apply {
            text = subtitle
            textSize = if (narrow) 9f else 10f
            setTextColor(Color.parseColor(MUTED))
            maxLines = 1
            ellipsize = TextUtils.TruncateAt.END
        })
        option.addView(textColumn, if (stacked) {
            LinearLayout.LayoutParams(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.WRAP_CONTENT)
        } else {
            LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.WRAP_CONTENT, 1f)
        })
        if (!narrow && !veryShort) option.addView(TextView(activity).apply {
            text = "›"
            textSize = 18f
            setTextColor(Color.parseColor(MUTED))
            gravity = Gravity.CENTER
        }, LinearLayout.LayoutParams(dp(14f), ViewGroup.LayoutParams.WRAP_CONTENT))
        row.addView(option, LinearLayout.LayoutParams(0, ViewGroup.LayoutParams.MATCH_PARENT, 1f).apply {
            this.rightMargin = rightMargin
        })
    }

    private fun rounded(fill: String, radius: Int, stroke: String? = null) = GradientDrawable().apply {
        setColor(Color.parseColor(fill))
        cornerRadius = radius.toFloat()
        stroke?.let { setStroke(1, Color.parseColor(it)) }
    }
}

/** Canvas icons cannot depend on module resource IDs inside the hooked host. */
private class BuildToolIconDrawable(
    private val tool: BuildToolsLauncher.Tool,
    color: Int
) : Drawable() {
    private val paint = Paint(Paint.ANTI_ALIAS_FLAG).apply {
        this.color = color
        style = Paint.Style.STROKE
        strokeCap = Paint.Cap.ROUND
        strokeJoin = Paint.Join.ROUND
    }

    override fun draw(canvas: Canvas) {
        val size = minOf(bounds.width(), bounds.height()).toFloat()
        if (size <= 0f) return
        val left = bounds.left + (bounds.width() - size) / 2f
        val top = bounds.top + (bounds.height() - size) / 2f
        fun x(v: Float) = left + size * v
        fun y(v: Float) = top + size * v
        fun path(vararg points: Float) {
            val shape = Path().apply {
                moveTo(x(points[0]), y(points[1]))
                for (index in 2 until points.size step 2) lineTo(x(points[index]), y(points[index + 1]))
            }
            canvas.drawPath(shape, paint)
        }
        paint.style = Paint.Style.STROKE
        paint.strokeWidth = size * .075f
        when (tool) {
            BuildToolsLauncher.Tool.IMPORT -> {
                path(.5f, .12f, .5f, .68f)
                path(.27f, .45f, .5f, .68f, .73f, .45f)
                path(.17f, .76f, .17f, .88f, .83f, .88f, .83f, .76f)
            }
            BuildToolsLauncher.Tool.EXPORT -> {
                path(.5f, .69f, .5f, .13f)
                path(.27f, .36f, .5f, .13f, .73f, .36f)
                path(.17f, .76f, .17f, .88f, .83f, .88f, .83f, .76f)
            }
            BuildToolsLauncher.Tool.PIXEL_ART -> {
                canvas.drawRoundRect(RectF(x(.14f), y(.17f), x(.86f), y(.82f)), size * .07f, size * .07f, paint)
                canvas.drawCircle(x(.34f), y(.38f), size * .065f, paint)
                path(.19f, .69f, .39f, .51f, .52f, .63f, .66f, .43f, .82f, .65f)
            }
            BuildToolsLauncher.Tool.PROJECTION -> {
                path(.5f, .11f, .87f, .31f, .5f, .52f, .13f, .31f, .5f, .11f)
                path(.13f, .48f, .5f, .69f, .87f, .48f)
                path(.13f, .65f, .5f, .86f, .87f, .65f)
            }
        }
    }

    override fun setAlpha(alpha: Int) { paint.alpha = alpha; invalidateSelf() }
    override fun setColorFilter(colorFilter: ColorFilter?) { paint.colorFilter = colorFilter; invalidateSelf() }
    override fun getOpacity(): Int = PixelFormat.TRANSLUCENT
}
