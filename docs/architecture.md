# 工作原理

Kirakara Show 接收视频、歌词和音频文件，根据当前播放时间生成完整的 KTV 画面。它可以把
画面显示在本机第二块屏幕，也可以编码成局域网投屏使用的视频流。

## 从文件到画面

```text
视频 + KRL/LRC + 原唱/伴奏
              │
              v
        解析歌词和配置
              │
              v
      按播放时间计算字幕状态
              │
              v
       合成视频、标题和歌词
              │
       ┌──────┴──────┐
       v             v
   本机 Stage      Cast 视频流
```

字幕不是按输出帧数向前推进的。即使电脑短暂忙碌，下一帧也会直接显示当前播放时间对应的
歌词状态，不会从旧画面开始追赶。

## 主要组成

### 歌词核心

负责读取 KRL/LRC、处理逐字时间戳、注音、角色、段落、淡入淡出和指示灯。主要代码位于：

```text
include/kirakara/show/
src/
```

### Windows 渲染器

使用 DirectWrite、Direct2D 和 D3D11 绘制文字、描边、标题与背景。主要代码位于：

```text
include/kirakara/show/win32/
src/win32/
```

### ShowHost

`show_host.dll` 把媒体播放、音频控制、第二屏输出和 Cast 组合成一套供应用调用的接口。
Kirakara-App 使用的入口文件是：

```text
apps/show_host/show_host_api.h
```

## 两种输出方式

| 方式 | 用途 |
|---|---|
| Native Stage | 在本机窗口或实体第二屏显示视频与字幕 |
| Cast | 生成 H.264/AAC MPEG-TS 视频流，交给上层应用投放到电视或盒子 |

两种方式使用相同的歌词文件和显示配置。开发新效果时，应同时检查本机 Stage 和 Cast 的最终
画面是否一致。

## 播放时间

大多数歌曲使用音频作为播放时间基准。外链 DASH 音视频的长度可能不同，这时宿主可以选择
视频作为时间基准。具体选择方式见 [宿主集成指南](integration.md)。

标题使用歌曲正常播放时间，从 `0` 秒开始显示，不会在歌曲前额外增加一段时间。启用了标题但
没有填写文字也是合法配置，此时不会绘制标题。

## 继续阅读

- 想把 Show 接入应用：阅读 [宿主集成指南](integration.md)
- 想制作歌词：阅读 [KRL / LRC 格式](formats.md)
- 想调整画面：阅读 [JSON 配置参考](config-reference.md)
- 想编译或排查问题：阅读 [开发指南](development.md)
