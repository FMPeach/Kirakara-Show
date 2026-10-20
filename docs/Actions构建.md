# 在 GitHub 构建 Show DLL

main 提供独立的 DLL 构建，不创建 Release，也不修改源码或版本。`Actions-Test` 仍可用于测试工作流。

在 main 或 `Actions-Test` 修改 `config/actions-build.json` 的 `request`（每次换一个值）并推送，即可请求一次构建。成功后不需要将它改回。普通代码提交不会自动构建。

也可以在 Actions 的「Show DLL 构建测试」里点击 **Run workflow**，选择 main 或其他需要的分支。App 默认使用 main 最近一次成功构建的附件。

成功的附件名为 `kirakara-show-windows-x64`，包含 `libshow_host.dll` 和记录提交、构建编号、文件哈希的 `component-manifest.json`，保留 7 天。App 可以直接消费此附件，不需要 Show 发版。附件过期后需重新构建。

本地验证使用已有的 MinGW、CMake、Ninja：

```powershell
.\tools\build-actions-artifact.ps1
```

脚本编译 DLL，运行六组不需要显卡或媒体素材的单元测试，并检查现有 C ABI。不把云端构建通过当作真实投屏、显卡性能或实体双屏验收。
