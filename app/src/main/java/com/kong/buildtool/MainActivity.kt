package com.kong.buildtool

import android.os.Bundle
import androidx.activity.ComponentActivity
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.BorderStroke
import androidx.compose.foundation.background
import androidx.compose.foundation.border
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.widthIn
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.unit.dp
import com.kong.buildtool.ui.theme.无尽纹理BUILDTOOLTheme

/** Informational launcher page. The usable UI is exposed only inside the XP host. */
class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge(
            statusBarStyle = SystemBarStyle.light(
                android.graphics.Color.TRANSPARENT,
                android.graphics.Color.TRANSPARENT
            ),
            navigationBarStyle = SystemBarStyle.light(
                android.graphics.Color.TRANSPARENT,
                android.graphics.Color.TRANSPARENT
            )
        )
        setContent {
            无尽纹理BUILDTOOLTheme(darkTheme = false, dynamicColor = false) {
                BuildingToolsIntroduction()
            }
        }
    }
}

@Composable
private fun BuildingToolsIntroduction() {
    val ink = Color(0xFF302D29)
    val muted = Color(0xFF77716A)
    val accent = Color(0xFFAA4E31)
    Surface(modifier = Modifier.fillMaxSize(), color = Color(0xFFF7F5F0)) {
        Column(
            modifier = Modifier
                .fillMaxSize()
                .verticalScroll(rememberScrollState())
                .padding(horizontal = 22.dp, vertical = 46.dp),
            horizontalAlignment = Alignment.CenterHorizontally,
            verticalArrangement = Arrangement.Center
        ) {
            Column(modifier = Modifier.widthIn(max = 480.dp)) {
                Text(
                    text = "BUILD TOOLS / GUIDE",
                    color = accent,
                    fontWeight = FontWeight.Bold,
                    style = MaterialTheme.typography.labelMedium
                )
                Spacer(Modifier.height(10.dp))
                Text(
                    text = "建筑工具",
                    style = MaterialTheme.typography.headlineLarge,
                    fontFamily = FontFamily.Serif,
                    fontWeight = FontWeight.SemiBold,
                    color = ink
                )
                Spacer(Modifier.height(10.dp))
                Text(
                    text = "为建筑创作准备的导入、投影与导出工具。",
                    color = muted,
                    style = MaterialTheme.typography.bodyLarge
                )
                Spacer(Modifier.height(24.dp))

                Surface(
                    color = Color(0xFFFAE9DF),
                    shape = RoundedCornerShape(14.dp),
                    border = BorderStroke(1.dp, Color(0xFFEBD2C4))
                ) {
                    Text(
                    text = "本页面仅作功能介绍。请通过 XP/LSPosed 加载到游戏，并点击游戏内 BUILD TOOLS 水印使用功能。",
                        modifier = Modifier.padding(15.dp),
                        color = Color(0xFF8A4D39),
                        style = MaterialTheme.typography.bodyMedium
                    )
                }
                Spacer(Modifier.height(22.dp))

                IntroductionCard(
                    index = "01",
                    title = "建筑导入",
                    description = "导入结构与 MIDI，支持位置、速度、断点恢复、校验修复和进度控制。"
                )
                Spacer(Modifier.height(9.dp))
                IntroductionCard(
                    index = "02",
                    title = "图片导入",
                    description = "将 PNG/JPG 图片转换为方块画，可设置目标宽度、放置坐标、速度和导入策略。"
                )
                Spacer(Modifier.height(9.dp))
                IntroductionCard(
                    index = "03",
                    title = "建筑投影",
                    description = "预览结构与图片，查看完整材料清单，并按需要开启投影打印机。"
                )
                Spacer(Modifier.height(9.dp))
                IntroductionCard(
                    index = "04",
                    title = "建筑导出",
                    description = "选取两个角点保存建筑，支持分区扫描、进度跟踪与断点续传。"
                )
            }
        }
    }
}

@Composable
private fun IntroductionCard(index: String, title: String, description: String) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(Color(0xFFFFFEFA), RoundedCornerShape(14.dp))
            .border(1.dp, Color(0xFFE7E0D7), RoundedCornerShape(14.dp))
            .padding(15.dp),
        verticalAlignment = Alignment.Top
    ) {
        Text(
            text = index,
            color = Color(0xFFAA4E31),
            fontWeight = FontWeight.Bold,
            style = MaterialTheme.typography.labelMedium,
            modifier = Modifier.padding(top = 2.dp, end = 14.dp)
        )
        Column {
        Text(
            text = title,
            color = Color(0xFF302D29),
            fontWeight = FontWeight.Bold,
            style = MaterialTheme.typography.titleMedium
        )
        Spacer(Modifier.height(4.dp))
        Text(
            text = description,
            color = Color(0xFF77716A),
            style = MaterialTheme.typography.bodyMedium
        )
        }
    }
}
