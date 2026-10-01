# 开发指南

## 准备环境

Windows 开发需要：

- Windows 10 或 Windows 11 x64
- CMake 3.20 或更新版本
- Ninja，或安装了“使用 C++ 的桌面开发”的 Visual Studio
- Windows SDK

部分媒体 smoke 会调用 FFmpeg；只有运行这些测试时才需要把 `ffmpeg` 加入 `PATH`。

## 构建

最简单的方式是：

```powershell
.\build-release.bat
```

也可以手动构建：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
```

主要产物位于 `build/bin/`：

| 文件 | 用途 |
|---|---|
| `kirakara_show_demo.exe` | 歌词预览和参数调试 |
| `kirakara_video_demo.exe` | 视频、标题和字幕组合测试 |
| `show_host.dll` | 提供给 Kirakara-App 等宿主使用 |

## 运行测试

```powershell
ctest --test-dir build --output-on-failure
```

只想构建库和演示程序时，可以在首次配置时加入：

```powershell
-DKIRAKARA_BUILD_TESTS=OFF
```

## 生成 Smoke 媒体

仓库不附带测试视频。安装 FFmpeg 后可以生成两组短视频和音频：

```powershell
.\tools\make_smoke_fixtures.ps1
```

文件默认写到：

```text
%TEMP%\kira_fixtures
```

脚本完成后会显示 Native Stage 和 Cast smoke 的完整运行命令。

## 排查 Native Stage

需要观察实体第二屏或 Flutter 预览的运行情况时：

```powershell
$env:KIRAKARA_NATIVE_STAGE_DIAGNOSTICS = '1'
```

从同一个终端启动应用或 smoke。Show 会定期把摘要输出到终端和 Windows 调试输出，不会自动
创建日志文件。测试结束后关闭终端，或删除该环境变量即可：

```powershell
Remove-Item Env:KIRAKARA_NATIVE_STAGE_DIAGNOSTICS
```

分析卡顿时建议同时记录：

- Show 输出的帧率与缓冲状态；
- 任务管理器中的 3D、Video Decode 和 Video Processing；
- GPU 频率、温度与是否处于电池模式；
- 是否同时打开 Flutter 预览或实体第二屏。

## 排查 Cast

一般问题先使用 `show_host_get_cast_pipeline_stats()` 查看累计统计。需要定位短暂卡顿时，可以在
启动 Cast 前临时开启详细轨迹：

```c
show_host_set_cast_pipeline_trace_enabled(host, true);
show_host_start_cast_stream(host, 0);
```

轨迹保存在内存中，不会自动写入磁盘。完成排查后关闭即可，正式运行不建议长期启用。

## 修改公共接口

修改 `show_host_api.h` 后，需要运行完整测试并确认导出列表没有意外变化。需要更新导出基线时：

```powershell
.\tools\show_host_abi.ps1 `
  -Dll .\build\bin\show_host.dll `
  -Baseline .\tests\abi\show_host_exports.txt `
  -Update
```

只有明确新增或删除接口时才更新基线。

## 修改 Cast Shader

修改 `apps/show_host/cast/shaders/nv12_overlay.hlsl` 后，按照同目录 README 中的命令重新生成
Shader 字节码，并运行相关测试。

## 提交前检查

```powershell
cmake --build build
ctest --test-dir build --output-on-failure
git diff --check
```

涉及画面效果时，还应分别查看歌词 Demo、视频 Demo，以及实际的 Stage 或 Cast 输出。自动测试
通过不代表肉眼效果一定正确。
