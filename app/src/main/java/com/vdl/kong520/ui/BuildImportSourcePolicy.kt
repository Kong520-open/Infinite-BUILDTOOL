package com.vdl.kong520.ui

import java.util.Locale

internal enum class BuildImportSourceMode {
    STRUCTURE,
    PIXEL_ART
}

internal object BuildImportSourcePolicy {
    private val structureExtensions = setOf(
        "schematic",
        "schem",
        "litematic",
        "bdx",
        "mcworld",
        "mid",
        "midi",
        "infinity",
        "ibuild"
    )
    private val pixelArtExtensions = setOf("png", "jpg", "jpeg")

    fun accepts(fileName: String, mode: BuildImportSourceMode): Boolean {
        val extension = fileName.substringAfterLast('.', "").lowercase(Locale.ROOT)
        if (extension.isEmpty()) return false
        return when (mode) {
            BuildImportSourceMode.STRUCTURE -> extension in structureExtensions
            BuildImportSourceMode.PIXEL_ART -> extension in pixelArtExtensions
        }
    }

    fun isJpeg(fileName: String): Boolean {
        val extension = fileName.substringAfterLast('.', "").lowercase(Locale.ROOT)
        return extension == "jpg" || extension == "jpeg"
    }
}
