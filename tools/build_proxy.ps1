# build_proxy.ps1 - build the d3d9 proxy (proxy\d3d9.dll) and its test exe (proxy\proxytest.exe), x86.
#
#   powershell -ExecutionPolicy Bypass -File tools\build_proxy.ps1 [-Test]
#
# -Test copies proxy + bridge into a scratch folder twice (once as CoD2SP_s.exe, once as other.exe) and runs the
# checks without the game: the proxy must forward Direct3DCreate9, and may load the bridge only in CoD2SP_s.exe.

param([switch]$Test)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
$proxy = Join-Path $root 'proxy'
$vcvars = @(
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat',
    'C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat',
    'C:\Program Files (x86)\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat'
) | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $vcvars) { throw "vcvars32.bat not found" }

function Compile([string]$line, [string]$out) {
    $target = Join-Path $proxy $out
    Remove-Item $target -Force -ErrorAction SilentlyContinue
    $cmd = "call `"$vcvars`" >nul && cd /d `"$proxy`" && cl /nologo /MT /O2 /W3 /D_CRT_SECURE_NO_WARNINGS $line 2>&1"
    $log = cmd.exe /c $cmd | Out-String
    if (-not (Test-Path $target) -or $log -match '(?m)\berror\b|LNK\d{4}') { Write-Host $log; throw "build failed: $out" }
    if ($log -match 'warning') { ($log -split "`n" | Select-String 'warning') | ForEach-Object { Write-Host $_ } }
    "built {0,-18} {1,8} B  sha256={2}" -f $out, (Get-Item $target).Length, (Get-FileHash $target -Algorithm SHA256).Hash
    Remove-Item (Join-Path $proxy '*.obj'), (Join-Path $proxy '*.lib'), (Join-Path $proxy '*.exp') -Force -ErrorAction SilentlyContinue
}

Compile 'd3d9proxy.c /LD /Fe:d3d9.dll' 'd3d9.dll'
Compile 'proxytest.c /Fe:proxytest.exe user32.lib' 'proxytest.exe'

