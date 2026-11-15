# verify.ps1 - check that this machine can build and run the mod.
#
# Read-only. Prints one PASS/FAIL line per check and exits 0 only if all hard checks pass.
# Soft checks (things that only matter when running the game) are reported as INFO.
#
#   powershell -ExecutionPolicy Bypass -File tools\verify.ps1
#   powershell -ExecutionPolicy Bypass -File tools\verify.ps1 -GameDir "D:\Games\Call of Duty 2"

param(
    [string]$GameDir = "",
    [string]$TestDir = ""
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$idPath = Join-Path $root 'docs\identity.json'
if (-not (Test-Path $idPath)) { Write-Host "FAIL  identity.json missing at $idPath"; exit 2 }
$id = Get-Content $idPath -Raw | ConvertFrom-Json

if (-not $GameDir) { $GameDir = $id.install_default }
if (-not $TestDir) { $TestDir = $id.test_install_default }

$fail = 0
function Check($ok, $name, $detail) {
    if ($ok) { Write-Host ("PASS  {0,-38} {1}" -f $name, $detail) }
    else { Write-Host ("FAIL  {0,-38} {1}" -f $name, $detail); $script:fail++ }
}
function Info($name, $detail) { Write-Host ("INFO  {0,-38} {1}" -f $name, $detail) }

Write-Host "=== identity ==="
Write-Host ("game      : {0} ({1} {2})" -f $id.game, $id.store, $id.appid)
Write-Host ("exe       : {0}  {1} bytes" -f $id.exe_name, $id.exe_size)
Write-Host ("expected  : {0}" -f $id.exe_sha256)

$exe = Join-Path $GameDir $id.exe_name
Check (Test-Path $exe) 'game install present' $GameDir
if (Test-Path $exe) {
    $size = (Get-Item $exe).Length
    Check ($size -eq $id.exe_size) 'exe size matches' "$size bytes"
    $hash = (Get-FileHash $exe -Algorithm SHA256).Hash
    Check ($hash -eq $id.exe_sha256) 'exe sha256 matches' $hash
    $inf = Join-Path $GameDir 'version.inf'
    if (Test-Path $inf) { Info 'version.inf' ((Get-Content $inf | Select-String 'ExtVersion|IntVersion|QA') -join '; ') }
    Check (Test-Path (Join-Path $GameDir 'main\iw_07.iwd')) 'data archives present' 'main\iw_07.iwd'
}

Write-Host "=== build toolchain ==="
$vcvars = @(
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
Check ([bool]$vcvars) 'msvc x86 toolchain' ($(if ($vcvars) { $vcvars } else { 'vcvars32.bat not found - install VS Build Tools with C++' }))

$py = Get-Command python -ErrorAction SilentlyContinue
Check ([bool]$py) 'python' ($(if ($py) { $py.Source } else { 'python not on PATH' }))

Write-Host "=== mod artefacts in this repo ==="
Check (Test-Path (Join-Path $root 'bridge\cod2net5.c')) 'bridge source present' 'bridge\cod2net5.c'
Check (Test-Path (Join-Path $root 'bridge\loader.c')) 'loader source present' 'bridge\loader.c'
Check (Test-Path (Join-Path $root 'tools\make_mod.ps1')) 'mod builder present' 'tools\make_mod.ps1'

Write-Host "=== running state (informational) ==="
if (Test-Path $TestDir) {
    $mod = Join-Path $TestDir $id.mod_archive
    if (Test-Path $mod) { Info 'test install has mod archive' $mod } else { Info 'test install without mod' $TestDir }
} else { Info 'test install not created' $TestDir }
$p = Get-Process CoD2SP_s -ErrorAction SilentlyContinue
if ($p) { Info 'game process' ("pid $($p.Id)") } else { Info 'game process' 'not running' }

Write-Host ""
if ($fail -eq 0) { Write-Host "ALL OK - this machine can build the mod" ; exit 0 }
Write-Host "$fail check(s) failed" ; exit 1
