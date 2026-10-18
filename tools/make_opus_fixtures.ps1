param(
    [string]$FFmpeg = 'ffmpeg',
    [string]$OutputDirectory = (Join-Path $PSScriptRoot '../build/opus-fixtures')
)
$ErrorActionPreference = 'Stop'
New-Item -ItemType Directory -Force $OutputDirectory | Out-Null
function Encode([string[]]$Arguments) {
    & $FFmpeg -hide_banner -loglevel error -y @Arguments
    if ($LASTEXITCODE -ne 0) { throw 'Fixture encoding failed.' }
}
foreach ($entry in @(@('mono',1), @('stereo',2), @('surround',6))) {
    Encode @('-f','lavfi','-i','sine=frequency=440:sample_rate=48000','-t','2',
        '-ac',"$($entry[1])",'-c:a','libopus',(Join-Path $OutputDirectory "$($entry[0]).opus"))
}
Copy-Item (Join-Path $OutputDirectory 'stereo.opus') (Join-Path $OutputDirectory '中文音轨.bin') -Force
$mono = [IO.File]::ReadAllBytes((Join-Path $OutputDirectory 'mono.opus'))
$stereo = [IO.File]::ReadAllBytes((Join-Path $OutputDirectory 'stereo.opus'))
[IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'chained.ogg'), [byte[]]($mono + $stereo))
[IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'truncated.opus'), [byte[]]($stereo[0..40]))
# A CRC failure in a middle page must never publish partial decoded audio.
$damaged = [byte[]]$stereo.Clone()
$damaged[[int]($damaged.Length / 2)] = $damaged[[int]($damaged.Length / 2)] -bxor 1
[IO.File]::WriteAllBytes((Join-Path $OutputDirectory 'damaged.opus'), $damaged)
foreach ($codec in @('pcm_s16le','aac','libmp3lame')) {
    $extension = @{pcm_s16le='wav';aac='m4a';libmp3lame='mp3'}[$codec]
    Encode @('-f','lavfi','-i','sine=frequency=660:sample_rate=44100','-t','2',
        '-ac','1','-c:a',$codec,(Join-Path $OutputDirectory "original.$extension"))
}
Write-Host "Opus and Media Foundation regression fixtures: $OutputDirectory"
