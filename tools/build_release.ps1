# build_release.ps1 - build the release for the second player: dist\release\CoD2Coop.exe (+ zip, sha256).
#
#   powershell -ExecutionPolicy Bypass -File tools\build_release.ps1
#
# Steps: bridge DLL (build.ps1) -> net5 GSC mod built from the STOCK moscow.gsc into a scratch folder (the
# game may stay open, no installation is touched) -> embed both into launcher\blobs.h -> compile
# launcher\cod2coop.c as one x86 exe (/MT, no runtime to install) -> README + zip.

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$id = Get-Content (Join-Path $root 'docs\identity.json') -Raw | ConvertFrom-Json
$out = Join-Path $root 'dist\release'
$version = (Select-String -Path (Join-Path $root 'launcher\cod2coop.c') -Pattern '#define VERSION "([^"]+)"').Matches[0].Groups[1].Value

# 1. bridge
& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build.ps1') | Select-String 'built'
$dll = Join-Path $root 'bridge\cod2net5.dll'
if (-not (Test-Path $dll)) { throw "bridge DLL missing" }

# 2. mod, from a folder whose main\ holds only the stock archives (a zz_coop.iwd there would be read
#    back as if it were the stock script)
$candidates = @(($id.install_default -replace 'Call of Duty 2$', 'Call of Duty 2 - original'), $id.install_default)
$stock = $candidates | Where-Object { (Test-Path (Join-Path $_ 'main\iw_00.iwd')) -and -not (Get-ChildItem (Join-Path $_ 'main') -Filter 'zz_coop*.iwd' -ErrorAction SilentlyContinue) } | Select-Object -First 1
if (-not $stock) { throw "no game folder without a zz_coop archive found to take the stock moscow.gsc from" }
$exeHash = (Get-FileHash (Join-Path $stock $id.exe_name) -Algorithm SHA256).Hash
if ($exeHash -ne $id.exe_sha256 -and $exeHash -ne $id.retail_exe_sha256) { throw "stock folder exe hash mismatch: $exeHash" }
$scratch = Join-Path $env:TEMP 'cod2coop-release-mod'
Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue
New-Item -ItemType Directory -Force (Join-Path $scratch 'main') | Out-Null
& (Join-Path $PSScriptRoot 'make_mod.ps1') -GameDir $stock -TestDir $scratch -Probe net7 -Level moscow -Levels all | Select-String 'probe variant|sha256:'
$iwd = Join-Path $scratch 'main\zz_coop.iwd'
if (-not (Test-Path $iwd)) { throw "mod archive not built" }

# the mod must be the net7 one with the pieces the bridge needs
$chk = Join-Path $env:TEMP 'release-check.py'
Set-Content $chk @'
import sys, zipfile
t = zipfile.ZipFile(sys.argv[1]).read("maps/moscow.gsc").decode()
need = ["coop_pose_move", "drone_run_forward_1", "COOPN peer lost", "coop_publish_state(", "coop_peer_state()", "coop_peer_yaw()", 'setcvar("coop_pyaw"', "setcontents(0)", 'setcvar("coop_anim", "0000000001")']
need += ["coop_dress(puppet);", "coop_precache_character();", "coop_weapon_id(", "coop_player_parent()", "coop_ride(", "coop_level_watch"]
miss = [n for n in need if n not in t]
z = zipfile.ZipFile(sys.argv[1])
names = set(z.namelist())
LEVELS = ['88ridge','beltot','bergstein','breakout','cityhall','crossroads','decoytown','decoytrenches','demolition','downtown_assault','downtown_sniper','duhoc_assault','duhoc_defend','elalamein','eldaba','hill400_assault','hill400_defend','libya','matmata','moscow','newvillers','rhine','silotown_assault','tankhunt','toujane','toujane_ride','trainyard']
for lv in LEVELS:
    n = "maps/" + lv + ".gsc"
    if n not in names: miss.append("level " + lv)
    elif b"coop_orig_main()" not in z.read(n) or b"coop_chan_publish" not in z.read(n): miss.append("level text " + lv)
for n in ("ui/main.menu", "ui/menus.txt", "ui/coop.menu"):
    if n not in names: miss.append("entry " + n)
