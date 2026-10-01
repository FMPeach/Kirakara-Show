<#
.SYNOPSIS
    Generates the synthetic media fixtures used by the unified Stage smokes.

.DESCRIPTION
    kirakara_show_unified_native_smoke and kirakara_show_unified_cast_smoke
    both need two complete songs (video + krl + vocal + accompaniment). Real
    media cannot be committed, so these are synthesised with ffmpeg: a 720p60
    test pattern plus separate sine tracks. That exercises the same paths the
    real content does (MediaEngine video source, processed PCM audio backend,
    lyric timeline, song switching) while keeping every fixture small and
    reproducible.

    The fixtures live outside the repository so they never reach git.

.PARAMETER OutputDirectory
    Where the fixtures are written. Defaults to $env:TEMP\kira_fixtures.

.PARAMETER Ffmpeg
    ffmpeg executable. Defaults to ffmpeg found on PATH.

.PARAMETER Seconds
    Fixture duration. Defaults to 20 seconds, which is long enough for the
    smoke's soak window plus a song switch.

.EXAMPLE
    tools/make_smoke_fixtures.ps1
    build/bin/kirakara_show_unified_native_smoke.exe `
        "$env:TEMP\kira_fixtures\v1.mp4" "$env:TEMP\kira_fixtures\l1.krl" `
        "$env:TEMP\kira_fixtures\vocal1.wav" "$env:TEMP\kira_fixtures\inst1.wav" `
        "$env:TEMP\kira_fixtures\v2.mp4" "$env:TEMP\kira_fixtures\l2.krl" `
        "$env:TEMP\kira_fixtures\vocal2.wav" "$env:TEMP\kira_fixtures\inst2.wav" `
        --texture-only --soak-seconds=8 --stage-diagnostics
#>
[CmdletBinding()]
param(
    [string]$OutputDirectory = (Join-Path $env:TEMP 'kira_fixtures'),
    [string]$Ffmpeg = 'ffmpeg',
    [int]$Seconds = 20
)

$ErrorActionPreference = 'Stop'

if (-not (Get-Command $Ffmpeg -ErrorAction SilentlyContinue)) {
    Write-Error "make_smoke_fixtures: ffmpeg not found: $Ffmpeg"
    exit 2
}

$root = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$lyric = Join-Path $root 'docs/example-ikari.krl'
if (-not (Test-Path -LiteralPath $lyric)) {
    Write-Error "make_smoke_fixtures: lyric example missing: $lyric"
    exit 2
}

New-Item -ItemType Directory -Force -Path $OutputDirectory | Out-Null

# Two visibly different patterns at the Cast pipeline's native rate, so the
# frame-cadence assertions in the soak have something real to measure.
$videos = @(
    @{ name = 'v1.mp4'; filter = 'testsrc2'; rate = 60 },
    @{ name = 'v2.mp4'; filter = 'testsrc';  rate = 60 }
)
foreach ($video in $videos) {
    $path = Join-Path $OutputDirectory $video.name
    Write-Host "make_smoke_fixtures: $($video.name)"
    & $Ffmpeg -y -v error -f lavfi `
        -i "$($video.filter)=size=1280x720:rate=$($video.rate):duration=$Seconds" `
        -c:v libx264 -preset veryfast -pix_fmt yuv420p $path
    if ($LASTEXITCODE -ne 0) { Write-Error "ffmpeg failed for $($video.name)" }
}

# Separate vocal and accompaniment tracks, matching how the App hands catalog
# audio to the host. PCM keeps decoding out of the picture.
$audios = @(
    @{ name = 'vocal1.wav'; frequency = 440 },
    @{ name = 'inst1.wav';  frequency = 220 },
    @{ name = 'vocal2.wav'; frequency = 330 },
    @{ name = 'inst2.wav';  frequency = 165 }
)
foreach ($audio in $audios) {
    $path = Join-Path $OutputDirectory $audio.name
    Write-Host "make_smoke_fixtures: $($audio.name)"
    & $Ffmpeg -y -v error -f lavfi `
        -i "sine=frequency=$($audio.frequency):sample_rate=44100:duration=$Seconds" `
        -ac 2 -c:a pcm_s16le $path
    if ($LASTEXITCODE -ne 0) { Write-Error "ffmpeg failed for $($audio.name)" }
}

foreach ($index in 1, 2) {
    Copy-Item -LiteralPath $lyric `
        -Destination (Join-Path $OutputDirectory "l$index.krl") -Force
}

$media = @(
    (Join-Path $OutputDirectory 'v1.mp4'),
    (Join-Path $OutputDirectory 'l1.krl'),
    (Join-Path $OutputDirectory 'vocal1.wav'),
    (Join-Path $OutputDirectory 'inst1.wav'),
    (Join-Path $OutputDirectory 'v2.mp4'),
    (Join-Path $OutputDirectory 'l2.krl'),
    (Join-Path $OutputDirectory 'vocal2.wav'),
    (Join-Path $OutputDirectory 'inst2.wav')
)
$quoted = ($media | ForEach-Object { '"' + $_ + '"' }) -join ' '

Write-Host ''
Write-Host 'make_smoke_fixtures: fixtures ready in'
Write-Host "  $OutputDirectory"
Write-Host ''
Write-Host 'Native dual-screen path (texture output only):'
Write-Host "  build/bin/kirakara_show_unified_native_smoke.exe $quoted --texture-only --soak-seconds=8 --stage-diagnostics"
Write-Host ''
Write-Host 'Cast path:'
Write-Host "  build/bin/kirakara_show_unified_cast_smoke.exe $quoted --soak-seconds=8"
