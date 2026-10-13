param([string]$Directory = (Join-Path $PSScriptRoot '../artifacts/ffmpeg-comparison'))
$ErrorActionPreference = 'Stop'
$directory = [IO.Path]::GetFullPath($Directory)
$final = @(Get-Content -LiteralPath (Join-Path $directory 'results.json') -Raw | ConvertFrom-Json)
$pure = @(Get-Content -LiteralPath (Join-Path $directory 'pure-switch/results.json') -Raw | ConvertFrom-Json)
$metadata = Get-Content -LiteralPath (Join-Path $directory 'provenance.json') -Raw | ConvertFrom-Json
function Summarize($rows) {
    @($rows | Group-Object dataset,variant | ForEach-Object {
        if ($_.Count -ne 3) { throw "Incomplete sampling: $($_.Name) has $($_.Count) runs" }
        $group = $_.Group
        $result = [ordered]@{ dataset=$group[0].dataset; variant=$group[0].variant; runs=$group.Count }
        foreach ($metric in @('cpu_percent_machine','cpu_core_equivalents','working_set_mb','private_bytes_mb',
            'gpu_video_decode_percent','gpu_3d_percent','gpu_dedicated_mb','gpu_shared_mb','stage_fps','preview_fps','load_ms')) {
            $values = @($group | ForEach-Object { $_.$metric })
            $measure = $values | Measure-Object -Average -Minimum -Maximum
            $result[$metric] = [ordered]@{ mean=$measure.Average; min=$measure.Minimum; max=$measure.Maximum }
        }
        $result['dropped_frames_total'] = ($group.stage_dropped_frames | Measure-Object -Sum).Sum
        [pscustomobject]$result
    })
}
$summary = [ordered]@{ final=@(Summarize $final); pureSwitch=@(Summarize $pure) }
$summary | ConvertTo-Json -Depth 8 | Set-Content -LiteralPath (Join-Path $directory 'summary.json') -Encoding utf8
function Value($record,[string]$metric,[int]$digits=2) {
    $record.$metric.mean.ToString("F$digits", [Globalization.CultureInfo]::InvariantCulture)
}
function Table($records) {
    '| 场景 | 版本 | CPU（整机%） | 工作集 MiB | 提交内存 MiB | GPU 解码% | GPU 3D% | Stage fps |'
    '|---|---|---:|---:|---:|---:|---:|---:|'
    foreach ($record in $records) {
        $label = switch ($record.dataset) {
            'h264-1080p' { 'H.264 + 外部音轨' }
            'hevc-1080p10' { 'H.265 10-bit + 外部音轨' }
            'h264-embedded' { 'H.264 + 内嵌 AAC' }
        }
        $variant = switch ($record.variant) {
            'baseline' { '备份原版' }
            'ffmpeg' { '纯 FFmpeg 切换' }
            'ffmpeg-optimized' { 'FFmpeg + PCM 等待修复' }
        }
        "| $label | $variant | $(Value $record 'cpu_percent_machine') | $(Value $record 'working_set_mb' 1) | $(Value $record 'private_bytes_mb' 1) | $(Value $record 'gpu_video_decode_percent') | $(Value $record 'gpu_3d_percent') | $(Value $record 'stage_fps' 1) |"
    }
}
$lines = [Collections.Generic.List[string]]::new()
@(
    '# 全 FFmpeg 解码切换与占用实测', '',
    '日期：2026-10-13（Asia/Seoul）。', '',
    '已先备份源码与原版 DLL，然后把 show_host.dll 的音视频解码及源能力检测统一改为 FFmpeg。',
    '视频优先 D3D11VA，设备/格式不支持时回退软件；本机视频时钟独立于 Media Engine。',
    'DirectSound、变调、PCM 音轨切换和 Cast 输出编码继续使用原有流程。独立演示程序不在本次宿主切换范围内。', '',
    '## 最终版本（三次均值）', ''
) | ForEach-Object { $lines.Add($_) }
Table $summary.final | ForEach-Object { $lines.Add($_) }
@('', '## 纯切换实验（三次均值，未修复 PCM 忙循环）', '') | ForEach-Object { $lines.Add($_) }
Table $summary.pureSwitch | ForEach-Object { $lines.Add($_) }
@(
    '', '## 结果解释', '',
    '- 纯切换的 H.264 GPU 解码负载降低，外部音轨模式 CPU 基本相近；工作集减少，但提交内存增加。不能概括为所有内存占用都降低。',
    '- 纯切换使内嵌 AAC 也进入已有 PCM 播放器，从而暴露了原有音频线程忙循环：播放期间 wait_for 的 playing 条件一直为真，约占一个逻辑 CPU。',
    '- 最终版本将等待移到每次填充之后，以 8 ms 间隔补充音频；停止/暂停可结束等待，首次填充立即执行。CPU 的大幅下降主要来自这个修复，不能全部归功于 FFmpeg 解码器。',
    '- H.265 在备份版本里已经使用 FFmpeg；其 GPU/内存变化主要落在重复测量波动范围内。',
    '- 所有样本输出都是 1920×1080，生产/预览约 60 fps，纹理有效、播放时钟持续推进；本次不含 Flutter UI、电视或完整桌面 App 的开销。',
    '- 音频仍整轨预解码到内存。长视频内嵌音轨的内存和加载时间会随时长增加，60 秒片段结果不能直接外推到整集或整部电影。', '',
    '## 设备与方法', '',
    "CPU：$($metadata.cpu[0].Name.Trim())，$($metadata.cpu[0].NumberOfLogicalProcessors) 个逻辑处理器。",
    "GPU：$(($metadata.gpu.Name) -join ' / ')。",
    "系统：$($metadata.os[0].Caption) $($metadata.os[0].Version)。", '',
    '- 使用本机真实视频的片段，选取约第 90 秒开始的内容，直接复制原始码流，不重新编码。',
    '- H.264：1080p、8-bit、约 23.976 fps；H.265：1080p、10-bit、约 23.810 fps。',
    '- 外部音轨组共用同一份 48 kHz 双声道浮点 PCM 音轨；内嵌音轨组使用原始 AAC。音量为零，仍执行音频解码与处理。',
    '- 同一探针程序放在两个 DLL 目录，各组交替原版/新版，运行三轮；预热 4 秒，随后初始化 GPU 计数器并采样 12 秒。',
    '- CPU 用进程 user+kernel 时间除以实际采样时长和 32 个逻辑处理器，口径与整机百分比一致。',
    '- 内存分别记录 WorkingSet 和 PrivateUsage；GPU 采用 PDH 的本进程 VideoDecode/3D 引擎计数器，并非整块 GPU 的总占用或功耗。',
    '- 每组的范围、原始样本、DLL/媒体/探针 SHA-256 和设备版本分别保存在 summary.json、runs、provenance.json。纯切换记录另保存在 pure-switch，两个探针版本差异仅为支持显式选择内嵌音轨；同一轮两版使用完全相同的探针。', '',
    '## 验证与回退', '',
    '- 最终构建与回归：39/39 通过，包括 H.264/AV1/H.265 硬解、Hi10P 软件回退、8/10 位输出、Opus、内嵌 AAC、暂停/跳转/自然结束、变调、投屏和 C ABI。',
    '- 原版备份：[备份说明](../../../backups/2026-10-13-before-all-ffmpeg/README.md)，源码归档含 517 个文件，runtime 保存原 DLL 及依赖。',
    '- 未修复忙循环的全 FFmpeg 产物另保存在 unoptimized-runtime，便于复核。',
    '- 最终产物：[show_host.dll](../../build-ffmpeg/bin/show_host.dll)，需同目录 FFmpeg DLL 与许可材料。',
    '- 自有代码保持 MIT；FFmpeg 仍为 LGPL 动态库，公开分发要求见 [依赖说明](../../third_party/ffmpeg/README.md)。'
) | ForEach-Object { $lines.Add($_) }
$lines | Set-Content -LiteralPath (Join-Path $directory 'report.md') -Encoding utf8
Write-Host "Report: $directory/report.md"