if "ui/menus.txt" in names and b'loadMenu { "ui/coop.menu" }' not in z.read("ui/menus.txt"): miss.append("loadMenu coop")
if "ui/coop.menu" in names and (b"coop_status" not in z.read("ui/coop.menu") or b'coop_cmd "idle"' not in z.read("ui/coop.menu")): miss.append("coop.menu status line")
if "ui/main.menu" in names and z.read("ui/main.menu").count(b'open "coop_menu"') != 1: miss.append("main.menu coop button")
print("missing:", miss) if miss else print("mod text and menu files ok")
sys.exit(1 if miss else 0)
'@
python -I $chk $iwd
if ($LASTEXITCODE -ne 0) { throw "mod text check failed" }

# 2b. the d3d9 proxy that loads the bridge at game start (tested without the game: it forwards, and loads the bridge only in CoD2SP_s.exe)
& powershell -ExecutionPolicy Bypass -File (Join-Path $PSScriptRoot 'build_proxy.ps1') -Test | Select-String 'built d3d9|passed|FAIL'
if ($LASTEXITCODE -ne 0) { throw "proxy build or test failed" }
$proxyDll = Join-Path $root 'proxy\d3d9.dll'
if (-not (Test-Path $proxyDll)) { throw "proxy d3d9.dll missing" }

# 3. embed + compile
$blobs = Join-Path $root 'launcher\blobs.h'
# NO game content is embedded: the mod archive is built on the player's PC from the player's own game files by the scripts below
python -I (Join-Path $PSScriptRoot 'embed_blob.py') $blobs "g_dll=$dll" "g_d3d9=$proxyDll" "g_ps_make=$(Join-Path $PSScriptRoot 'make_mod.ps1')" "g_ps_menu=$(Join-Path $PSScriptRoot 'patch_menu.ps1')" "g_atr=$(Join-Path $PSScriptRoot 'animtrees\coop_puppet.atr')" "g_coopmenu=$(Join-Path $root 'launcher\menu\coop.menu')"
if ($LASTEXITCODE -ne 0) { throw "embedding failed" }

$vcvars = @(
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) { throw "vcvars32.bat not found" }

New-Item -ItemType Directory -Force $out | Out-Null
$exe = Join-Path $out 'CoD2Coop.exe'
Remove-Item $exe -Force -ErrorAction SilentlyContinue
$cmd = "call `"$vcvars`" >nul && cd /d `"$($root)\launcher`" && cl /nologo /MT /O2 /GS /W3 /DNDEBUG /D_CRT_SECURE_NO_WARNINGS /D_WINSOCK_DEPRECATED_NO_WARNINGS cod2coop.c /Fe:`"$exe`" /link /RELEASE /INCREMENTAL:NO 2>&1"
$log = cmd.exe /c $cmd | Out-String
if (-not (Test-Path $exe) -or $log -match '(?m)\berror\b|LNK\d{4}') { Write-Host $log; throw "launcher build failed" }
Remove-Item (Join-Path $root 'launcher\cod2coop.obj') -Force -ErrorAction SilentlyContinue
if ($log -match 'warning') { ($log -split "`n" | Select-String 'warning') | Select-Object -First 8 | ForEach-Object { Write-Host $_ } }

# 4. extras
Copy-Item (Join-Path $PSScriptRoot 'log_collector.py') $out -Force
$readme = Join-Path $root 'launcher\README-player2.txt'
if (Test-Path $readme) { Copy-Item $readme (Join-Path $out 'README.txt') -Force }
$h = (Get-FileHash $exe -Algorithm SHA256).Hash
Set-Content (Join-Path $out 'CoD2Coop.exe.sha256') "$h  CoD2Coop.exe" -Encoding ASCII
$zip = Join-Path $root "dist\CoD2Coop-$version.zip"
Remove-Item $zip -Force -ErrorAction SilentlyContinue
# an explicit list, not '*': 'connect' may leave an extracted cod2net5_<n>.dll next to the exe when it is run from here
$release = 'CoD2Coop.exe', 'CoD2Coop.exe.sha256', 'README.txt', 'log_collector.py'
Get-ChildItem $out -Filter 'cod2net5_*.dll' -ErrorAction SilentlyContinue | Remove-Item -Force -ErrorAction SilentlyContinue   # locked ones (loaded by a game) stay, but are not zipped
Compress-Archive -Path ($release | ForEach-Object { Join-Path $out $_ }) -DestinationPath $zip
Remove-Item $scratch -Recurse -Force -ErrorAction SilentlyContinue

Get-ChildItem $out | ForEach-Object { "{0,-24} {1,10} B" -f $_.Name, $_.Length }
Write-Host "exe sha256: $h"
Write-Host "zip       : $zip ($((Get-Item $zip).Length) B)"
