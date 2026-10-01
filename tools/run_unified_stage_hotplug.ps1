[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)]
    [string]$FirstSongDirectory,
    [Parameter(Mandatory = $true)]
    [string]$SecondSongDirectory,
    [ValidateRange(15, 300)]
    [int]$Seconds = 20,
    [ValidateRange(0, 5000)]
    [int]$DisconnectedMilliseconds = 250,
    [string]$BuildDirectory = (Join-Path $PSScriptRoot '..\build')
)

Set-StrictMode -Version Latest
$ErrorActionPreference = 'Stop'

if (-not ('Kirakara.NativeMonitorMetrics' -as [type])) {
    Add-Type -TypeDefinition @'
namespace Kirakara {
    using System.Runtime.InteropServices;

    public static class NativeMonitorMetrics {
        [DllImport("user32.dll")]
        public static extern int GetSystemMetrics(int index);
    }
}
'@
}

function Get-ActiveMonitorCount {
    return [Kirakara.NativeMonitorMetrics]::GetSystemMetrics(80)
}

function Wait-Until {
    param(
        [Parameter(Mandatory)][scriptblock]$Condition,
        [Parameter(Mandatory)][string]$FailureMessage,
        [ValidateRange(1, 300)][int]$TimeoutSeconds = 30
    )

    $deadline = [DateTime]::UtcNow.AddSeconds($TimeoutSeconds)
    do {
        if (& $Condition) {
            return
        }
        Start-Sleep -Milliseconds 100
    } while ([DateTime]::UtcNow -lt $deadline)

    throw $FailureMessage
}

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
            throw "Required hotplug asset is missing: $asset"
        }
    }
    return $assets
}

function Get-SmokeOutput {
    param([Parameter(Mandatory)][string]$Path)

    if (-not (Test-Path -LiteralPath $Path -PathType Leaf)) {
        return ''
    }
    return Get-Content -LiteralPath $Path -Raw -Encoding UTF8
}

$displaySwitch = Join-Path $env:WINDIR 'System32\DisplaySwitch.exe'
$executable = Join-Path $BuildDirectory 'bin\kirakara_show_unified_native_smoke.exe'
if (-not (Test-Path -LiteralPath $executable -PathType Leaf)) {
    throw "Build the Release smoke target first: $executable"
}

$first = Resolve-SongAssets -Directory $FirstSongDirectory
$second = Resolve-SongAssets -Directory $SecondSongDirectory
$arguments = @($first) + @($second) + @(
    "--soak-seconds=$Seconds",
    '--allow-transient-stage-fps'
)

$runId = [Guid]::NewGuid().ToString('N')
$stdoutPath = Join-Path $env:TEMP "kirakara-stage-hotplug-$runId.stdout.log"
$stderrPath = Join-Path $env:TEMP "kirakara-stage-hotplug-$runId.stderr.log"
$argumentLine = ($arguments | ForEach-Object {
    '"' + $_.Replace('"', '\"') + '"'
}) -join ' '
$process = $null
$startedAt = [DateTime]::UtcNow
$internalAt = $null
$restoredAt = $null

try {
    & $displaySwitch /extend
    Wait-Until -TimeoutSeconds 15 `
        -FailureMessage 'Extended desktop did not expose a second active monitor.' `
        -Condition { (Get-ActiveMonitorCount) -ge 2 }

    Write-Host "Starting Unified Stage hotplug smoke with $(Get-ActiveMonitorCount) monitors..."
    $process = Start-Process -FilePath $executable `
        -ArgumentList $argumentLine `
        -RedirectStandardOutput $stdoutPath `
        -RedirectStandardError $stderrPath `
        -PassThru `
        -NoNewWindow
    # Keep the native process handle alive so PowerShell can retrieve ExitCode
    # after the redirected child has already terminated.
    $null = $process.Handle

    Wait-Until -TimeoutSeconds 30 `
        -FailureMessage 'Smoke did not enter the physical Stage soak.' `
        -Condition {
            $process.Refresh()
            if ($process.HasExited) {
                return $true
            }
            (Get-SmokeOutput -Path $stdoutPath) -match
                'Unified Stage soak started .* with physical output'
        }
    $process.Refresh()
    if ($process.HasExited) {
        throw "Smoke exited before the display hotplug (exit $($process.ExitCode))."
    }

    & $displaySwitch /internal
    Wait-Until -TimeoutSeconds 15 `
        -FailureMessage 'Desktop did not switch to one active monitor.' `
        -Condition { (Get-ActiveMonitorCount) -eq 1 }
    $internalAt = [DateTime]::UtcNow

    Start-Sleep -Milliseconds $DisconnectedMilliseconds

    & $displaySwitch /extend
    Wait-Until -TimeoutSeconds 15 `
        -FailureMessage 'Second monitor did not return after hotplug.' `
        -Condition { (Get-ActiveMonitorCount) -ge 2 }
    $restoredAt = [DateTime]::UtcNow

    Wait-Until -TimeoutSeconds ($Seconds + 90) `
        -FailureMessage 'Smoke did not exit after the hotplug soak.' `
        -Condition {
            $process.Refresh()
            $process.HasExited
        }
    $process.WaitForExit()
    $process.Refresh()
    $exitCode = [int]$process.ExitCode

    $stdout = Get-SmokeOutput -Path $stdoutPath
    $stderr = Get-SmokeOutput -Path $stderrPath
    if ($stdout) {
        Write-Host $stdout.TrimEnd()
    }
    if ($stderr) {
        Write-Error $stderr.TrimEnd()
    }

    if ($exitCode -ne 0) {
        throw "Unified Stage hotplug smoke failed with exit code $exitCode."
    }
    if ($stdout -notmatch 'Unified ShowHost native Stage smoke passed with second display') {
        throw 'Smoke exited successfully without the second-display completion marker.'
    }

    $internalMs = [math]::Round(($internalAt - $startedAt).TotalMilliseconds)
    $restoredMs = [math]::Round(($restoredAt - $internalAt).TotalMilliseconds)
    Write-Host "Unified Stage hotplug passed: disconnect at ${internalMs}ms, reconnect after ${restoredMs}ms."
}
finally {
    & $displaySwitch /extend
    if ($null -ne $process) {
        $process.Refresh()
        if (-not $process.HasExited) {
            Stop-Process -Id $process.Id -Force -ErrorAction SilentlyContinue
        }
        $process.Dispose()
    }
}
