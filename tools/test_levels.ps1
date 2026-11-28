# test_levels.ps1 - load every playable level with the mod and report per level: did our publisher start (bridge log
# "script: coop_publish_state #0"), and any script compile/runtime error from main\console.log.
# Needs: the mod for the levels installed in the TEST copy, the test profile with `seta logfile "2"`, the game closed.
# It starts and closes the game itself (Steam URL with +map). ~1 minute per level.
#   powershell -ExecutionPolicy Bypass -File tools\test_levels.ps1 [-Levels cityhall,libya] [-Out result.txt]
param(
    [string[]]$Levels = @('88ridge','beltot','bergstein','breakout','cityhall','crossroads','decoytown','decoytrenches','demolition','downtown_assault','downtown_sniper','duhoc_assault','duhoc_defend','elalamein','eldaba','hill400_assault','hill400_defend','libya','matmata','moscow','newvillers','rhine','silotown_assault','tankhunt','toujane','toujane_ride','trainyard'),
    [string]$Out = 'test_levels_result.txt',
    [int]$WaitSeconds = 75
)
$G = 'C:\Program Files (x86)\Steam\steamapps\common\Call of Duty 2'
$log = Join-Path $G 'coop5.log'
$con = Join-Path $G 'main\console.log'

function Close-Game {
    $p = Get-Process CoD2SP_s -ErrorAction SilentlyContinue
    if ($p) { $p | ForEach-Object { $_.CloseMainWindow() | Out-Null; if (-not $_.WaitForExit(15000)) { $_.Kill() } } }
    Start-Sleep -Seconds 3
}

"level test started $(Get-Date -Format 'HH:mm:ss')" | Set-Content $Out
foreach ($lv in $Levels) {
    Close-Game
    $before = if (Test-Path $log) { (Get-Content $log | Measure-Object -Line).Lines } else { 0 }
    # direct start of the exe (the Steam URL makes Steam ask about custom launch options each time); steam_appid.txt in the folder lets it run
    Start-Process (Join-Path $G 'CoD2SP_s.exe') -ArgumentList "+map $lv" -WorkingDirectory $G
    $ok = $false; $t0 = Get-Date
    while (((Get-Date) - $t0).TotalSeconds -lt $WaitSeconds) {
        Start-Sleep -Seconds 3
        $new = Get-Content $log | Select-Object -Skip $before
        if ($new -match 'publish_state #0') { $ok = $true; break }
    }
    $extra = ''
    if ($ok) { Start-Sleep -Seconds 6 }          # let it run a little: runtime errors appear within seconds
    Close-Game
    $err = ''
    if (Test-Path $con) {
        $c = Get-Content $con -Raw
        $m = [regex]::Matches($c, '\*{7} script (compile|runtime) error \*{7}\r?\n([^\r\n]*)\r?\n([^\r\n]*)\r?\n([^\r\n]*)')
        if ($m.Count -gt 0) { $err = ($m | Select-Object -First 1).Value -replace '\r?\n', ' | ' }
    }
    $line = "{0,-18} publisher={1,-5} {2}" -f $lv, $ok, $err
    $line | Add-Content $Out
    Write-Host $line
}
"level test finished $(Get-Date -Format 'HH:mm:ss')" | Add-Content $Out
