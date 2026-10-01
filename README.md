# 🎤 Kirakara Show

面向卡拉 OK 场景的 C++ 实时字幕与媒体输出引擎。

Kirakara Show 为 [Kirakara-App](https://github.com/FMPeach/Kirakara-App) 提供歌词渲染、
音视频播放、实体双屏和局域网投屏能力。本仓库是引擎与宿主 DLL，不是带点歌界面的完整
KTV 应用。

## ✨ 功能特性

- **逐字走字动画** — 根据媒体时间实时计算每个字符的进度，支持平滑的填充与描边变色
- **双注音显示** — 支持主字、上方假名和下方罗马音，并保留各自的时序与排版方式
- **多角色歌词** — 支持独唱、合唱、角色分色和角色文字或图片标签
- **KTV 字幕效果** — 支持圆角描边、淡入淡出、倒计时指示灯和双行交替布局
- **标题与背景** — 支持歌曲标题文字块、纯色背景、背景图片和背景视频合成
- **实体双屏** — 可将完整视频与字幕画面直接输出到第二块显示器
- **无线投屏输出** — 离屏合成 H.264/AAC MPEG-TS，供上层应用连接投屏设备
- **音频控制** — 支持原唱/伴奏切换、音量和升降 Key，并在变调后维持原播放时长
- **时间驱动渲染** — 画面始终由当前媒体时间求值，不依赖累计帧数，也不会补画过期帧

## 使用方式

### 构建演示程序

当前完整实现面向 Windows 10/11 x64。开始前请准备 CMake 3.20 或更新版本、Ninja 或
Visual Studio C++ 工具链，以及 Windows SDK。

Clone 本仓库后运行：

```powershell
cd Kirakara-Show
.\build-release.bat
```

脚本会完成 Release 构建并运行测试。也可以手动执行：

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

### 预览歌词

```powershell
.\build\bin\kirakara_show_demo.exe .\tests\fixtures\dom-parity.lrc
```

演示程序支持载入 `.lrc` / `.krl`、调整常用字幕参数和导出当前画面。视频、标题与字幕组合可
通过 `kirakara_video_demo.exe` 验证。

### 接入应用

Windows 应用通过 `show_host.dll` 和
[`apps/show_host/show_host_api.h`](apps/show_host/show_host_api.h) 接入。上层应用负责点歌界面、
曲库、下载和输出模式选择，Show 负责媒体播放与最终画面。

```c
ShowHostHandle host = show_host_create();
show_host_load(host, video, lyrics, vocal, accompaniment);
show_host_play(host);

double position = show_host_get_position(host);

show_host_stop(host);
show_host_destroy(host);
```

完整生命周期、实体 Stage、Flutter 共享画面和 Cast 接口见
[宿主集成指南](docs/integration.md)。

## 项目结构

```text
Kirakara Show/
├── include/kirakara/show/   # 公共类型、解析、时间线与渲染接口
├── src/                     # 歌词核心与共享模型实现
├── src/win32/               # DirectWrite、Direct2D、D3D11 与 Stage
├── apps/win32_demo/         # 歌词预览程序
├── apps/video_demo/         # 视频、标题与字幕组合演示
├── apps/show_host/          # 应用宿主、音频、实体 Stage 与 Cast
├── config/                  # 演示程序默认配置
├── tests/                   # 单元测试、回归测试与 smoke
├── third_party/             # 随源码分发的第三方组件
├── tools/                   # 构建、测试与诊断辅助脚本
└── docs/                    # 架构、集成、格式与开发文档
```

## 歌词格式与配置

Show 使用 UTF-8 KRL / LRC，支持逐字时间戳、行内注音、双注音、Ruby 字典、角色标记和标题
配置。

```text
【@miku】{気|[02:40:50]き>[02:40:50]ki}{付|[02:40:72]づ>[02:40:72]zu}
【@miku+rin】{空|[02:41:26]か[02:41:43]ら>[02:41:26]ka[02:41:43]ra}
```

详细语法见 [KRL / LRC 格式](docs/formats.md)，字体、颜色、描边、布局和角色样式见
[JSON 配置参考](docs/config-reference.md)。

## 开发文档

- [工作原理](docs/architecture.md)
- [宿主集成指南](docs/integration.md)
- [KRL / LRC 格式](docs/formats.md)
- [JSON 配置参考](docs/config-reference.md)
- [开发指南](docs/development.md)
- [兼容性说明](docs/compatibility.md)

## ⚠️ 当前范围

- 完整播放、实体第二屏、Flutter 共享画面与 Cast 当前以 Windows 为实现平台
- 当前版本只能在 Windows 上完成完整构建和播放
- 本项目仍在开发中，正式接入前请同时阅读兼容性文档并运行仓库测试

## Todo

- [ ] 增加 Skia 等跨平台渲染后端
- [ ] 完成二维码与报幕 Overlay 的正式绘制
- [ ] 补全 Windows 11 新版本和更多 Intel / AMD / NVIDIA 设备验证
- [ ] 建立与 Kirakara Player 的自动画面对比测试

## LICENSE

Kirakara Show 自有源代码采用 [MIT License](LICENSE)。第三方组件仍适用其各自许可证，见
[第三方许可证说明](THIRD_PARTY_NOTICES.md)。
