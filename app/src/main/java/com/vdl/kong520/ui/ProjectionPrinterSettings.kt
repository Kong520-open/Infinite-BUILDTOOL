package com.vdl.kong520.ui

import android.content.Context

/**
 * Persisted printer preferences shared by the projection setup dialog and the
 * in-game printer control.  Keeping this separate avoids tying the injected
 * overlay to a live projection-dialog instance.
 */
object ProjectionPrinterSettings {
    private const val PREFS = "build_projection_ui"
    private const val KEY_ENABLED = "projection_printer_enabled"
    private const val KEY_RATE = "projection_printer_rate"

    const val MIN_RATE = 1
    const val MAX_RATE = 20
    const val DEFAULT_RATE = 4

    data class Config(
        val enabled: Boolean,
        val blocksPerSecond: Int
    )

    fun read(context: Context): Config {
        val prefs = context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
        return Config(
            enabled = prefs.getBoolean(KEY_ENABLED, false),
            blocksPerSecond = prefs.getInt(KEY_RATE, DEFAULT_RATE).coerceIn(MIN_RATE, MAX_RATE)
        )
    }

    fun setEnabled(context: Context, enabled: Boolean) {
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putBoolean(KEY_ENABLED, enabled)
            .apply()
    }

    fun setRate(context: Context, blocksPerSecond: Int) {
        context.applicationContext.getSharedPreferences(PREFS, Context.MODE_PRIVATE)
            .edit()
            .putInt(KEY_RATE, blocksPerSecond.coerceIn(MIN_RATE, MAX_RATE))
            .apply()
    }
}
