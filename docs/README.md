# 文档

这里汇总随代码长期维护的使用说明，内容面向引擎使用者和参与开发的协作者。

| 文档 | 内容 |
|---|---|
| [工作原理](architecture.md) | Show 如何把媒体和歌词变成本机画面或 Cast 视频流 |
| [宿主集成](integration.md) | 在 Windows 应用中加载、播放、控制第二屏和启动 Cast |
| [KRL / LRC 格式](formats.md) | 时间戳、逐字时序、注音、角色和标题配置 |
| [配置参考](config-reference.md) | 字体、颜色、布局、淡入淡出、指示灯和角色配置 |
| [开发指南](development.md) | 准备环境、构建、测试和排查常见问题 |
| [兼容性说明](compatibility.md) | 当前支持的平台、建议测试项目和已知限制 |

第一次接入时建议先看 [宿主集成](integration.md)；参与开发时再阅读
[工作原理](architecture.md) 和 [开发指南](development.md)。

仓库中的 [example-ikari.krl](example-ikari.krl) 是较完整的格式样例。它用于观察真实歌词的
复杂度，不代替格式规范和自动测试。
