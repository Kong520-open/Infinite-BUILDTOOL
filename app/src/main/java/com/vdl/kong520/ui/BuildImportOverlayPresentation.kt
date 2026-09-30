package com.vdl.kong520.ui

import java.util.Locale

internal const val IMPORT_OVERLAY_ENDED_STATE = 8

internal data class BuildImportOverlaySnapshot(
    val state: Int,
    val total: Long,
    val imported: Long
)

/** Keeps a finished task visible even if native cleanup immediately returns to Idle. */
internal class BuildImportOverlayStateTracker {
    private var observedTask = false
    private var latchedTerminalState: Int? = null
    private var lastUsableState = 0
    private var largestTotal = 0L
    private var largestImported = 0L

    fun update(observedState: Int, total: Long, imported: Long): BuildImportOverlaySnapshot {
        val safeTotal = total.coerceAtLeast(0L)
        val safeImported = imported.coerceAtLeast(0L)
        largestTotal = maxOf(largestTotal, safeTotal)
        largestImported = maxOf(largestImported, safeImported)

        if (latchedTerminalState == null) {
            when (observedState) {
                1, 2, 3, 4, 7 -> {
                    observedTask = true
                    lastUsableState = observedState
                }
                5, 6 -> {
                    observedTask = true
                    latchedTerminalState = observedState
                }
                0 -> if (observedTask) {
                    latchedTerminalState = if (
                        largestTotal > 0L && largestImported >= largestTotal
                    ) {
                        5
                    } else {
                        IMPORT_OVERLAY_ENDED_STATE
                    }
                }
            }
        }

        val displayState = latchedTerminalState
            ?: observedState.takeIf { it in 0..7 }
            ?: lastUsableState
        val displayImported = if (largestTotal > 0L) {
            largestImported.coerceAtMost(largestTotal)
        } else {
            largestImported
        }
        return BuildImportOverlaySnapshot(displayState, largestTotal, displayImported)
    }

    fun latchTerminal(state: Int) {
        if (state == 5 || state == 6) {
            observedTask = true
            latchedTerminalState = state
        }
    }
}

internal fun conciseImportOverlayStatus(state: Int, nativeStatus: String): String {
    val detail = nativeStatus.lowercase(Locale.ROOT)
    return when (state) {
        0 -> "准备就绪"
        1 -> when {
            "optimiz" in detail -> "正在优化导入计划"
            "finaliz" in detail -> "正在生成导入计划"
            else -> "正在读取建筑"
        }
        2 -> when {
            "ticking-area cleanup" in detail -> "等待服务器确认导入准备"
            "container" in detail -> "正在恢复物品NBT"
            "sign" in detail -> "正在恢复告示牌"
            "command-block" in detail || "command block" in detail -> "正在恢复命令方块"
            "entit" in detail -> "正在恢复实体"
            "teleport" in detail || "reposition" in detail ||
                "waiting above" in detail || "loading" in detail -> "正在加载目标区域"
            "queue" in detail -> "等待服务器处理"
            "clear" in detail -> "正在清理目标区域"
            else -> "正在执行方块指令（相邻同类区域使用 /fill）"
        }
        3 -> "任务已暂停"
        4 -> "等待返回原世界"
        5 -> "任务已完成"
        6 -> "任务失败，请查看导入界面"
        7 -> when {
            "map" in detail -> "正在制作地图"
            "container" in detail -> "正在恢复物品NBT"
            "sign" in detail -> "正在恢复告示牌"
            "command-block" in detail || "command block" in detail -> "正在恢复命令方块"
            "entit" in detail -> "正在恢复实体"
            "teleport" in detail || "reposition" in detail ||
                "waiting above" in detail || "loading" in detail -> "正在加载目标区域"
            "repair" in detail -> "正在修复差异"
            else -> "正在校验导入结果"
        }
        IMPORT_OVERLAY_ENDED_STATE -> "任务已结束"
        else -> "正在同步状态"
    }
}