if ($Test) {
    $bridge = Join-Path $root 'bridge\cod2net5.dll'
    if (-not (Test-Path $bridge)) { throw "build the bridge first (tools\build.ps1)" }
    $fails = 0
    foreach ($case in @(@{ name = 'CoD2SP_s.exe'; expectBridge = $true }, @{ name = 'other.exe'; expectBridge = $false })) {
        $t = Join-Path $env:TEMP ("proxytest-" + [IO.Path]::GetFileNameWithoutExtension($case.name))
        Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
        New-Item -ItemType Directory -Force $t | Out-Null
        Copy-Item (Join-Path $proxy 'd3d9.dll') $t
        Copy-Item $bridge $t
        Copy-Item (Join-Path $proxy 'proxytest.exe') (Join-Path $t $case.name)
        Write-Host "--- as $($case.name):"
        $out = & (Join-Path $t $case.name) 2>&1 | Out-String
        $out.TrimEnd() -split "`n" | ForEach-Object { Write-Host "  $_" }
        $loaded = $out -match 'loaded into this process: YES'
        $forwarded = $out -match 'non-null'
        if (-not $forwarded) { Write-Host "  FAIL: no forward"; $fails++ }
        if ($loaded -ne $case.expectBridge) { Write-Host "  FAIL: bridge loaded=$loaded, expected $($case.expectBridge)"; $fails++ }
        Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
    }
    # live reload: while the test exe runs, the bridge file is replaced; the proxy must stop the running copy, free it and
    # load a fresh one (two "=== cod2net5" start lines, the old copy logs stop/stopped, exactly one live copy loaded at the end)
    $t = Join-Path $env:TEMP 'proxytest-reload'
    Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $t | Out-Null
    Copy-Item (Join-Path $proxy 'd3d9.dll') $t
    Copy-Item $bridge $t
    Copy-Item (Join-Path $proxy 'proxytest.exe') (Join-Path $t 'CoD2SP_s.exe')
    Write-Host "--- live reload (the bridge file is replaced while the exe runs):"
    $outFile = Join-Path $t 'out.txt'
    $p = Start-Process -FilePath (Join-Path $t 'CoD2SP_s.exe') -ArgumentList '16' -WorkingDirectory $t -RedirectStandardOutput $outFile -PassThru -WindowStyle Hidden
    Start-Sleep -Seconds 5
    (Get-Item (Join-Path $t 'cod2net5.dll')).LastWriteTime = (Get-Date)     # a changed file time is a changed bridge
    $null = $p.WaitForExit(40000)
    $out = Get-Content $outFile -Raw
    $log = Get-Content (Join-Path $t 'coop5.log') -Raw
    $starts = ([regex]::Matches($log, '=== cod2net5')).Count
    $okStop = $log -match 'stop requested \(live reload\)' -and $log -match 'bridge .*: stopped'
    $okProxy = $log -match '\[proxy\].*reloading the bridge' -and $log -match '\[proxy\].*bridge reloaded'
    $m = [regex]::Match($out, 'bridge modules: (\d+) \[(.*)\]')
    $copies = if ($m.Success) { [int]$m.Groups[1].Value } else { -1 }
    Write-Host "  bridge start lines: $starts (expected 2); stop logged: $okStop; proxy reload lines: $okProxy; modules at the end: $copies [$($m.Groups[2].Value)]"
    $log -split "`n" | Where-Object { $_ -match 'proxy|stop|=== cod2net5' } | Select-Object -First 10 | ForEach-Object { Write-Host "  | $($_.TrimEnd())" }
    if ($starts -ne 2) { Write-Host "  FAIL: expected two bridge starts"; $fails++ }
    if (-not $okStop) { Write-Host "  FAIL: the old copy did not log a clean stop"; $fails++ }
    if (-not $okProxy) { Write-Host "  FAIL: the proxy did not log the reload"; $fails++ }
    if ($copies -ne 1) { Write-Host "  FAIL: expected exactly one bridge module at the end"; $fails++ }
    $noReports = -not (Get-ChildItem (Join-Path $t 'coop_crash_*.txt'), (Join-Path $t 'coop_hang_*.txt') -ErrorAction SilentlyContinue)
    Write-Host "  no crash/hang report in a normal run: $noReports"
    if (-not $noReports) { Write-Host "  FAIL: a report was written without a crash or hang"; $fails++ }
    Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue

    # crash report: a deliberate access violation in the exe that loaded the proxy
    $t = Join-Path $env:TEMP 'proxytest-crash'
    Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $t | Out-Null
    Copy-Item (Join-Path $proxy 'd3d9.dll') $t
    Copy-Item (Join-Path $proxy 'proxytest.exe') (Join-Path $t 'CoD2SP_s.exe')      # no bridge: the reports must not need it
    Write-Host "--- crash report (a deliberate access violation):"
    $p = Start-Process -FilePath (Join-Path $t 'CoD2SP_s.exe') -ArgumentList 'crash' -WorkingDirectory $t -PassThru -WindowStyle Hidden
    $null = $p.WaitForExit(30000)
    $rep = Get-ChildItem $t -Filter 'coop_crash_*.txt' | Select-Object -First 1
    if (-not $rep) { Write-Host "  FAIL: no crash report was written"; $fails++ }
    else {
        $txt = Get-Content $rep.FullName -Raw
        $checks = [ordered]@{
            'exception code'    = $txt -match 'EXCEPTION_ACCESS_VIOLATION'
            'write to 0'        = $txt -match 'WRITE to address 0x00000000'
            'module + offset'   = $txt -match 'CoD2SP_s\.exe\+0x[0-9A-F]+'
            'registers'         = $txt -match 'EIP=[0-9A-F]{8}'
            'module list'       = $txt -match 'Loaded modules'
            'no folder path'    = $txt -notmatch '[A-Za-z]:\\'
        }
        $checks.GetEnumerator() | ForEach-Object { Write-Host ("  {0,-18} {1}" -f $_.Key, $_.Value) }
        $txt -split "`n" | Select-Object -First 8 | ForEach-Object { Write-Host "  | $($_.TrimEnd())" }
        if ($checks.Values -contains $false) { $fails++; Write-Host "  FAIL: the crash report is incomplete" }
    }
    Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue

    # hang report: a window of class "CoD2" whose thread stops answering messages for ~14 s
    $t = Join-Path $env:TEMP 'proxytest-hang'
    Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue
    New-Item -ItemType Directory -Force $t | Out-Null
    Copy-Item (Join-Path $proxy 'd3d9.dll') $t
    Copy-Item (Join-Path $proxy 'proxytest.exe') (Join-Path $t 'CoD2SP_s.exe')
    Write-Host "--- hang report (the UI thread stops answering for ~14 s):"
    $p = Start-Process -FilePath (Join-Path $t 'CoD2SP_s.exe') -ArgumentList 'hang' -WorkingDirectory $t -PassThru -WindowStyle Hidden
    $null = $p.WaitForExit(60000)
    $reps = @(Get-ChildItem $t -Filter 'coop_hang_*.txt')
    if ($reps.Count -ne 1) { Write-Host "  FAIL: expected exactly one hang report, found $($reps.Count)"; $fails++ }
    else {
        $txt = Get-Content $reps[0].FullName -Raw
        $checks = [ordered]@{
            'hang text'        = $txt -match 'did not answer for about \d+ seconds'
            'UI thread stop'   = $txt -match 'UI thread \(id \d+\) stopped at'
            'stack scan'       = $txt -match 'CoD2SP_s\.exe\+0x[0-9A-F]+'
            'module list'      = $txt -match 'Loaded modules'
            'no folder path'   = $txt -notmatch '[A-Za-z]:\\'
        }
        $checks.GetEnumerator() | ForEach-Object { Write-Host ("  {0,-18} {1}" -f $_.Key, $_.Value) }
        if ($checks.Values -contains $false) { $fails++; Write-Host "  FAIL: the hang report is incomplete" }
    }
    Remove-Item $t -Recurse -Force -ErrorAction SilentlyContinue

    if ($fails) { throw "proxy test failed ($fails)" }
    Write-Host "proxy test passed"
}
