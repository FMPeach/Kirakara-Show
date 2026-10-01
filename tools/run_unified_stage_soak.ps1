[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$FirstSongDirectory,
    [Parameter(Mandatory = $true)]
    [string]$SecondSongDirectory,
    [ValidateRange(1, 86400)]
    [int]$Seconds = 1800,
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

function Resolve-SongAssets {
    param([Parameter(Mandatory)][string]$Directory)

    $resolved = (Resolve-Path -LiteralPath $Directory).Path
    $assets = @(
        (Join-Path $resolved 'background.mp4'),
        (Join-Path $resolved 'lyrics.krl'),
        (Join-Path $resolved 'vocal.m4a'),
        (Join-Path $resolved 'accompaniment.m4a')
    )
    foreach ($asset in $assets) {
        if (-not (Test-Path -LiteralPath $asset -PathType Leaf)) {
            throw "Required soak asset is missing: $asset"
        }
    }
    return $assets
}

$executable = Join-Path $BuildDirectory 'bin\kirakara_show_unified_native_smoke.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Build the Release smoke target first: $executable"
}

$first = Resolve-SongAssets -Directory $FirstSongDirectory
$second = Resolve-SongAssets -Directory $SecondSongDirectory
$arguments = @($first) + @($second) + "--soak-seconds=$Seconds"

Write-Host "Running Unified Stage soak for $Seconds seconds..."
& $executable @arguments
if ($LASTEXITCODE -ne 0) {
    throw "Unified Stage soak failed with exit code $LASTEXITCODE"
}
