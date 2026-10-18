# 原版增加 libopus 解码：范围与验证

验证日期：2026-10-18（Asia/Shanghai）。
上游基础：FMPeach/Kirakara-Show `d8bc3b26798ac3692d8a3dfb2897531d76f8b3c8`。

## 修改范围

只增加 Ogg/Opus 独立音轨解码，通过 libopus + libopusfile + libogg 静态链接到宿主。
支持本地中文路径、改名文件、HTTP/HTTPS、查询参数、重定向及无扩展名 HTTP 地址回退。
原唱/伴奏、音量、升降 Key、跳转、预加载和投屏继续消费原有 PCM 音频接口。

Media Foundation 音频解码函数保留原实现；非 Opus 的成功解码继续走该函数。
视频解码、原生 Stage、时钟调度、H.264/AAC 编码、传输协议和对外 C ABI 均未修改。
不引入 FFmpeg，不添加 WebM/MKV 内嵌 Opus 的拆封装。

## 体积

同一上游提交、MSVC 19.51、Windows SDK 10.0.26100、x64 Release、相同编译参数：

| 宿主 DLL | 字节 | KiB |
|---|---:|---:|
| 未修改原版 | 760,832 | 743 |
| 新增 Opus | 957,440 | 935 |
| 增量 | 196,608 | 192 |

三个依赖均为私有静态库，不增加第三方运行时 DLL。
新增系统依赖为 Windows 自带的 WinHTTP；已有 MF 与系统 VC++ 运行库依赖保持原有部署方式。
BSD 许可声明位于 `bin/third_party/opus`，分发时需要一起保留。

## 验证结果

- 新增解码测试：单/双/六声道、中文改名文件、精确 pre-skip/尾部裁剪、链式流及损坏输入。
- HTTP 测试：查询参数、重定向、短分块读取、无扩展名地址、404 处理，以及原 MF HTTP WAV 回归。
- WAV/AAC/MP3 继续走原 MF 解码，原采样率与声道数保持不变。
- 原有测试与新增测试共 40 项；宿主导出接口基线检查通过。
- 真实 H.264 视频 + Opus 原唱/AAC 伴奏通过原生 Stage 综合 smoke，覆盖切轨、变调、
  跳转、切歌、预加载、纹理输出和 5 秒采样。

### 原版已有的投屏综合检查限制

同一个未修改的投屏 smoke 使用相同视频、歌词和采样率音轨对照：
新增 Opus 版本的最大最近 H.264/AAC PTS 差为 72.678 ms，
未修改原版使用对应解码 WAV 时为 71.989 ms，均超过测试中的 50 ms 门限。
两版在这项检查中均失败；这不是电视端延迟测量，也不据此声称 Opus 版投屏综合测试通过。
按“其他不变”的范围，本分支保留上游的时钟、编码和投屏实现，未放宽检查门限。

## 本机 MSVC 构建说明

上游在本机 SDK 下已有 `MF_XVP_DISABLE_FRC` 重复声明，以及少数演示/测试文件的
D3D10/D3D10.1 头文件顺序问题。对照验证只通过编译参数绕过，没有修改这些原版源码。
宿主编译参数包含 `/utf-8 /EHsc /DNOMINMAX /DMF_XVP_DISABLE_FRC=MF_XVP_DISABLE_FRC`；
已有演示/测试目标额外预包含 `winsock2.h`、`d3d10_1.h`。
导出检查工具需要 `objdump` 在 PATH 中；Python 仅用于可选回环 HTTP 测试。

依赖固定版本、SHA-256、离线准备方式及许可证见
[Opus 依赖说明](../third_party/opus/README.md)。
