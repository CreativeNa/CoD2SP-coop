# patch_menu.ps1 - build the menu override for the co-op mod from the game's OWN files (port of patch_menu.py, so no Python is needed on a player's PC).
#
#   . .\patch_menu.ps1 ; Invoke-PatchMenu -Game <GameDir> -Out <OutDir> [-MainOnly]
#
# Reads the stock ui/main.menu, ui/hud.menu and ui/menus.txt out of the game's own archives (main\iw_*.iwd only: a zz_coop archive that is already
# installed is never read back) and writes into <OutDir>\ui: main.menu (a CO-OP button), hud.menu (the partner's health next to the compass),
# menus.txt (+ loadMenu of ui/coop.menu) and coop.menu (launcher\menu\coop.menu, copied as it is).
# Every anchor is asserted: a stock file that is not what this expects fails loudly instead of producing a broken menu.

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem
$script:L1 = [Text.Encoding]::GetEncoding(28591)

function Read-StockMenu([string]$Game, [string]$Name) {
    $found = @()
    foreach ($f in (Get-ChildItem (Join-Path $Game 'main') -Filter 'iw_*.iwd' | Sort-Object Name)) {
        $z = [IO.Compression.ZipFile]::OpenRead($f.FullName)
        try {
            $e = $z.Entries | Where-Object { $_.FullName -eq $Name } | Select-Object -First 1
            if ($e) {
                $ms = New-Object IO.MemoryStream
                $s = $e.Open(); $s.CopyTo($ms); $s.Close()
                $found += $script:L1.GetString($ms.ToArray())
            }
        } finally { $z.Dispose() }
    }
    if ($found.Count -ne 1) { throw "expected $Name in exactly one stock archive, found $($found.Count)" }
    return $found[0]
}

function Get-BlockEnd([string]$t, [int]$open) {
    $depth = 0
    for ($i = $open; $i -lt $t.Length; $i++) {
        $c = $t[$i]
        if ($c -eq '{') { $depth++ }
        elseif ($c -eq '}') { $depth--; if ($depth -eq 0) { return $i } }
    }
    throw "unbalanced braces in the stock menu"
}

function Get-ItemSpans([string]$blk) {
    $spans = @(); $i = 0
    while ($true) {
        $j = $blk.IndexOf('itemDef', $i, [StringComparison]::Ordinal)
        if ($j -lt 0) { return ,$spans }
        $k = $blk.IndexOf('{', $j)
        $e = (Get-BlockEnd $blk $k) + 1
        $spans += ,@($j, $e)
        $i = $e
    }
}

function Get-ItemName([string]$it) {
    $m = [regex]::Match($it, 'name\s+"?([A-Za-z_0-9]+)')
    if ($m.Success) { return $m.Groups[1].Value } else { return '' }
}

