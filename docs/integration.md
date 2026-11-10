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

观众浮窗显示在实体第二屏与 Cast 上，主控预览保持视频、字幕和歌曲标题。二维码与待播列表
各有一个独立开关；滚动报幕字段暂时保留。

### 提供二维码图片

由应用的二维码库生成 PNG，保留完整留白，并传入图片尺寸、总模块数和内容版本：

```c
ShowHostPhoneOrderQrImage image = {0};
image.struct_size = sizeof(image);
image.abi_version = SHOW_HOST_AUDIENCE_HUD_ABI_VERSION;
image.revision = image_revision;
image.pixel_size = image_side;
image.modules = total_modules; /* 包括四周留白 */
image.png = png_bytes;
image.png_size = png_length;
show_host_set_phone_order_qr_image(host, &image);
```

图片为正方形，边长应是总模块数的整数倍，上限为 2048 像素、4 MiB。图片内容改变时递增
`revision`。Show 在调用返回前复制数据，按版本缓存解码结果，并等比缩放到卡片布局中，采用
邻近采样保留黑白模块边缘。
传入 `NULL` 可清除图片。

Kirakara-App 的手机点歌弹窗和大屏共用同一份成品图片。关闭弹窗后，共享资源仍然可用。

### 提供设置与队列

```c
ShowHostHudQueueItem next[] = {{L"下一首歌名", L"歌手"}};
ShowHostAudienceHudState hud = {0};
hud.struct_size = sizeof(hud);
hud.abi_version = SHOW_HOST_AUDIENCE_HUD_ABI_VERSION;
hud.flags = SHOW_HOST_HUD_QR_ENABLED | SHOW_HOST_HUD_QUEUE_ENABLED;
hud.revision = content_revision;
hud.network_name = L"家庭网络";
hud.queue = next;
hud.queue_count = 1;
hud.current_title = L"当前播放的歌名";
hud.current_artist = L"当前歌手";
show_host_set_audience_hud_state(host, &hud);
```

列表显示当前播放和后四首。当前播放带播放图标，后四首编号为 `01` 至 `04`。歌名、歌手和
网络文字各限 512 个 UTF-16 代码单元，超长文字在卡片中省略。当前播放为空时省略该行。
设置或歌曲信息改变时更新快照即可；暂停、音量和进度变化沿用同一份卡片。
入口复制数据后立即返回，Host 合并更新通知，再刷新观众画面。

`network_name` 使用系统网络名称：Wi-Fi 传 SSID，有线传对应连接的网络配置文件名。
Wi-Fi 同时设置 `SHOW_HOST_HUD_NETWORK_WIFI` 标志，卡片会显示对应图标与标签。

正常播放自然进入最后十秒时显示列表；新歌曲实际接管画面后保留三秒，再恢复二维码。
空队列有独立提示。重唱和 seek 保留当前展示阶段，暂停与缓冲会冻结进度条。关闭列表开关后，
二维码按自己的开关和网络条件显示。

末尾进度条只用于最后十秒；换歌后的三秒保持空轨道。新歌曲的列表淡入约 150ms，二维码与
列表互换时先淡出 150ms，再淡入 150ms。队列增删、顶歌和歌名更新沿用当前展示进度。
卡片文字与图片继续使用缓存，过渡期间调整透明度，静态显示后停止动画更新。

### 网络变化

`show_host_get_lan_state()` 读取局域网状态，`show_host_set_lan_state_callback()` 订阅 Windows
接口和地址变化。局域网判断关注可用网卡与地址；没有外网的路由器仍可用于点歌。

收到事件后，应用刷新手机点歌入口，再提交当前二维码图片。地址相同也应重新确认图片；
地址变化则生成新的版本。确认完成前，Show 隐藏旧二维码。所有可用接口消失时，整张二维码
卡片隐藏，待播列表仍可显示。首次状态未知时等待网络就绪，查询暂时失败时保留已知状态。

回调在 Host 控制线程调用，参数按值传递，适合投递到应用自己的事件队列。取消订阅时传入
`NULL`；函数返回后，Host 不再调用旧回调。Dart 可使用 `NativeCallable.listener` 接入这类
低频事件，销毁前先解除订阅，再关闭 callable。

Show 构建目录会生成与 DLL 同名的 `.LICENSES.txt`，汇总项目和内置组件的完整许可条款，供
单独分发 DLL 时使用。宿主也可将这些条款收录到自己的第三方许可证页；组件链接见
[第三方声明](../THIRD_PARTY_NOTICES.md)。

旧的 `show_host_set_stage_overlay_state()` 继续接收原有元数据。接入新卡片时使用上述独立入口。

## 10. 空闲背景

宿主负责携带图片，在创建 ShowHost 后传入本地绝对路径：

```c
show_host_set_idle_background_image(host, absolute_image_path);
```

路径使用 UTF-16。图片等比缩放到输出画布，比例不同时保留黑边。空闲背景会出现在统一
Stage 的主控预览、实体第二屏及 Cast 中。播放中的缓冲与无缝换歌继续保留原有画面。

接口返回 `true` 表示路径更新已接收，输出线程随后加载图片并建立缓存。图片或输出尺寸变化
时才重新处理；读取失败时保留普通底色。再次调用可刷新同一路径的文件，传入 `NULL` 或
空字符串可清除背景。

Kirakara-App 将图片打包到程序目录下的 `data/idle-background.png`，启动时把完整路径传给
Show。其他宿主可以采用自己的资源目录，DLL 中不携带图片。

## DLL 路径

Show 和 App 可以放在不同目录开发。构建 App 时，通过 App 仓库自己的配置指向已经生成的
`show_host.dll`，不需要把两个仓库放进固定的磁盘路径。

## 遇到问题

- 加载失败：先确认四个路径是否存在、文件是否下载完成。
- 第二屏位置错误：确认传入的是目标显示器物理像素。
- Flutter 预览没有画面：确认 App 与 Show 使用同一块 GPU，并检查 device 是否已传入。
- Cast 无法启动：查看返回值、端口和 [开发指南](development.md) 中的诊断方法。
