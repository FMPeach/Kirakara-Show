param(
    [string]$FFmpeg = 'ffmpeg',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../build-ninja/codec-fixtures')
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
function Encode([string[]]$Arguments) {
    & $FFmpeg -hide_banner -loglevel error -y @Arguments
    if ($LASTEXITCODE -ne 0) { throw 'Codec fixture encoding failed.' }
}
# Encoders belong to the test tool, not the shipped decoder SDK.
foreach ($depth in @(8, 10)) {
    $pixelFormat = if ($depth -eq 10) { 'yuv420p10le' } else { 'yuv420p' }
    Encode @('-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=30', '-t', '2',
        '-an', '-c:v', 'libaom-av1', '-cpu-used', '8', '-crf', '40',
        '-pix_fmt', $pixelFormat, (Join-Path $OutputDirectory "av1-$depth.mkv"))
    Encode @('-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=30', '-t', '2',
        '-an', '-c:v', 'libx265', '-preset', 'ultrafast', '-x265-params',
        'log-level=error:keyint=15:bframes=3:pools=1', '-pix_fmt', $pixelFormat,
        '-tag:v', 'hvc1', (Join-Path $OutputDirectory "hevc-$depth.mp4"))
    Encode @('-f', 'lavfi', '-i', 'testsrc2=size=640x360:rate=30', '-t', '2',
        '-an', '-c:v', 'libx264', '-preset', 'ultrafast', '-g', '15', '-bf', '3',
        '-pix_fmt', $pixelFormat, (Join-Path $OutputDirectory "h264-$depth.mp4"))
}
foreach ($channels in @(1, 2, 6)) {
    Encode @('-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
        '-t', '2', '-ac', "$channels", '-c:a', 'libopus',
        (Join-Path $OutputDirectory "opus-$channels.ogg"))
}
Encode @('-i', (Join-Path $OutputDirectory 'av1-8.mkv'),
    '-i', (Join-Path $OutputDirectory 'opus-2.ogg'), '-c', 'copy',
    (Join-Path $OutputDirectory '视频与音频.webm'))
Write-Host "Codec fixtures: $OutputDirectory"
Encode @('-i', (Join-Path $OutputDirectory 'hevc-8.mp4'),
    '-i', (Join-Path $OutputDirectory 'opus-2.ogg'), '-c', 'copy',
    (Join-Path $OutputDirectory 'hevc-opus.mkv'))
Encode @('-i', (Join-Path $OutputDirectory 'h264-8.mp4'),
    '-f', 'lavfi', '-i', 'sine=frequency=440:sample_rate=48000',
    '-t', '2', '-ac', '2', '-c:v', 'copy', '-c:a', 'aac',
    (Join-Path $OutputDirectory 'h264-aac.mp4'))
