# Infinite-BUILDTOOL 项目功能总览

本文依据当前工作目录中的 Kotlin、Java、C++ 源码和构建配置整理。

## 项目定位

本项目是 Android 建筑工具模块。Xposed/LSPosed 入口为 `com.kong.buildtool.BuildToolsHookInit`，在游戏的 `com.mojang.minecraftpe.MainActivity` 中显示建筑工具入口。应用自身的 `MainActivity` 提供功能介绍页。

Android 模块位于 `app/`；应用 ID 为 `com.kong.buildtool`，最低 Android API 为 28，当前只构建 `arm64-v8a` 原生库。原生库名称为 `libweibosdkcore.so`。

## 入口与调用链

1. `BuildToolsHookInit.kt` 识别游戏 Activity，安装水印与原生运行时入口。
2. `BuildToolsLauncher.kt` 加载原生库，调用 `NativeCore.ensureBuildToolsHooks()`，然后打开工具面板或指定工具。
3. `NativeCore.java` 提供原生库加载状态、hook 初始化和容器界面关闭回调。
4. `native-lib.cpp` 在 `JNI_OnLoad` 注册上述 JNI 方法，等待 `libminecraftpe.so`，并初始化命令发送、回执、游戏 tick 和投影渲染所需的 hook。
5. `TpModule.cpp` 提供导入、投影、导出等操作的 JNI 接口；具体运行状态由 `build_import/` 下的模块管理。

建筑工具入口不要求账号登录或远程许可验证。应用清单没有 `INTERNET` 和 `ACCESS_NETWORK_STATE` 权限。命令操作由游戏或服务器检查权限；投影打印使用玩家已有的材料。

## 功能与源码

| 功能 | Android 界面 | 原生实现 |
| --- | --- | --- |
| 建筑导入 | `BuildImportUi.kt` | `tp/TpModule.cpp`、`build_import/BuildImportRuntime.cpp`、`build_import/BuildImportController.cpp` |
| 图片导入 | `PixelArtImportUi.kt` | `tp/TpModule.cpp`、`build_import/PixelArtParser.cpp`、`build_import/BuildImportRuntime.cpp` |
| 建筑投影与打印 | `BuildProjectionUi.kt`、`BuildProjectionPrinterOverlay.kt` | `build_import/BuildProjectionRuntime.cpp`、`build_import/BuildProjectionRenderer.cpp`、`build_import/ProjectionPrinterRuntime.cpp` |
| 建筑导出 | `BuildExportUi.kt` | `tp/TpModule.cpp`、`build_import/BuildExportRuntime.cpp`、`build_import/BuildExportCheckpoint.cpp` |

结构文件解析和写入分布在 `build_import/BdxParser.cpp`、`BdxWriter.cpp`、`SchematicParser.cpp`、`SchematicWriter.cpp`、`LitematicParser.cpp`、`McworldParser.cpp` 等文件。MIDI 和命令音乐解析由同目录中的 `MidiCommandMusicParser.cpp` 与 `CommandMusicParser.cpp` 实现。

游戏函数地址集中在 `tp/FunctionsAddress.cpp` 及相关 ABI 配置中。构建使用 Dobby、Brotli 和 OpenSSL。建筑功能通过 `tp/PythonUtils` 调用游戏内的 Python 接口；项目不单独编译 Python 解释器。修改 hook 或支持的游戏版本时，应核对当前源码中的地址和签名。

## `tp/` 目录职责

| 文件 | 建筑工具中的用途 |
| --- | --- |
| `TpModule.cpp`、`TpModule.h` | 建筑导入、图片导入、投影、打印、导出的 JNI 接口注册与参数处理 |
| `MinecraftUpdateHook.cpp`、`MinecraftUpdateHook.h` | 在游戏线程驱动建筑任务、查询世界与玩家上下文、处理导出区域间的移动 |
| `PythonUtils.h` | 调用游戏内 Python 的建筑操作桥接 |
| `BuildPacketReceiveHook.cpp`、`BuildPacketReceiveHook.h` | 处理建筑命令回执、容器同步、地图与告示牌数据 |
| `LoopbackPacketSenderCapture.cpp`、`LoopbackPacketSenderCapture.h` | 获取建筑操作使用的发送器并跟踪相关数据包 |
| `FunctionsAddress.cpp`、`FunctionsAddress.h`、`FunctionUtils.h` | 建筑功能依赖的游戏函数地址与类型化调用 |

导出时的区域移动使用服务器 `/tp` 命令，只移动当前玩家。任务通过实际坐标变化确认到达目标，并检查世界与维度是否仍匹配；服务器拒绝命令时不会伪造移动成功。

## 构建

在项目根目录运行：

```powershell
.\gradlew.bat :app:assembleDebug --offline
.\gradlew.bat :app:assembleRelease --offline
```

Debug APK 输出到 `app/build/outputs/apk/debug/`；Release 构建输出未签名 APK 到 `app/build/outputs/apk/release/`。

## 许可证

项目自有代码采用根目录 `LICENSE` 中的 MIT 许可。随项目包含的第三方代码和库遵循各自的许可证。