function Update-MainMenu([string]$t, [bool]$ingame) {
    $m = [regex]::Match($t, 'name\s+main_text\b')
    if (-not $m.Success) { throw "menuDef main_text not found" }
    $ms = $t.LastIndexOf('menuDef', $m.Index, [StringComparison]::Ordinal)
    $me = (Get-BlockEnd $t $t.IndexOf('{', $ms)) + 1
    $blk = $t.Substring($ms, $me - $ms)
    $spans = Get-ItemSpans $blk

    $opts = @($spans | Where-Object { (Get-ItemName $blk.Substring($_[0], $_[1] - $_[0])) -eq 'options' })
    if ($opts.Count -ne 1) { throw "expected one 'options' button in main_text, found $($opts.Count)" }
    $s0 = $opts[0][0]; $e0 = $opts[0][1]
    $tmpl = $blk.Substring($s0, $e0 - $s0)
    $y = [regex]::Match($tmpl, '(rect\s+MAIN_RECT_X\s+)(\d+)(\s)')
    if (-not $y.Success) { throw "options button has no 'rect MAIN_RECT_X <y>'" }
    $baseY = [int]$y.Groups[2].Value

    $new = $tmpl
    $new = ([regex]'(name\s+)"?options"?').Replace($new, '${1}coop', 1)
    $new = ([regex]'(\btext\s+)"[^"]*"').Replace($new, '${1}"CO-OP"', 1)
    $actionRe = [regex]'action\s*\{[^}]*\}'
    if (-not $actionRe.IsMatch($new)) { throw "could not replace the action of the cloned button" }
    $act = "action`n`t`t`t{`n`t`t`t`tplay `"mouse_click`";`n`t`t`t`topen `"coop_menu`";`n`t`t`t}"
    $new = $actionRe.Replace($new, { param($mm) $act }, 1)
    if ($new.Contains('dvarTest')) { throw "the 'options' button already has a dvarTest: the clone would need a different rule" }
    if (-not $ingame) {
        $last = $new.LastIndexOf('}')
        $new = $new.Substring(0, $last) + "`t`tdvarTest `"sv_running`"`n`t`tshowDvar { `"0`" }`n`t}" + $new.Substring($last + 1)
    }

    $shifted = @()
    foreach ($sp in $spans) {
        $it = $blk.Substring($sp[0], $sp[1] - $sp[0])
        $mm = [regex]::Match($it, '(rect\s+MAIN_RECT_X\s+)(\d+)(\s)')
        $nm = Get-ItemName $it
        if ($mm.Success -and @('options', 'playselection', 'credit', 'quit') -contains $nm) {
            $g = $mm.Groups[2]
            $it = $it.Substring(0, $g.Index) + ([int]$g.Value + 30) + $it.Substring($g.Index + $g.Length)
        }
        $shifted += ,@($sp[0], $sp[1], $it)
    }
    $sb = New-Object Text.StringBuilder
    $pos = 0
    foreach ($x in $shifted) {
        [void]$sb.Append($blk.Substring($pos, $x[0] - $pos))
        if ($x[0] -eq $s0 -and $x[1] -eq $e0) { [void]$sb.Append($new + "`n`t`t") }
        [void]$sb.Append($x[2])
        $pos = $x[1]
    }
    [void]$sb.Append($blk.Substring($pos))
    $blk2 = $sb.ToString()
    $cnt = ([regex]::Matches($blk2, 'name\s+"?coop"?\s')).Count
    $openCnt = ([regex]::Matches($blk2, [regex]::Escape('open "coop_menu"'))).Count
    if ($cnt -ne 1 -or $openCnt -ne 1) { throw "the CO-OP button was not inserted exactly once" }
    return $t.Substring(0, $ms) + $blk2 + $t.Substring($me)
}

function Update-HudMenu([string]$t) {
    $m = [regex]::Match($t, 'name\s+"Compass"')
    if (-not $m.Success) { throw 'menuDef "Compass" not found in ui/hud.menu' }
    $ms = $t.LastIndexOf('menuDef', $m.Index, [StringComparison]::Ordinal)
    $me = Get-BlockEnd $t $t.IndexOf('{', $ms)
    function Item([string]$name, [string]$extra, [string]$rect) {
        return "`t`titemDef`n`t`t{`n`t`t`tname `"$name`"`n`t`t`trect $rect`n`t`t`ttextscale .32`n`t`t`ttextstyle ITEM_TEXTSTYLE_SHADOWED`n`t`t`ttextalign 2`n`t`t`ttextaligny 12`n`t`t`tdecoration`n`t`t`tvisible MENU_TRUE`n$extra`t`t}`n"
    }
    $on = "`t`t`tdvartest `"coop_partner_on`"`n`t`t`tshowdvar { `"1`" }`n"
    $label = Item 'coop_partner_label' ("`t`t`ttext `"@COOP_PARTNER`"`n`t`t`tforecolor .85 .85 .85 1`n" + $on) '150 112 70 14'
    $val = Item 'coop_partner_value' "`t`t`ttype ITEM_TYPE_TEXT`n`t`t`tdvar `"coop_partner`"`n`t`t`tforecolor 1 1 1 1`n`t`t`tdvartest `"coop_partner_low`"`n`t`t`tshowdvar { `"0`" }`n" '215 112 60 14'
    $low = Item 'coop_partner_low' "`t`t`ttype ITEM_TYPE_TEXT`n`t`t`tdvar `"coop_partner`"`n`t`t`tforecolor 1 .3 .3 1`n`t`t`tdvartest `"coop_partner_low`"`n`t`t`tshowdvar { `"1`" }`n" '215 112 60 14'
    return $t.Substring(0, $me) + "`n" + $label + $val + $low + "`t" + $t.Substring($me)
}

function Update-MenusTxt([string]$t) {
    $anchor = 'loadMenu { "ui/main.menu" }'
    $n = ([regex]::Matches($t, [regex]::Escape($anchor))).Count
    if ($n -ne 1) { throw "ui/menus.txt: the main.menu anchor is not unique" }
    return $t.Replace($anchor, $anchor + "`n`tloadMenu { `"ui/coop.menu`" }")
}

function Update-ConnectMenu([string]$t) {
    # the loading screen: the partner-wait line (the dvar coop_status holds the text, coop_wait = 1 turns it on)
    $m = [regex]::Match($t, 'name\s+"Connect"')
    if (-not $m.Success) { throw 'menuDef "Connect" not found in ui/connect.menu' }
    $ms = $t.LastIndexOf('menuDef', $m.Index, [StringComparison]::Ordinal)
    $me = Get-BlockEnd $t $t.IndexOf('{', $ms)
    $item = "`t`titemDef`n`t`t{`n`t`t`tname `"coop_wait`"`n`t`t`trect 0 395 640 24`n`t`t`ttype ITEM_TYPE_TEXT`n`t`t`tdvar `"coop_status`"`n`t`t`ttextscale .42`n`t`t`ttextstyle ITEM_TEXTSTYLE_SHADOWED`n`t`t`ttextalign ITEM_ALIGN_CENTER`n`t`t`ttextalignx 320`n`t`t`ttextaligny 18`n`t`t`tforecolor 1 .85 .2 1`n`t`t`tdecoration`n`t`t`tvisible MENU_TRUE`n`t`t`tdvartest `"coop_wait`"`n`t`t`tshowdvar { `"1`" }`n`t`t}`n"
    return $t.Substring(0, $me) + $item + "`t" + $t.Substring($me)
}

function Invoke-PatchMenu([string]$Game, [string]$Out, [bool]$MainOnly = $false, [string]$CoopMenu = '') {
    $ui = Join-Path $Out 'ui'
    New-Item -ItemType Directory -Force -Path $ui | Out-Null
    if (-not $CoopMenu) { $CoopMenu = Join-Path $PSScriptRoot '..\launcher\menu\coop.menu' }
    [IO.File]::WriteAllText((Join-Path $ui 'main.menu'), (Update-MainMenu (Read-StockMenu $Game 'ui/main.menu') (-not $MainOnly)), $script:L1)
    [IO.File]::WriteAllText((Join-Path $ui 'hud.menu'), (Update-HudMenu (Read-StockMenu $Game 'ui/hud.menu')), $script:L1)
    [IO.File]::WriteAllText((Join-Path $ui 'menus.txt'), (Update-MenusTxt (Read-StockMenu $Game 'ui/menus.txt')), $script:L1)
    # the version line in the bottom right corner: game version | mod version (the engine label is hidden so the
    # two numbers read as one line). The mod version comes from coop_mod_version.txt written by the installer.
    $ver = 'dev'
    $vf = Join-Path $Game 'coop_mod_version.txt'
    if (Test-Path $vf) { $ver = (Get-Content $vf -Raw).Trim() }
    $verLine = "1.2 | $ver"
    $cm = [IO.File]::ReadAllText($CoopMenu)
    $cm = $cm.Replace('__COOPVER__', $verLine)
    [IO.File]::WriteAllText((Join-Path $ui 'coop.menu'), $cm, $script:L1)
    $mmPath = Join-Path $ui 'main.menu'
    $mm = [IO.File]::ReadAllText($mmPath)
    $mm = $mm.Replace('setdvar ui_version_show "1";', 'setdvar ui_version_show "0";')
    $verItem = "`t`titemDef`n`t`t{`n`t`t`tname `"coop_verline`"`n`t`t`trect 590 470 155 12`n`t`t`ttype ITEM_TYPE_TEXT`n`t`t`ttext `"$verLine`"`n`t`t`ttextscale .22`n`t`t`ttextstyle ITEM_TEXTSTYLE_SHADOWED`n`t`t`ttextalign 2`n`t`t`ttextalignx 155`n`t`t`ttextaligny 10`n`t`t`tforecolor .7 .7 .65 1`n`t`t`tdecoration`n`t`t`tvisible 1`n`t`t}`n"
    # it must go into main_text: that is the menu that draws the list, and it is loaded after "main", so anything
    # put into "main" is covered by it. Insert at the end of the main_text block so it is drawn last.
    $first = $mm.IndexOf('menuDef')                      # menuDef "main"
    $mdOpen = $mm.IndexOf('menuDef', $first + 6)         # the next one is menuDef "main_text"
    $anchor = Get-BlockEnd $mm $mm.IndexOf('{', $mdOpen)
    [IO.File]::WriteAllText($mmPath, $mm.Substring(0, $anchor) + $verItem + "`t" + $mm.Substring($anchor), $script:L1)
    Write-Host "menu patched -> $Out"
}
