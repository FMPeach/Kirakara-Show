# Third-Party Notices

Kirakara Show 自有代码使用仓库根目录中的 MIT License。以下随源码分发的第三方组件保留其
各自版权和许可证。

## Signalsmith Stretch

- Project: Signalsmith Stretch
- Upstream: <https://github.com/Signalsmith-Audio/signalsmith-stretch>
- Copyright: Copyright (c) 2022 Geraint Luff / Signalsmith Audio Ltd.
- License: MIT
- Local license: [third_party/signalsmith-stretch/LICENSE.txt](third_party/signalsmith-stretch/LICENSE.txt)

## Signalsmith Linear

- Project: Signalsmith Linear
- Upstream: <https://github.com/Signalsmith-Audio/linear>
- Copyright: Copyright (c) 2025 Signalsmith Audio
- License: MIT
- Local license: [third_party/signalsmith-linear/LICENSE.txt](third_party/signalsmith-linear/LICENSE.txt)

第三方许可证原文以各组件目录中的 `LICENSE.txt` 为准。

## Opus 音轨解码

- libopus 1.6.1：[Xiph.Org Opus](https://github.com/xiph/opus)，BSD-3-Clause。
- libopusfile 0.12：[Xiph.Org opusfile](https://github.com/xiph/opusfile)，BSD 类许可。
- libogg 1.3.6：[Xiph.Org Ogg](https://github.com/xiph/ogg)，BSD-3-Clause。
- 用途：仅新增 Ogg/Opus 音轨解码，静态链接到宿主；不引入 FFmpeg。
- 固定下载来源、SHA-256 和许可原文见 [third_party/opus](third_party/opus/README.md)。
- 二进制分发时须一并保留 `third_party/opus/*-COPYING`；自有源码仍为 MIT。
