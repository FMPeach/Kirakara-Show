# 宿主集成指南

Windows 应用通过 `show_host.dll` 和
[`apps/show_host/show_host_api.h`](../apps/show_host/show_host_api.h) 使用 Kirakara Show。
应用负责界面、曲库、下载和播放队列，Show 负责播放媒体并生成最终画面。

## 1. 创建与销毁

```c
ShowHostHandle host = show_host_create();
if (!host) {
    /* 初始化失败 */
}

/* 使用 ShowHost */

show_host_destroy(host);
```

每个返回成功的 handle 最后都要调用 `show_host_destroy()`。

## 2. 加载歌曲

```c
bool loaded = show_host_load(
    host,
    L"video.mp4",
    L"lyrics.krl",
    L"vocal.wav",
    L"accompaniment.wav");
```

路径使用 Windows UTF-16 字符串。没有独立原唱时可以根据应用的资源规则传入对应音频文件。
下载、缓存和文件是否完整应由应用先处理好。

需要指定播放时间基准时使用 `show_host_load_with_clock()`：

| 模式 | 适合场景 |
|---|---|
| `SHOW_CLOCK_AUDIO_MASTER` | 普通曲库歌曲和以独立音频为主的播放 |
| `SHOW_CLOCK_VIDEO_MASTER` | 音视频分离、长度可能不同的外链 DASH 媒体 |

需要控制切歌画面时使用 `show_host_load_with_options()`：

| 模式 | 效果 |
|---|---|
| `SHOW_TRANSITION_HARD` | 立即切换到新歌曲 |
| `SHOW_TRANSITION_SEAMLESS` | 新画面准备好之前保留上一首的最后画面 |

## 3. 播放控制

```c
show_host_play(host);
show_host_pause(host);
show_host_seek(host, 0.0);  /* 重唱 */
show_host_stop(host);
```

状态和时间可以这样读取：

```c
int state = show_host_get_state(host);
bool buffering = show_host_is_buffering(host);
double position = show_host_get_position(host);
double duration = show_host_get_duration(host);
```

暂停与缓冲是不同状态。界面不可见或播放状态没有变化时，不需要持续高频查询。

## 4. 准备下一首

队列中已有下一首时，可以提前调用 `show_host_prepare_next()`：

```c
show_host_prepare_next(
    host,
    next_video,
    next_lyrics,
    next_vocal,
    next_accompaniment,
    SHOW_CLOCK_AUDIO_MASTER);
```

准备操作不会切换当前歌曲。真正换歌时仍需调用相应的 load 接口，并传入相同的文件路径。

## 5. 音频控制

```c
show_host_set_volume(host, 80);
show_host_set_key_semitones(host, 2);
show_host_set_audio_track(host, SHOW_AUDIO_TRACK_ACCOMPANIMENT);
```

- 音量使用百分比。
- Key 当前限制在 `-6` 到 `+6` 个半音。
- 音轨可以在原唱与伴奏之间切换。
- `show_host_set_audio_clock_offset()` 可用于修正已知的音频时间偏移。

## 6. 实体第二屏

先把目标显示器的物理坐标和尺寸传给 Show，再显示 Stage：

```c
show_host_set_stage_window_rect(host, x, y, width, height);
show_host_set_stage_visible(host, true);
```

Windows 开启缩放时，传入的是显示器物理像素，不是 Flutter 逻辑像素。关闭第二屏时调用：

```c
show_host_set_stage_visible(host, false);
```

## 7. Flutter 预览

Flutter 可以使用仓库提供的 Stage texture source，也可以使用定制 Engine 支持的 Stage Visual
接口。接入前先把 Flutter 使用的 D3D11 device 交给 Show：

```c
show_host_set_stage_d3d_device(host, native_d3d11_device);
```

随后根据 App 使用的 Flutter Engine 选择：

- `show_host_create_stage_texture_source()`：普通 external texture 方式；
- `show_host_get_stage_visual_api()`：Kirakara-App 定制 Engine 使用的 Stage Visual 方式。

回调只负责通知新画面已经可用，不要在回调中执行耗时操作。具体结构字段和同步要求以
`show_host_api.h` 中紧邻接口的注释为准。

## 8. Cast

```c
if (show_host_start_cast_stream(host, 0)) {
    uint16_t port = show_host_get_cast_stream_port(host);
    /* 把本机地址和端口交给投屏服务 */
}

show_host_stop_cast_stream(host);
```

端口传入 `0` 时会自动选择可用端口。Show 生成视频流；搜索设备、建立 DLNA 会话和显示连接
状态由上层应用负责。

## 9. Overlay

`show_host_set_stage_overlay_state()` 用于传入二维码、公告文字等大屏信息。当前版本已经提供状态
接口，完整的 QR 与报幕绘制仍在开发中。

## DLL 路径

Show 和 App 可以放在不同目录开发。构建 App 时，通过 App 仓库自己的配置指向已经生成的
`show_host.dll`，不需要把两个仓库放进固定的磁盘路径。

## 遇到问题

- 加载失败：先确认四个路径是否存在、文件是否下载完成。
- 第二屏位置错误：确认传入的是目标显示器物理像素。
- Flutter 预览没有画面：确认 App 与 Show 使用同一块 GPU，并检查 device 是否已传入。
- Cast 无法启动：查看返回值、端口和 [开发指南](development.md) 中的诊断方法。
