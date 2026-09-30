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
    private val limitedStructureExtensions = setOf("schematic", "schem")
    private val pixelArtExtensions = setOf("png", "jpg", "jpeg")

    fun accepts(fileName: String, mode: BuildImportSourceMode, limited: Boolean = false): Boolean {
        val extension = fileName.substringAfterLast('.', "").lowercase(Locale.ROOT)
        if (extension.isEmpty()) return false
        return when (mode) {
            BuildImportSourceMode.STRUCTURE -> extension in if (limited) {
                limitedStructureExtensions
            } else {
                structureExtensions
            }
            BuildImportSourceMode.PIXEL_ART -> !limited && extension in pixelArtExtensions
        }
    }

    fun isJpeg(fileName: String): Boolean {
        val extension = fileName.substringAfterLast('.', "").lowercase(Locale.ROOT)
        return extension == "jpg" || extension == "jpeg"
    }
}
