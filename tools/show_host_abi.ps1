<#
.SYNOPSIS
    Guards the show_host C ABI (the Flutter FFI contract) against refactors.

.DESCRIPTION
    ShowHostHandle is opaque, so internal restructuring (splitting
    host_commands.h into translation units, regrouping ShowHost state,
    introducing CastSession) must not change the exported symbol set. This
    script dumps the export table of the built DLL and compares it with a
    committed baseline, so an accidental rename or a dropped export fails in
    CI instead of at runtime inside the App.

    The workspace path contains non-ASCII characters, which MinGW's objdump
    cannot receive on its command line, so the DLL is copied to an ASCII temp
    path first.

.PARAMETER Dll
    Path to the built show_host DLL.

.PARAMETER Baseline
    Path to the committed export list. Add -Update to rewrite it.

.PARAMETER Objdump
    objdump executable to use. Defaults to objdump from PATH.

.PARAMETER Update
    Rewrite the baseline from the current DLL. Only use together with an
    intentional ABI change.
#>
[CmdletBinding()]
param(
    [Parameter(Mandatory = $true)][string]$Dll,
    [Parameter(Mandatory = $true)][string]$Baseline,
    [string]$Objdump = 'objdump',
    [switch]$Update
)

$ErrorActionPreference = 'Stop'

if (-not (Test-Path -LiteralPath $Dll)) {
    Write-Error "show_host ABI: DLL not found: $Dll"
    exit 2
}

$stage = Join-Path ([System.IO.Path]::GetTempPath()) `
    ('show_host_abi_' + [guid]::NewGuid().ToString('N') + '.dll')
Copy-Item -LiteralPath $Dll -Destination $stage -Force

try {
    $raw = & $Objdump -p $stage 2>&1
    $dumpExit = $LASTEXITCODE
} finally {
    Remove-Item -LiteralPath $stage -Force -ErrorAction SilentlyContinue
}

if ($dumpExit -ne 0) {
    Write-Error "show_host ABI: objdump failed ($dumpExit): $raw"
    exit 2
}

$exports = @(
    $raw |
        Where-Object { $_ -match 'show_host_' -and $_ -notmatch 'file format' } |
        ForEach-Object {
            if ($_ -match '(show_host_[A-Za-z0-9_]+)\s*$') { $Matches[1] }
        } |
        Sort-Object -Unique
)

if ($exports.Count -eq 0) {
    Write-Error "show_host ABI: no show_host_* exports found in $Dll"
    exit 2
}

if ($Update) {
    # Keep every element a single interpolated string: in PowerShell the comma
    # operator binds tighter than '+', so concatenating inside @() would merge
    # the header into one line.
    $header = @(
        "# Exported C ABI of show_host ($($exports.Count) symbols).",
        "# Regenerate with: tools/show_host_abi.ps1 -Dll <dll> -Baseline <file> -Update",
        "# The Flutter FFI contract must stay stable across internal refactors."
    )
    $header + $exports | Set-Content -LiteralPath $Baseline -Encoding utf8
    Write-Host "show_host ABI: baseline rewritten with $($exports.Count) exports"
    exit 0
}

if (-not (Test-Path -LiteralPath $Baseline)) {
    Write-Error "show_host ABI: baseline missing: $Baseline"
    exit 2
}

# Name this differently from the -Baseline parameter: PowerShell variable names
# are case-insensitive, so $baseline and $Baseline would be the same variable
# and reading through it would clobber the path.
$baselineExports = New-Object System.Collections.Generic.List[string]
foreach ($line in [System.IO.File]::ReadAllLines($Baseline)) {
    $trimmed = $line.Trim()
    if ($trimmed.Length -eq 0 -or $trimmed[0] -eq '#') { continue }
    $baselineExports.Add($trimmed)
}
$baselineExports = @($baselineExports | Sort-Object -Unique)

$removed = @($baselineExports | Where-Object { $exports -notcontains $_ })
$added = @($exports | Where-Object { $baselineExports -notcontains $_ })

if ($removed.Count -eq 0 -and $added.Count -eq 0) {
    Write-Host "show_host ABI: $($exports.Count) exports unchanged"
    exit 0
}

Write-Host "show_host ABI changed: $($baselineExports.Count) -> $($exports.Count) exports"
if ($removed.Count -gt 0) {
    Write-Host 'removed:'
    $removed | ForEach-Object { Write-Host "  - $_" }
}
if ($added.Count -gt 0) {
    Write-Host 'added:'
    $added | ForEach-Object { Write-Host "  + $_" }
}
Write-Host ('If this is intentional, regenerate the baseline with -Update ' +
    'and update the App side in the same change.')
exit 1
