# Opus 音轨解码依赖

此分支基于 FMPeach/Kirakara-Show 原版，仅增加 Ogg/Opus 独立音轨解码。
不引入 FFmpeg，不替换视频解码、Media Foundation 的其他音频格式或 H.264/AAC 编码。

- libopus 1.6.1：Opus 解码。
- libopusfile 0.12：Ogg Opus 封装、pre-skip、尾部裁剪、增益、多声道和链式流处理。
- libogg 1.3.6：Ogg 页面和包读取。

三者均静态链接到 `show_host.dll`，不增加需要部署的第三方 DLL。
自有代码继续使用 MIT；各库的 BSD 类许可证原文分别保存在本目录的 `*-COPYING`。
宿主构建会把此目录复制到 `bin/third_party/opus`，分发时保留这些声明。

## 构建

首次 CMake 配置按 `dependency.json` 中固定 URL 下载源码并检查 SHA-256。
也可以将经过相同校验的三个原始 ZIP 放到仓库 `.deps/sources/` 下，按版本锁中的
`archive` 字段命名，供离线配置使用。关闭依赖自己的测试、命令行程序、共享库及 DRED/OSCE。
编解码以外的播放器行为不变；未使用的 Opus 编码入口不会接入宿主。

## 支持范围

- 本地 `.opus` / `.ogg` / `.oga`，按 Ogg/Opus 内容识别，因此也支持改名文件和中文路径。
- HTTP/HTTPS Opus 音轨、查询参数及常规 HTTP 重定向；无扩展名 URL 在原 MF 解码失败后回退。
- 输出 48 kHz 立体声浮点 PCM，继续使用原有原唱/伴奏、变调、音量、跳转和投屏音频管线。
- 本次不增加 WebM/MKV 中内嵌 Opus 的拆封装，也不改变原版的视频容器支持。

网络读取使用 Windows WinHTTP 的系统代理和证书验证，无 OpenSSL 或 opusurl 依赖。
本地 Unicode 路径使用宽字符文件接口。解码错误不会把已解出的部分音频交给播放器。

## 验证

使用测试工具生成样片（测试用 FFmpeg 不参与运行库）：

```powershell
.\tools\make_opus_fixtures.ps1 -FFmpeg C:\ffmpeg\ffmpeg.exe
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

可以通过 `KIRAKARA_OPUS_FIXTURE_DIR` 指定样片目录。有 Python 时，还加入回环 HTTP 测试。
新增测试验证单/双/六声道、中文改名文件、pre-skip/尾部裁剪、链式流、损坏输入、
HTTP 状态/重定向/无扩展名，以及 WAV/AAC/MP3 仍经过原版 MF 解码并保持原输出格式。
