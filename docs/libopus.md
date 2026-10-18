# Stage-Rebuild-Dev 增加 libopus 解码：范围与验证

验证日期：2026-10-18（Asia/Shanghai）。
上游分支：FMPeach/Kirakara-Show 的 Stage-Rebuild-Dev。
上游基础：`1705b1c1c758633ba5caca1e970f6cfc64dc724d`（按媒体源事件控制原生 Stage 缓冲）。

## 修改范围

只增加 Ogg/Opus 独立音轨解码，通过 libopus + libopusfile + libogg 静态链接到宿主。
支持本地中文路径、改名文件、HTTP/HTTPS、查询参数、重定向及无扩展名 HTTP 地址回退。
原唱/伴奏、音量、升降 Key、跳转、预加载和投屏继续消费原有 PCM 音频接口。

Media Foundation 音频解码函数保留原实现；非 Opus 的成功解码继续走该函数。
视频解码、原生 Stage、缓冲处理、时钟调度、H.264/AAC 编码、传输协议和对外 C ABI
均保留该上游分支的实现。与上游基础的差异不包含这些模块的源码修改。
不引入 FFmpeg，不添加 WebM/MKV/MP4 内嵌 Opus 的拆封装。
视频内嵌 Opus 继续依赖系统 Media Foundation 能力。

更新保留既有 libopus 提交历史，合入 Stage-Rebuild-Dev 基础，发布到
feiyangjun-1/Kirakara-Show 的 libopus；不修改两个仓库的 main。

## 体积

同一上游提交、MSVC 19.51、Windows SDK 10.0.26100、x64 Release、相同编译参数：

| 宿主 DLL | 字节 | KiB |
|---|---:|---:|
| 未修改 Stage-Rebuild-Dev | 760,832 | 743 |
| 新增 Opus | 957,440 | 935 |
| 增量 | 196,608 | 192 |

三个依赖均为私有静态库，不增加第三方运行时 DLL。
新增系统依赖为 Windows 自带的 WinHTTP；已有 MF 与系统 VC++ 运行库依赖保持原有部署方式。
BSD 许可声明位于 bin/third_party/opus，分发时需要一起保留。

## 验证结果

- Release 全部目标构建成功，完整 CTest 40/40 通过，包括宿主导出接口检查。
- 本地解码：单/双/六声道、中文改名文件、精确 pre-skip/尾部裁剪、链式流及损坏输入。
- HTTP：查询参数、重定向、短分块读取、无扩展名地址、404，以及原 MF HTTP WAV 回归。
- WAV/AAC/MP3 继续走原 MF 解码，原采样率与声道数保持不变。
- H.264 视频 + Opus 原唱/AAC 伴奏通过原生 Stage 综合 smoke，覆盖切轨、变调、
  跳转、暂停、切歌、预加载、纹理输出和 10 秒采样；采样平均约 59.58 fps。
- 相同输入通过 H.264/AAC MPEG-TS 投屏综合 smoke 和 5 秒连续采样：
  最大最近音视频包 PTS 差 10.656 ms，编码约 59.93 fps，编码丢帧及解码欠载均为 0。
  这是本机生成流的时间戳检查，不是电视端输出延迟或用户待提供 H.264 文件的同步测量。

本机原始日志保存在忽略目录 .deps/ctest.log、.deps/native-opus.log 和 .deps/cast-opus.log。

## 本机 MSVC 构建说明

上游在本机 SDK 下已有 MF_XVP_DISABLE_FRC 重复声明，以及少数演示/测试文件的
D3D10/D3D10.1 头文件顺序问题。对照验证只通过编译参数绕过，没有修改这些原版源码。
宿主编译参数包含 `/utf-8 /EHsc /DNOMINMAX /DMF_XVP_DISABLE_FRC=MF_XVP_DISABLE_FRC`；
已有演示/测试目标额外预包含 winsock2.h、d3d10_1.h。
导出检查工具需要 objdump 在 PATH 中；Python 仅用于可选回环 HTTP 测试。

依赖固定版本、SHA-256、离线准备方式及许可证见
[Opus 依赖说明](../third_party/opus/README.md)。
