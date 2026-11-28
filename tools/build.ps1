# build.ps1 - build everything this project needs, from source, on a clean machine.
#
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1
#   powershell -ExecutionPolicy Bypass -File tools\build.ps1 -Mod -GameDir "C:\...\Call of Duty 2 - coop-test"
#
# Produces:
#   bridge\cod2net5.dll   in-process bridge (UDP pose -> cvar strings)
#   bridge\loader.exe     generic x86 LoadLibrary loader (mirror of coop-kit/shared/inject/loader.c)
#   with -Mod: <GameDir>\main\zz_coop.iwd  (GSC map-script override, built from the game's own script)
#
# The game must be closed for -Mod, because a mounted .iwd is locked by the process.

param(
    [string]$GameDir = "",
    [string]$TestDir = "",
    [ValidateSet('probe', 'puppet', 'follow', 'char', 'net', 'net2', 'net3', 'net4', 'net5', 'net6', 'net7')]
    [string]$Probe = 'net',
    [string[]]$Levels = @(),
    [switch]$SkipBriefing,
    [switch]$Mod
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$bridge = Join-Path $root 'bridge'
$id = Get-Content (Join-Path $root 'docs\identity.json') -Raw | ConvertFrom-Json
if (-not $GameDir) { $GameDir = $id.install_default }
# Never default the mod output to the ORIGINAL install. While the installs are swapped the test copy
# lives under the normal Steam path and the original is renamed, so pick the directory that is
# actually the test copy right now.
if (-not $TestDir) {
    $TestDir = $id.test_install_default
    $backup = $id.install_default -replace 'Call of Duty 2$', 'Call of Duty 2 - original'
    if (-not (Test-Path $TestDir) -and (Test-Path $backup)) {
        $TestDir = $id.install_default   # swapped: this path is the test copy
        Write-Host "installs are swapped: mod target is $TestDir"
    }
}

$vcvars = @(
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) { throw "vcvars32.bat not found - install Visual Studio Build Tools with the C++ workload" }
Write-Host "toolchain: $vcvars"

function Build([string]$source, [string]$out, [string]$extra) {
    $target = Join-Path $bridge $out
    # A DLL that is currently injected into the game is locked: cl fails with LNK1104, the old
    # binary stays on disk, and this function used to report success because the file existed.
    $before = if (Test-Path $target) { (Get-Item $target).LastWriteTimeUtc } else { [datetime]::MinValue }
    $cmd = "call `"$vcvars`" >nul && cd /d `"$bridge`" && cl /nologo /MT /O2 /DCOOP_LOCAL_TEST /D_CRT_SECURE_NO_WARNINGS $source /Fe:$out $extra 2>&1"
    $out2 = cmd.exe /c $cmd | Out-String
    $after = if (Test-Path $target) { (Get-Item $target).LastWriteTimeUtc } else { [datetime]::MinValue }
    if ($out2 -match '(?m)\berror\b|LNK\d{4}' -or $after -le $before) {
        Write-Host $out2
        throw "build failed: $out (if the game is running with this DLL loaded, the file is locked - build under another name)"
    }
    $h = (Get-FileHash $target -Algorithm SHA256).Hash
    Write-Host ("built {0,-18} {1,8} B  sha256={2}" -f $out, (Get-Item $target).Length, $h)
}

Build 'cod2net5.c bridge_threads.c' 'cod2net5.dll' '/LD ws2_32.lib user32.lib gdi32.lib'
# The mod is built for the test copy and the bridge is the other half of the same release: installing one without the
# other is how a stale cod2net5.dll caused "unknown function" compile errors twice. Copy it next to the game as well.
if ($TestDir -and (Test-Path $TestDir)) {
    Copy-Item (Join-Path $bridge 'cod2net5.dll') (Join-Path $TestDir 'cod2net5.dll') -Force
    Write-Host ("bridge installed into " + $TestDir + " (" + (Get-FileHash (Join-Path $bridge 'cod2net5.dll') -Algorithm SHA256).Hash.Substring(0,16) + ")")
}
Build 'loader.c'   'loader.exe'   ''

if ($Mod) {
    if (Get-Process CoD2SP_s -ErrorAction SilentlyContinue) { throw "close the game first: a mounted .iwd is locked" }
    $modScript = Join-Path $root 'tools\make_mod.ps1'
    Write-Host "building the GSC mod into $TestDir (probe: $Probe)"
    # called in-session: passing an empty -Suffix through powershell.exe -File drops the argument
    & $modScript -GameDir $GameDir -TestDir $TestDir -Probe $Probe -Level moscow -Levels $Levels -SkipBriefing:$SkipBriefing
    if ($LASTEXITCODE -ne 0) { throw "make_mod.ps1 failed" }
}

Write-Host ""
Write-Host "done. next:"
Write-Host "  tools\verify.ps1"
Write-Host "  tools\switch-install.ps1 -Mode test   # if the mod must run from a copy"
Write-Host "  Start-Process steam://rungameid/2630"
Write-Host "  bridge\loader.exe --process CoD2SP_s.exe --dll bridge\cod2net5.dll"
Write-Host "  start the game and click CO-OP in the main menu"
