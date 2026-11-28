# make_test_install.ps1 - create the hardlink test copy of the game.
#
# The game is launched by Steam from <steamapps>\common\Call of Duty 2, so the test copy must be
# a sibling folder that switch-install.ps1 can swap into that path.
#
# Layout:
#   * every .iwd is a HARDLINK (3.4 GB, no extra space, cannot be modified through the copy)
#   * everything else is a real copy (exes, dlls, Docs, pb, cfg - about 9 MB)
#   * nothing the game writes at runtime is hardlinked: main\players, *.cfg, *.log are excluded,
#     so the original install and its saves cannot be changed through the test copy.
#
#   powershell -ExecutionPolicy Bypass -File tools\make_test_install.ps1
#   powershell -ExecutionPolicy Bypass -File tools\make_test_install.ps1 -Force

param(
    [string]$GameDir = "",
    [string]$TestDir = "",
    [switch]$Force
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$id = Get-Content (Join-Path $root 'docs\identity.json') -Raw | ConvertFrom-Json
if (-not $GameDir) { $GameDir = $id.install_default }
if (-not $TestDir) { $TestDir = $id.test_install_default }

if (-not (Test-Path (Join-Path $GameDir $id.exe_name))) {
    throw "no game at $GameDir (expected $($id.exe_name))"
}
$hash = (Get-FileHash (Join-Path $GameDir $id.exe_name) -Algorithm SHA256).Hash
if ($hash -ne $id.exe_sha256) {
    throw "exe hash mismatch at $GameDir - refusing to build a test copy from an unknown build"
}

if (Test-Path $TestDir) {
    if (-not $Force) { throw "test install already exists: $TestDir (use -Force to recreate)" }
    Remove-Item $TestDir -Recurse -Force
}

# what must never be hardlinked / copied: runtime state of the original install
$skipNames = @('players', 'hunkusage.dat')
$skipExt = @('.log', '.dmp', '.cfg.bak')

Write-Host "source: $GameDir"
Write-Host "target: $TestDir"

$link = 0; $copy = 0
Get-ChildItem $GameDir -Recurse -Force | ForEach-Object {
    $rel = $_.FullName.Substring($GameDir.Length).TrimStart('\')
    if ($_.PSIsContainer) { return }
    if ($skipExt -contains $_.Extension) { return }
    if ($rel -like 'main\players\*') { return }
    if ($rel -like 'main\zz_coop*' -or $rel -like 'coop5.log') { return }

    $dst = Join-Path $TestDir $rel
    New-Item -ItemType Directory -Force (Split-Path $dst) | Out-Null

    if ($_.Extension -eq '.iwd') {
        New-Item -ItemType HardLink -Path $dst -Target $_.FullName | Out-Null
        $script:link++
    } else {
        Copy-Item $_.FullName $dst -Force
        $script:copy++
    }
}

# an empty players dir so the first launch has somewhere to write
New-Item -ItemType Directory -Force (Join-Path $TestDir 'main\players') | Out-Null

$testHash = (Get-FileHash (Join-Path $TestDir $id.exe_name) -Algorithm SHA256).Hash
Write-Host ""
Write-Host "hardlinked .iwd : $link"
Write-Host "copied files    : $copy"
Write-Host "exe sha256      : $testHash"
if ($testHash -ne $id.exe_sha256) { throw "test copy exe hash mismatch" }

$h = Get-Item (Join-Path $TestDir 'main\iw_00.iwd')
Write-Host "iw_00 linktype  : $($h.LinkType)"
Write-Host "done."
