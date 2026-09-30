package com.kong.buildtool

import android.app.Activity
import android.content.Context
import android.graphics.Color
import android.graphics.Typeface
import android.graphics.drawable.GradientDrawable
import android.view.Gravity
import android.view.HapticFeedbackConstants
import android.view.MotionEvent
import android.view.View
import android.view.ViewConfiguration
import android.view.ViewGroup
import android.widget.FrameLayout
import android.widget.TextView

/**
 * Local replacement for the source project's remotely-provided watermark.
 * It is also the switch that opens the Kotlin building-tools panel in-game.
 */
object BuildToolsWatermark {
    private const val VIEW_TAG = "build-tools-local-watermark"
    private const val PREFS = "build_tools_watermark_position"
    private const val KEY_X = "x"
    private const val KEY_Y = "y"

    fun install(activity: Activity) {
        if (activity.isFinishing || activity.isDestroyed) return
        val root = activity.window.decorView as? ViewGroup ?: return
        if (root.findViewWithTag<TextView>(VIEW_TAG) != null) return

        val density = activity.resources.displayMetrics.density
        fun dp(value: Int) = (value * density).toInt()
        val preferences = activity.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        val watermark = TextView(activity).apply {
            tag = VIEW_TAG
            text = "建筑工具  ·  打开"
            gravity = Gravity.CENTER
            setTextColor(Color.rgb(170, 78, 49))
            textSize = 11f
            typeface = Typeface.create("sans-serif-medium", Typeface.BOLD)
            setPadding(dp(12), dp(7), dp(12), dp(7))
            background = GradientDrawable().apply {
                setColor(Color.argb(239, 255, 254, 250))
                cornerRadius = dp(9).toFloat()
                setStroke(dp(1), Color.rgb(231, 224, 215))
            }
            setOnClickListener { BuildToolsLauncher.openPanel(activity) }
        }

        var positionX = 0
        var positionY = 0
        var positioned = false
        fun moveTo(x: Int, y: Int, save: Boolean) {
            val maxX = (root.width - watermark.width).coerceAtLeast(0)
            val maxY = (root.height - watermark.height).coerceAtLeast(0)
            positionX = x.coerceIn(0, maxX)
            positionY = y.coerceIn(0, maxY)
            val params = watermark.layoutParams as? FrameLayout.LayoutParams ?: return
            params.gravity = Gravity.TOP or Gravity.START
            params.setMargins(positionX, positionY, 0, 0)
            watermark.layoutParams = params
            positioned = true
            if (save) {
                preferences.edit().putInt(KEY_X, positionX).putInt(KEY_Y, positionY).apply()
            }
        }

        val longPressTimeout = ViewConfiguration.getLongPressTimeout().toLong()
        val touchSlop = ViewConfiguration.get(activity).scaledTouchSlop.toFloat()
        watermark.setOnTouchListener(object : View.OnTouchListener {
            private var tracking = false
            private var dragging = false
            private var cancelled = false
            private var downRawX = 0f
            private var downRawY = 0f
            private var downTime = 0L
            private var startX = 0
            private var startY = 0
            private val beginDrag = Runnable {
                if (tracking && !cancelled && watermark.isAttachedToWindow) {
                    dragging = true
                    watermark.performHapticFeedback(HapticFeedbackConstants.LONG_PRESS)
                }
            }

            override fun onTouch(view: View, event: MotionEvent): Boolean {
                when (event.actionMasked) {
                    MotionEvent.ACTION_DOWN -> {
                        tracking = true
                        dragging = false
                        cancelled = false
                        downRawX = event.rawX
                        downRawY = event.rawY
                        downTime = event.eventTime
                        // The initial layout is BOTTOM|END. Convert its actual
                        // location to TOP|START only when moving it, without a jump.
                        startX = if (positioned) positionX else watermark.left
                        startY = if (positioned) positionY else watermark.top
                        view.isPressed = true
                        view.parent?.requestDisallowInterceptTouchEvent(true)
                        view.postDelayed(beginDrag, longPressTimeout)
                    }

                    MotionEvent.ACTION_MOVE -> {
                        if (tracking) {
                            val dx = event.rawX - downRawX
                            val dy = event.rawY - downRawY
                            if (!dragging && dx * dx + dy * dy > touchSlop * touchSlop) {
                                // A swipe before long press is neither a drag nor a click.
                                cancelled = true
                                view.isPressed = false
                                view.removeCallbacks(beginDrag)
                            }
                            if (dragging) {
                                moveTo(startX + dx.toInt(), startY + dy.toInt(), save = false)
                            }
                        }
                    }

                    MotionEvent.ACTION_POINTER_DOWN, MotionEvent.ACTION_POINTER_UP -> {
                        cancelled = true
                        view.removeCallbacks(beginDrag)
                        if (dragging) moveTo(positionX, positionY, save = true)
                        dragging = false
                        view.isPressed = false
                    }

                    MotionEvent.ACTION_UP, MotionEvent.ACTION_CANCEL -> {
                        view.removeCallbacks(beginDrag)
                        view.isPressed = false
                        val wasDragging = dragging
                        if (wasDragging) moveTo(positionX, positionY, save = true)
                        val dx = event.rawX - downRawX
                        val dy = event.rawY - downRawY
                        val click = tracking && !wasDragging && !cancelled &&
                            event.actionMasked == MotionEvent.ACTION_UP &&
                            event.eventTime - downTime < longPressTimeout &&
                            dx * dx + dy * dy <= touchSlop * touchSlop
                        tracking = false
                        dragging = false
                        if (click) view.performClick()
                    }
                }
                return true
            }
        })
        root.addView(
            watermark,
            FrameLayout.LayoutParams(
                ViewGroup.LayoutParams.WRAP_CONTENT,
                ViewGroup.LayoutParams.WRAP_CONTENT,
                Gravity.BOTTOM or Gravity.END
            ).apply {
                setMargins(dp(12), dp(12), dp(18), dp(38))
            }
        )
        // Keep the original bottom-right anchor until the view has a real
        // size. A posted callback can run before its first layout and would
        // otherwise restore the default position as (0, 0).
        if (preferences.contains(KEY_X) && preferences.contains(KEY_Y)) {
            watermark.addOnLayoutChangeListener(object : View.OnLayoutChangeListener {
                override fun onLayoutChange(
                    view: View, left: Int, top: Int, right: Int, bottom: Int,
                    oldLeft: Int, oldTop: Int, oldRight: Int, oldBottom: Int
                ) {
                    if (root.width <= 0 || right <= left || bottom <= top) return
                    view.removeOnLayoutChangeListener(this)
                    moveTo(
                        preferences.getInt(KEY_X, left),
                        preferences.getInt(KEY_Y, top),
                        save = false
                    )
                }
            })
        }
        root.addOnLayoutChangeListener { _, left, top, right, bottom,
                                         oldLeft, oldTop, oldRight, oldBottom ->
            if (positioned && (right - left != oldRight - oldLeft ||
                    bottom - top != oldBottom - oldTop)) {
                moveTo(positionX, positionY, save = false)
            }
        }
    }
}
