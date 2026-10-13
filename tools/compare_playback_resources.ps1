param(
    [string]$BaselineRuntime = (Join-Path $PSScriptRoot '../../backups/2026-10-13-before-all-ffmpeg/runtime'),
    [string]$FfmpegRuntime = (Join-Path $PSScriptRoot '../build-ffmpeg/bin'),
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../artifacts/ffmpeg-comparison'),
    [int]$Rounds = 3,
    [int]$SampleSeconds = 12,
    [string[]]$Datasets = @('h264-1080p', 'hevc-1080p10', 'h264-embedded'),
    [string]$FfmpegVariant = 'ffmpeg'
)
$ErrorActionPreference = 'Stop'
$baseline = [IO.Path]::GetFullPath($BaselineRuntime)
$updated = [IO.Path]::GetFullPath($FfmpegRuntime)
$output = [IO.Path]::GetFullPath($OutputDirectory)
$probeName = 'kirakara_show_playback_resource_probe.exe'
# Identical probe executable; DLL selection is determined by executable folder.
Copy-Item -LiteralPath (Join-Path $updated $probeName) -Destination $baseline -Force
New-Item -ItemType Directory -Force (Join-Path $output 'runs') | Out-Null
$all = [Collections.Generic.List[object]]::new()
if (Test-Path -LiteralPath (Join-Path $output 'results.json')) {
    foreach ($previous in (Get-Content -LiteralPath (Join-Path $output 'results.json') -Raw | ConvertFrom-Json)) {
        if ($previous.dataset -notin $Datasets) { $all.Add($previous) }
    }
}
foreach ($dataset in $Datasets) {
    $extension = if ($dataset.StartsWith('h264')) { '.mp4' } else { '.mkv' }
    $video = Join-Path $output "media/$dataset$extension"
    $audio = if ($dataset -eq 'h264-embedded') { '-' } else { Join-Path $output 'media/audio.wav' }
    for ($round = 1; $round -le $Rounds; $round++) {
        $order = if ($round % 2) { @('baseline', $FfmpegVariant) } else { @($FfmpegVariant, 'baseline') }
        foreach ($variant in $order) {
            $runtime = if ($variant -eq 'baseline') { $baseline } else { $updated }
            $resultFile = Join-Path $output "runs/$dataset-$variant-$round.json"
            $log = Join-Path $output "runs/$dataset-$variant-$round.log"
            Write-Host "Sampling $dataset / $variant / $round of $Rounds"
            & (Join-Path $runtime $probeName) $video $audio $resultFile $SampleSeconds *> $log
            if ($LASTEXITCODE -ne 0) { throw "Playback probe failed: $log" }
            $result = Get-Content -LiteralPath $resultFile -Raw | ConvertFrom-Json
            if (!$result.playing_at_end -or !$result.texture_valid -or $result.stage_fps -lt 45) {
                throw "Playback failed validation: $resultFile"
            }
            $result | Add-Member dataset $dataset
            $result | Add-Member variant $variant
            $result | Add-Member round $round
            $result | Add-Member probe_exe_sha256 (Get-FileHash -LiteralPath (Join-Path $runtime $probeName) -Algorithm SHA256).Hash
            $all.Add($result)
            $all | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'results.json') -Encoding utf8
            Write-Host ("CPU {0:N2}% | RAM {1:N1} MiB | GPU decode {2:N2}% | Stage {3:N1} fps" -f
                $result.cpu_percent_machine, $result.working_set_mb, $result.gpu_video_decode_percent, $result.stage_fps)
        }
    }
}
$provenance = [ordered]@{
    baselineDll = (Get-FileHash -LiteralPath (Join-Path $baseline 'show_host.dll') -Algorithm SHA256).Hash
    ffmpegDll = (Get-FileHash -LiteralPath (Join-Path $updated 'show_host.dll') -Algorithm SHA256).Hash
    probeExe = (Get-FileHash -LiteralPath (Join-Path $updated $probeName) -Algorithm SHA256).Hash
    rounds = $Rounds
    measuredSecondsPerRun = $SampleSeconds
    warmupSecondsPerRun = 4
    cpu = @(Get-CimInstance Win32_Processor | Select-Object Name, NumberOfLogicalProcessors)
    gpu = @(Get-CimInstance Win32_VideoController | Select-Object Name, DriverVersion)
    os = @(Get-CimInstance Win32_OperatingSystem | Select-Object Caption, Version)
    media = @(Get-ChildItem -LiteralPath (Join-Path $output 'media') -File | Get-FileHash -Algorithm SHA256 | Select-Object Path, Hash)
}
$provenance | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $output 'provenance.json') -Encoding utf8
Write-Host "Resource comparison saved: $output/results.json"
