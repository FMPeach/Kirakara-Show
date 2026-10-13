param([string]$SdkDirectory = (Join-Path $PSScriptRoot '../.deps/ffmpeg-sdk'))
$ErrorActionPreference = 'Stop'
$repository = [IO.Path]::GetFullPath((Join-Path $PSScriptRoot '..'))
$sdk = [IO.Path]::GetFullPath($SdkDirectory)
$lockFile = Join-Path $repository 'third_party/ffmpeg/dependency.json'
$dependency = Get-Content -LiteralPath $lockFile -Raw | ConvertFrom-Json
$downloads = Join-Path $repository '.deps/ffmpeg-download'
New-Item -ItemType Directory -Force $downloads, $sdk | Out-Null
$archive = Join-Path $downloads 'sdk.zip'
if (!(Test-Path -LiteralPath $archive) -or
    (Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $dependency.archiveSha256) {
    Invoke-WebRequest $dependency.archiveUrl -OutFile $archive
}
if ((Get-FileHash -LiteralPath $archive -Algorithm SHA256).Hash.ToLowerInvariant() -ne $dependency.archiveSha256) {
    throw 'FFmpeg SDK SHA-256 mismatch.'
}
$expanded = Join-Path $downloads 'expanded'
Expand-Archive -LiteralPath $archive -DestinationPath $expanded -Force
$package = Join-Path $expanded 'ffmpeg-n8.1.3-18-g46d899d7a0-win64-lgpl-shared-8.1'
foreach ($directory in @('bin', 'include', 'lib', 'doc')) {
    Copy-Item -LiteralPath (Join-Path $package $directory) -Destination $sdk -Recurse -Force
}
Copy-Item -LiteralPath (Join-Path $package 'LICENSE.txt') -Destination $sdk -Force
$version = & (Join-Path $sdk 'bin/ffmpeg.exe') -version 2>&1 | Out-String
if ($LASTEXITCODE -ne 0 -or $version -match '--enable-(gpl|nonfree)(\s|$)' -or
    $version -notmatch '--enable-shared' -or $version -notmatch '--enable-libdav1d') {
    throw 'Expected LGPL shared FFmpeg with dav1d and without GPL/nonfree components.'
}
$notices = Join-Path $sdk 'third_party/ffmpeg'
$sources = Join-Path $notices 'sources'
New-Item -ItemType Directory -Force $sources | Out-Null
Copy-Item -LiteralPath $lockFile -Destination $notices -Force
Copy-Item -LiteralPath (Join-Path $sdk 'LICENSE.txt') -Destination $notices -Force
Copy-Item -LiteralPath (Join-Path $repository 'third_party/ffmpeg/README.md') -Destination $notices -Force
Set-Content -LiteralPath (Join-Path $notices 'build-configuration.txt') -Value $version -Encoding utf8
foreach ($license in @('COPYING.LGPLv3', 'COPYING.GPLv3')) {
    $uri = "https://raw.githubusercontent.com/FFmpeg/FFmpeg/$($dependency.ffmpegRevision)/$license"
    Invoke-WebRequest $uri -OutFile (Join-Path $notices $license)
}
$sourceArchives = @(
    @{ name = 'ffmpeg-source.zip'; uri = "https://github.com/FFmpeg/FFmpeg/archive/$($dependency.ffmpegRevision).zip" },
    @{ name = 'build-scripts.zip'; uri = "https://github.com/BtbN/FFmpeg-Builds/archive/$($dependency.buildScriptsRevision).zip" }
)
foreach ($source in $sourceArchives) {
    $destination = Join-Path $sources $source.name
    if (!(Test-Path -LiteralPath $destination)) { Invoke-WebRequest $source.uri -OutFile $destination }
}
Get-ChildItem -LiteralPath $sources -File | Get-FileHash -Algorithm SHA256 |
    Select-Object @{ n = 'file'; e = { Split-Path $_.Path -Leaf } }, Hash |
    ConvertTo-Json | Set-Content -LiteralPath (Join-Path $notices 'source-hashes.json') -Encoding utf8
Write-Host "FFmpeg shared SDK ready: $sdk"
