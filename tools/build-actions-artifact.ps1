#Requires -Version 5.1
[CmdletBinding()]
param([string]$BuildDirectory, [string]$OutputDirectory)
$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
if (-not $BuildDirectory) { $BuildDirectory = Join-Path $root 'build/actions-test/compile' }
if (-not $OutputDirectory) { $OutputDirectory = Join-Path $root 'build/actions-test/artifact' }
$BuildDirectory = [IO.Path]::GetFullPath($BuildDirectory)
$OutputDirectory = [IO.Path]::GetFullPath($OutputDirectory)
if ((Test-Path -LiteralPath $OutputDirectory) -and
    @(Get-ChildItem -LiteralPath $OutputDirectory -Force).Count) {
  throw 'OutputDirectory must be empty; use a new directory for each build.'
}
$commit = (& git -C $root rev-parse HEAD).Trim()
if ($LASTEXITCODE -ne 0) { throw 'Cannot identify Show source commit.' }
$tests = @('kirakara_show_tests', 'kirakara_show_song_title_tests',
  'kirakara_show_stage_frame_tests', 'kirakara_show_stage_overlay_tests',
  'kirakara_show_cast_program_clock_tests', 'kirakara_show_native_stage_program_clock_tests')
& cmake -S $root -B $BuildDirectory -G Ninja -DCMAKE_BUILD_TYPE=Release -DKIRAKARA_BUILD_TESTS=ON
if ($LASTEXITCODE -ne 0) { throw 'Show CMake configure failed.' }
& cmake --build $BuildDirectory --parallel 2 --target show_host @tests
if ($LASTEXITCODE -ne 0) { throw 'Show build failed.' }
& ctest --test-dir $BuildDirectory --output-on-failure --no-tests=error -R ('^(' + ($tests -join '|') + ')$')
if ($LASTEXITCODE -ne 0) { throw 'Show unit tests failed.' }
$dlls = @(Get-ChildItem -LiteralPath (Join-Path $BuildDirectory 'bin') -Filter '*show_host.dll' -File)
if ($dlls.Count -ne 1) { throw 'Expected exactly one show_host DLL.' }
# The production DLL is unchanged. Compare the existing C ABI, not GPU/TV behavior.
& (Join-Path $root 'tools/show_host_abi.ps1') -Dll $dlls[0].FullName `
  -Baseline (Join-Path $root 'tests/abi/show_host_exports.txt')
if ($LASTEXITCODE -ne 0) { throw 'Show ABI verification failed.' }
$null = New-Item -ItemType Directory -Path $OutputDirectory -Force
$output = Join-Path $OutputDirectory 'libshow_host.dll'
Copy-Item -LiteralPath $dlls[0].FullName -Destination $output
$manifest = [ordered]@{
  schemaVersion = 1; component = 'show'; target = 'windows-x64'
  source = [ordered]@{ repository = 'FMPeach/Kirakara-Show'; commit = $commit }
  build = [ordered]@{
    runId = if ($env:GITHUB_RUN_ID) { $env:GITHUB_RUN_ID } else { 'local' }
    runAttempt = if ($env:GITHUB_RUN_ATTEMPT) { $env:GITHUB_RUN_ATTEMPT } else { '1' }
  }
  entryPoint = 'libshow_host.dll'
  files = @([ordered]@{ path = 'libshow_host.dll'; size = (Get-Item -LiteralPath $output).Length
      sha256 = (Get-FileHash -LiteralPath $output -Algorithm SHA256).Hash })
}
[IO.File]::WriteAllText((Join-Path $OutputDirectory 'component-manifest.json'),
  ($manifest | ConvertTo-Json -Depth 8) + "`n", [Text.UTF8Encoding]::new($false))
Write-Host "Show artifact: $OutputDirectory"
