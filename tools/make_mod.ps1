# Build the probe mod for the TEST COPY only.
#
# How the hook works: the engine calls maps/<level>.gsc::main(). We take the original
# script from the game's own iwd, rename its main() to coop_orig_main(), and prepend a
# wrapper that starts a monitor thread and then calls the original. Mission logic is
# therefore untouched and executed exactly as before; our code only observes.
#
# Output: <test install>\mods\coop\z_coop.iwd  (zip with maps/moscow.gsc)
# Launch: CoD2SP_s.exe +set fs_game mods/coop +set logfile 2 +set developer 1 +map moscow

param(
    [string]$GameDir = "C:\Program Files (x86)\Steam\steamapps\common\Call of Duty 2",
    [string]$TestDir = "C:\Program Files (x86)\Steam\steamapps\common\Call of Duty 2 - coop-test",
    [string]$Level = "moscow",
    [ValidateSet('probe', 'puppet', 'follow', 'char', 'net', 'net2', 'net3', 'net4', 'net5', 'net6', 'net7')]
    [string]$Probe = 'puppet',
    [string]$Suffix = '',
    [switch]$NoBank2,         # dev: build WITHOUT the second body (maps/coop_anims2.gsc + animtrees/coop_puppet2.atr: the pistol poses on a second animation tree, see docs/BUGS.md)
    [switch]$Install,         # installer mode (a player's PC): no python, no dev leftovers; the archive is built into <TestDir>\main from the PLAYER'S OWN game files
    [switch]$MainMenuOnly,    # hide the CO-OP button in the pause menu (patch_menu.py --mainonly); by default it is shown there too
    [string[]]$Levels = @()   # extra levels to override with the same mod (all use the net7/net5 body); 'all' = every playable SP level
)

$ErrorActionPreference = 'Stop'
$Bank2 = -not $NoBank2          # the second body is part of the mod
Add-Type -AssemblyName System.IO.Compression.FileSystem
Add-Type -AssemblyName System.IO.Compression

function Get-FromIwd([string]$gameDir, [string]$entry) {
    foreach ($iwd in Get-ChildItem (Join-Path $gameDir 'main') -Filter 'iw_*.iwd') {   # stock archives only: an installed zz_coop archive is never read back
        $z = [IO.Compression.ZipFile]::OpenRead($iwd.FullName)
        try {
            $e = $z.Entries | Where-Object { $_.FullName -eq $entry } | Select-Object -First 1
            if ($e) {
                $sr = New-Object IO.StreamReader($e.Open())
                $t = $sr.ReadToEnd(); $sr.Close()
                return $t
            }
        } finally { $z.Dispose() }
    }
    throw "entry not found in any iwd: $entry"
}

$orig = Get-FromIwd $GameDir "maps/$Level.gsc"
Write-Host "original $Level.gsc : $($orig.Length) chars"

# Replace only the first 'main()' (the entry point) with a renamed copy.
$mm = [regex]::Match($orig, '(?m)^main\s*\(\s*\)')
if (-not $mm.Success) { throw "main() not found in $Level.gsc" }
$idx = $mm.Index
$patched = $orig.Remove($idx, $mm.Length).Insert($idx, 'coop_orig_main()')
Write-Host "renamed entry point at offset $idx"

$probeBody = @"

// ============================================================
// cod2-coop probe hook (built $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
// Observes only: prints the local player origin/angles/health every 0.5 s to the
// console log via println. (iprintlnbold is not used: the engine warns about
// non-localized game message strings and it spams the screen.)
// ============================================================
main()
{
    thread coop_probe();
    coop_orig_main();
}

coop_probe()
{
    while (!isdefined(level.player))
        wait 0.25;

    p = level.player;
    i = 0;
    while (i < 240)
    {
        org = p.origin;
        ang = p.angles;
        line = "COOPPROBE n=" + i + " x=" + org[0] + " y=" + org[1] + " z=" + org[2] + " yaw=" + ang[1] + " hp=" + p.health;
        iprintln(line);
        i = i + 1;
        wait 0.5;
    }
    iprintln("COOPPROBE done");
}
"@

# v2: the puppet. Everything here uses functions that the game's own scripts already
# call (checked against the shipped .gsc set on 2026-11-13):
#   spawn("script_model", org)  - 60 scripts      setModel("xmodel/...") - 87
#   anglestoforward(angles)     - 120            precacheModel          - 168
#   hide()                      - 41             delete()               - 69
# The model is precached in main(), before the original entry point, because the engine
# only accepts precache during level load.
$puppetBody = @"

// ============================================================
// cod2-coop puppet hook (built $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
// Stage 1 of the vertical slice: create a second body in the level, move it, remove it,
// logging every step. No networking yet - the point is that the engine accepts a
// script_model standing in for player 2.
// ============================================================
main()
{
    precacheModel("xmodel/playerbody_russian_padded01");
    thread coop_puppet();
    coop_orig_main();
}

coop_puppet()
{
    while (!isdefined(level.player))
        wait 0.1;

    p = level.player;
    wait 2.0;

    org = p.origin;
    ang = p.angles;
    fwd = anglestoforward(ang);
    px = org[0] + fwd[0] * 64;
    py = org[1] + fwd[1] * 64;
    pz = org[2];
    iprintln("COOP2 spawn-request x=" + px + " y=" + py + " z=" + pz);

    puppet = spawn("script_model", (px, py, pz));
    if (!isdefined(puppet))
    {
        iprintln("COOP2 ERROR spawn returned undefined");
        return;
    }
    puppet setModel("xmodel/playerbody_russian_padded01");
    por = puppet.origin;
    iprintln("COOP2 spawned puppet x=" + por[0] + " y=" + por[1] + " z=" + por[2]);

    i = 0;
    while (i < 30)
    {
        nx = px + i * 8;
        puppet setOrigin((nx, py, pz));
        por = puppet.origin;
        org = p.origin;
        iprintln("COOP2 move n=" + i + " puppet=" + por[0] + " " + por[1] + " " + por[2] + " player=" + org[0] + " " + org[1] + " " + org[2]);
        i = i + 1;
        wait 0.5;
    }

    puppet hide();
    iprintln("COOP2 hidden");
    wait 1.0;
    puppet delete();
    iprintln("COOP2 deleted, done");
}
"@

# v3: a puppet that follows the player. `setOrigin` is a player-only command in CoD2
# ("entity N is not a player" - runtime error, session 1), so the position is written as
# an entity field instead, the way the shipped scripts move script_models around.
$followBody = @"

// ============================================================
// cod2-coop puppet v3 (built $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
// Spawns a second soldier beside the player and makes it follow him, logging both
// origins. Position is written with 'puppet.origin = (...)' because the setOrigin
// script command is restricted to player entities in this build.
// ============================================================
main()
{
    precacheModel("xmodel/playerbody_russian_padded01");
    thread coop_follow();
    coop_orig_main();
}

coop_follow()
{
    while (!isdefined(level.player))
        wait 0.1;

    p = level.player;
    wait 2.0;

    org = p.origin;
    ang = p.angles;
    fwd = anglestoforward(ang);
    rx = fwd[1];
    ry = 0 - fwd[0];
    px = org[0] + rx * 40;
    py = org[1] + ry * 40;
    pz = org[2];

    puppet = spawn("script_model", (px, py, pz));
    if (!isdefined(puppet))
    {
        iprintln("COOP3 ERROR spawn returned undefined");
        return;
    }
    puppet setModel("xmodel/playerbody_russian_padded01");
    por = puppet.origin;
    iprintln("COOP3 spawned puppet=" + por[0] + " " + por[1] + " " + por[2]);

    i = 0;
    while (i < 300)
    {
        org = p.origin;
        ang = p.angles;
        fwd = anglestoforward(ang);
        rx = fwd[1];
        ry = 0 - fwd[0];
        nx = org[0] + rx * 40;
        ny = org[1] + ry * 40;
        nz = org[2];
        puppet.origin = (nx, ny, nz);
        por = puppet.origin;
        iprintln("COOP3 n=" + i + " puppet=" + por[0] + " " + por[1] + " " + por[2] + " player=" + org[0] + " " + org[1] + " " + org[2]);
        i = i + 1;
        wait 0.2;
    }
    iprintln("COOP3 loop finished");
}
"@

# v5: make the puppet look like a soldier. A bare script_model renders the body mesh in
# bind pose with no head, because the head is a separate model attached to tag_head and
# the animation comes from the AI/animation tree. makeFakeAI() is documented by the
# engine's own error string ("makeFakeAI must be applied to a script_model"), and heads
# are attached exactly as the shipped character scripts do: attach(model, tag).
# Each step is logged BEFORE it runs, so the log pinpoints the call that gets rejected.
$charBody = @"

// ============================================================
// cod2-coop puppet v8 (built $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
// body + head + makeFakeAI, then follow the player.
// ============================================================
main()
{
    precacheModel("xmodel/playerbody_russian_padded01");
    precacheModel("xmodel/head_russian_winter_volsky");
    thread coop_char();
    coop_orig_main();
}

coop_char()
{
    while (!isdefined(level.player))
        wait 0.1;

    p = level.player;
    wait 2.0;

    org = p.origin;
    ang = p.angles;
    fwd = anglestoforward(ang);
    rx = fwd[1];
    ry = 0 - fwd[0];
    px = org[0] - fwd[0] * 96;
    py = org[1] - fwd[1] * 96;
    pz = org[2];

    puppet = spawn("script_model", (px, py, pz));
    if (!isdefined(puppet))
    {
        iprintln("COOP8 ERROR spawn returned undefined");
        return;
    }
    iprintln("COOP8 step1 spawned");

    puppet setModel("xmodel/playerbody_russian_padded01");
    iprintln("COOP8 step2 body model set");

    puppet attach("xmodel/head_russian_winter_volsky", "", true);
    iprintln("COOP8 step3 head attached");

    puppet makeFakeAI();
    iprintln("COOP8 step4 makeFakeAI ok");

    i = 0;
    while (i < 300)
    {
        org = p.origin;
        ang = p.angles;
        fwd = anglestoforward(ang);
        rx = fwd[1];
        ry = 0 - fwd[0];
        nx = org[0] + rx * 40;
        ny = org[1] + ry * 40;
        nz = org[2];
        puppet.origin = (nx, ny, nz);
        por = puppet.origin;
        if (i < 3)
            iprintln("COOP8 follow n=" + i + " puppet=" + por[0] + " " + por[1] + " " + por[2] + " player=" + org[0] + " " + org[1] + " " + org[2]);
        i = i + 1;
        wait 0.2;
    }
    iprintln("COOP8 loop finished");
}
"@

$netBody = @"

// ============================================================
// cod2-coop net hook (built $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
// This is the variant that was MISSING from the repo while B1 was open: the build that
// reads coop_px/py/pz from the bridge. One build separates the two candidate causes of B1
// and fixes B3 at the same time:
//   * it creates the cvars the bridge writes into AND logs what getcvarfloat() returns,
//     so "the script never sees the value" is distinguishable from "the value arrives but
//     the model does not move";
//   * it moves the puppet with moveTo() - the engine's own mover, used by 109 shipped
//     script calls - instead of assigning .origin;
//   * it takes z from the player, not from the packet, so the puppet stands on the ground.
// Acceptance: a COOPN line whose "puppet=" part changes over time while "read=" matches
// what the bridge wrote.
// ============================================================
main()
{
    precacheModel("xmodel/playerbody_russian_padded01");
    precacheModel("xmodel/head_russian_winter_volsky");
    thread coop_net();
    coop_orig_main();
}

coop_net()
{
    // created here, at level start, so the bridge can resolve them by name text
    setcvar("coop_px", "0.000000000000000000");
    setcvar("coop_py", "0.000000000000000000");
    setcvar("coop_pz", "0.000000000000000000");
    setcvar("coop_cal", "1.000000000000000000");

    while (!isdefined(level.player))
        wait 0.1;

    p = level.player;
    wait 2.0;

    puppet = spawn("script_model", p.origin);
    if (!isdefined(puppet))
    {
        iprintln("COOPN ERROR spawn returned undefined");
        return;
    }
    puppet setModel("xmodel/playerbody_russian_padded01");
    puppet attach("xmodel/head_russian_winter_volsky", "", true);
    puppet makeFakeAI();
    por = puppet.origin;
    iprintln("COOPN spawn puppet=" + por[0] + " " + por[1] + " " + por[2]);

    i = 0;
    k = 0;
    while (i < 1200)
    {
        rx = getcvarfloat("coop_px");
        ry = getcvarfloat("coop_py");
        rz = getcvarfloat("coop_pz");
        po = p.origin;

        if (rx != 0 || ry != 0)
            puppet moveTo((rx, ry, po[2]), 0.2);

        por = puppet.origin;

        k = k + 1;
        if (k >= 10)
        {
            iprintln("COOPN n=" + i + " read=" + rx + " " + ry + " " + rz + " puppet=" + por[0] + " " + por[1] + " " + por[2] + " player=" + po[0] + " " + po[1] + " " + po[2]);
            k = 0;
        }
        i = i + 1;
        wait 0.2;
    }
    iprintln("COOPN loop finished");
}
"@

$net2Body = @"

// ============================================================
// cod2-coop net hook v2 (built $(Get-Date -Format 'yyyy-MM-dd HH:mm:ss'))
// Builds on the working net variant (B1 fixed: moveTo instead of .origin). Adds:
//   * the engine's own character setup (B2): character\...::main() then makeFakeAI(),
//     exactly the order the shipped drone_*_spawn() uses. Gives model + head + helmet
//     and the animation tree, instead of a bare script_model in bind pose;
//   * ground height (B3): bulletTrace down from above the target to below the player,
//     so the soldier stands on the terrain instead of inheriting the player's z;
//   * removal: no new packet for ~30 s and the puppet deletes itself (slice step 3);
//   * a readback channel: what getcvarfloat() actually returned is written into
//     coop_rx/coop_ry/coop_rz, which the bridge logs - acceptance without a human eye.
// ============================================================
main()
{
    character\russian_winter_trench_volskey::precache();
    thread coop_net2();
    coop_orig_main();
}

coop_net2()
{
    // Padded on purpose: the bridge sprintf()s a formatted float into these strings in place,
    // so the allocation must be longer than any value it will hold ("-2047.462" is 9 chars).
    // "0.000" is too short and corrupts the heap.
    setcvar("coop_px", "0.000000000000000000000000000000");
    setcvar("coop_py", "0.000000000000000000000000000000");
    setcvar("coop_pz", "0.000000000000000000000000000000");
    setcvar("coop_cal", "1.000000000000000000000000000000");
    setcvar("coop_rx", "none");
    setcvar("coop_ry", "none");
    setcvar("coop_rz", "none");
    // try to get a real log file; if the engine only opens it at startup this is a no-op
    setcvar("logfile", "2");

    while (!isdefined(level.player))
        wait 0.1;

    if (getcvar("mapname") == "libya" || getcvar("mapname") == "88ridge")
        return;
    p = level.player;
    wait 2.0;

    // Wait for the first packet before spawning. The earlier build spawned immediately and then
    // deleted the soldier after 30 s of silence - which is exactly what happens while the bridge
    // has not been injected yet, so the puppet vanished before it was ever driven.
    rx = getcvarfloat("coop_px");
    ry = getcvarfloat("coop_py");
    i = 0;
    while ((rx == 0 && ry == 0) && i < 3000)
    {
        rx = getcvarfloat("coop_px");
        ry = getcvarfloat("coop_py");
        i = i + 1;
        wait 0.2;
    }
    if (rx == 0 && ry == 0)
    {
        iprintln("COOPN2 no packets within 10 minutes, not spawning");
        return;
    }

    po = p.origin;
    tr = bulletTrace((rx, ry, po[2] + 512), (rx, ry, po[2] - 1024), false, undefined);
    gz = po[2];
    if (tr["fraction"] < 1)
        gz = tr["position"][2];

    puppet = spawn("script_model", (rx, ry, gz));
    if (!isdefined(puppet))
    {
        iprintln("COOPN2 ERROR spawn returned undefined");
        return;
    }
    puppet character\russian_winter_trench_volskey::main();
    puppet makeFakeAI();
    por = puppet.origin;
    iprintln("COOPN2 spawn puppet=" + por[0] + " " + por[1] + " " + por[2]);
    setcvar("coop_rx", "spawned");

    lastx = rx;
    lasty = ry;
    still = 0;
    i = 0;
    k = 0;
    while (i < 3000)
    {
        rx = getcvarfloat("coop_px");
        ry = getcvarfloat("coop_py");

        if (rx != lastx || ry != lasty)
        {
            po = p.origin;
            tr = bulletTrace((rx, ry, po[2] + 512), (rx, ry, po[2] - 1024), false, undefined);
            gz = po[2];
            if (tr["fraction"] < 1)
                gz = tr["position"][2];

            puppet moveTo((rx, ry, gz), 0.2);
            lastx = rx;
            lasty = ry;
            still = 0;
        }
        else
        {
            still = still + 1;
        }

        // removal is part of the slice, but only once the peer has actually moved it
        if (still > 150)
        {
            setcvar("coop_rx", "removing");
            puppet hide();
            wait 0.5;
            puppet delete();
            setcvar("coop_rx", "removed");
            iprintln("COOPN2 removed after 30 s without packets");
            return;
        }

        k = k + 1;
        if (k >= 10)
        {
            // string copy, not a float concatenation: setcvar("x", "" + float) stores an empty
            // string in this engine, which is why the earlier readback channel was always ''
            setcvar("coop_rx", getcvar("coop_px"));
            por = puppet.origin;
            iprintln("COOPN2 n=" + i + " read=" + rx + " " + ry + " puppet=" + por[0] + " " + por[1] + " " + por[2]);
            k = 0;
        }
        i = i + 1;
        wait 0.2;
    }
    iprintln("COOPN2 loop finished");
}
"@

# net3 = net2 + idle animation (B2). Recipe copied from the shipped maps\_drones.gsc: bind the
# "fakeshooters" tree, useAnimTree(#animtree), loop a flagged idle knob. setFlaggedAnimKnob does not
# touch the origin, so moveTo keeps driving the puppet. Risk: a bad identifier fails the level load.
$net3Body = $net2Body -replace '(?m)^coop_net2\(\)', "#using_animtree(`"fakeshooters`");`ncoop_net2()"
$net3Body = $net3Body -replace '(puppet makeFakeAI\(\);)', "`$1`n    puppet useAnimTree(#animtree);`n    puppet thread coop_idle();"
$net3Body += @"

coop_idle()
{
    self endon("death");
    while (isdefined(self))
    {
        self setFlaggedAnimKnob("coop_idle", %stand_alert_1);
        self waittillmatch("coop_idle", "end");
    }
}
"@

# net4 = net3 with a live pose selector: the GSC polls the cvar coop_anim (type `set coop_anim 3` in
# the console) and restarts the matching animation, so poses can be compared without rebuilding or
# restarting the game. Anim identifiers are fixed at compile time, hence the list below.
#   0-2 stand_alert_1..3   3 stand_aim_straight   4 crouch_aim_straight   5 stand_shoot_auto_straight
#   6 stand_shoot_straight 7 crouch_shoot_auto_straight   8 reload_stand_rifle
$net4Body = $net2Body -replace '(?m)^coop_net2\(\)', "#using_animtree(`"coop_puppet`");`ncoop_net2()"
$net4Body = $net4Body -replace '(puppet makeFakeAI\(\);)', "`$1`n    puppet useAnimTree(#animtree);`n    puppet thread coop_pose();"
# weapon: the aim poses put the hands on a rifle, but a bare script_model has none. Same call as the
# shipped drone_allies_assignWeapon (russian campaign: "ppsh" or "SVT40"); precached in main().
$net4Body = $net4Body -replace '(puppet thread coop_pose\(\);)', "`$1`n    puppet attach(getWeaponModel(`"ppsh`"), `"tag_weapon_right`");"
$net4Body = $net4Body -replace '(character\\russian_winter_trench_volskey::precache\(\);)', "`$1`n    precacheItem(`"ppsh`");"
# the in-loop ground trace must ignore the puppet itself, or it hits the puppet's head (the "elevator")
$net4Body = $net4Body -replace '(?m)^(\s{12}tr = bulletTrace\(.*?), false, undefined\);', '$1, false, puppet);'
$net4Body = $net4Body -replace '(setcvar\("coop_rz", "none"\);)', "`$1`n    setcvar(`"coop_anim`", `"0000000001`");"   # default pose 1 (calm idle)
$net4Body += @"

coop_pose()
{
    self endon("death");
    cur = -1;
    while (isdefined(self))
    {
        a = getcvarint("coop_anim");
        if (a != cur)
        {
            cur = a;
            if (a == 1)      self setFlaggedAnimKnobRestart("coop_pose", %stand_alert_2, 1, 0.2, 1);
            else if (a == 2) self setFlaggedAnimKnobRestart("coop_pose", %stand_alert_2, 1, 0.2, 1);
            else if (a == 3) self setFlaggedAnimKnobRestart("coop_pose", %stand_aim_straight, 1, 0.2, 1);
            else if (a == 4) self setFlaggedAnimKnobRestart("coop_pose", %crouch_aim_straight, 1, 0.2, 1);
            else if (a == 5) self setFlaggedAnimKnobRestart("coop_pose", %stand_shoot_auto_straight, 1, 0.2, 1);
            else if (a == 6) self setFlaggedAnimKnobRestart("coop_pose", %stand_shoot_straight, 1, 0.2, 1);
            else if (a == 7) self setFlaggedAnimKnobRestart("coop_pose", %crouch_shoot_auto_straight, 1, 0.2, 1);
            else if (a == 8) self setFlaggedAnimKnobRestart("coop_pose", %reload_stand_rifle, 1, 0.2, 1);
            else             self setFlaggedAnimKnobRestart("coop_pose", %stand_alert_1, 1, 0.2, 1);
            iprintln("COOPN4 pose " + a);
        }
        wait 0.2;
    }
}
"@

# net5 = net4 + the local player's state published for the bridge to send to the peer, and the peer's
# yaw applied to the puppet. A GSC thread writes coop_lx/ly/lz/lyaw every 0.1 s as "p" + float (a bare
# float concatenated to "" gives an empty string in this engine; the bridge strips the p). The bridge
# sends them over UDP to the address in coop_peer.txt; the peer's bridge writes coop_px/py/pz/pyaw.
# The 30 s "no movement" removal is turned off: a peer standing still must not lose its puppet.
# (A real disconnect would need a heartbeat; not built yet.)
$net5Body = $net4Body
$net5Body = $net5Body -replace '(setcvar\("coop_anim", "0000000001"\);)', "`$1`n    setcvar(`"coop_pyaw`", `"0.000000000000000000000000000000`");"
$net5Body = $net5Body -replace '(?m)^(\s*)(// removal is part of the slice)', "`$1puppet rotateTo((0, getcvarfloat(`"coop_pyaw`"), 0), 0.2);`n`$1`$2"
$net5Body = $net5Body -replace 'still > 150', 'still > 99999999'
$net5Body = $net5Body -replace '(thread coop_net2\(\);)', "`$1`n    thread coop_publish();"
# not solid: the puppet used to block the real player (a peer standing in a doorway locked the other
# player in, as the test bot did). setcontents(0) is what the shipped duhoc_assault does for its
# makeFakeAI actors; players then walk through each other. Side effect: bullets pass through it too.
$net5Body = $net5Body -replace '(puppet attach\(getWeaponModel\("ppsh"\), "tag_weapon_right"\);)', "`$1`n    puppet setcontents(0);"

# latency: the puppet loop polled every 0.2 s and glided over 0.2 s, the publisher wrote every 0.1 s
# (about 0.6 s end to end). 0.05 s is one server frame. The main loop was capped at 3000 iterations
# (10 min at 0.2 s), which would now be 2.5 min, so the cap goes too.
$net5Body = $net5Body -replace '(?m)(i = i \+ 1;\r?\n\s*)wait 0\.2;', '${1}wait 0.05;'
$net5Body = $net5Body -replace 'puppet moveTo\(\(rx, ry, gz\), 0\.2\);', 'puppet moveTo((rx, ry, gz), 0.1);'
$net5Body = $net5Body -replace '(getcvarfloat\("coop_pyaw"\), 0\), )0\.2\);', '${1}0.1);'
$net5Body = $net5Body -replace 'while \(i < 3000\)', 'while (i < 100000000)'
$net5Body = $net5Body -replace '\(rx == 0 && ry == 0\) && i < 3000\)', '(rx == 0 && ry == 0) && i < 100000000)'   # also waits 0.05 now: keep "wait for the peer" unbounded
# acceptance by log: coop_rx carries the PUPPET's real origin (net4 copied the coop_px string, which
# only proved the cvar path), once a second; the on-screen line every 20 iterations is enough
$net5Body = $net5Body -replace 'setcvar\("coop_rx", getcvar\("coop_px"\)\);(\r?\n\s*)por = puppet\.origin;', 'por = puppet.origin;${1}setcvar("coop_rx", "p" + por[0] + " " + por[1] + " " + por[2]);'
$net5Body = $net5Body -replace 'if \(k >= 10\)', 'if (k >= 20)'
$net5Body = $net5Body -replace '(?m)^(\s*)iprintln\("COOPN2 n=" \+ i[^\r\n]*\r?\n', ''
$net5Body = $net5Body -replace '(?m)^(\s*)iprintln\("COOPN2 spawn puppet=[^\r\n]*\r?\n', ''
$net5Body += @"

coop_publish()
{
    setcvar("coop_lx", "p0.000000");
    setcvar("coop_ly", "p0.000000");
    setcvar("coop_lz", "p0.000000");
    setcvar("coop_lyaw", "p0.000000");
    while (!isdefined(level.player))
        wait 0.1;
    for (;;)
    {
        po = level.player.origin;
        an = level.player getplayerangles();
        setcvar("coop_lx", "p" + po[0]);
        setcvar("coop_ly", "p" + po[1]);
        setcvar("coop_lz", "p" + po[2]);
        setcvar("coop_lyaw", "p" + an[1]);
        wait 0.05;
    }
}
"@

# net6 = net5 + a call to the bridge's own GSC built-ins (proxy hooks the engine's name lookup; see docs/ENGINE.md). A PROBE, not the release:
# the compiler resolves the names when the script is compiled, so this mod REFUSES to load (script error: unknown function) unless
# the d3d9 proxy has installed its hooks. net5 stays the release until the hooks are proven in the game.
$net6Body = $net5Body -replace 'thread coop_publish\(\);', 'thread coop_chan_publish();'
$net6Body = $net6Body -replace '(setcvar\("coop_anim", "0000000001"\);)', "`$1`n    probe_v = coop_version();`n    probe_p = coop_ping(41);`n    iprintln(`"COOPN6 bridge=`" + probe_v + `" ping(41)=`" + probe_p);`n    thread coop_probe_loop();"
# a call every 5 s for the whole mission: replacing the bridge file mid-mission must keep reaching the NEW bridge through the proxy's stub
$net6Body += @"

coop_chan_publish()
{
    while (!isdefined(level.player))
        wait 0.1;
    for (;;)
    {
        an = level.player getplayerangles();
        coop_publish_state(level.player.origin, an[1]);
        wait 0.05;
    }
}

coop_probe_loop()
{
    i = 100;
    for (;;)
    {
        wait 5;
        r = coop_ping(i);
        i = i + 1;
        r = coop_ping(coop_peer_seq());
        d = distance(coop_peer_state(), level.player.origin);
        r = coop_ping(int(d));
    }
}
"@

# net7 = net5 + the script channel, NO probe calls (the release candidate): the local player is published by coop_publish_state and the puppet reads the peer through the built-ins (coop_peer_state / coop_peer_yaw) instead of getcvarfloat on strings the
# bridge wrote. Same puppet code, only the three reads change. Needs the proxy hooks (like net6).
$net7Body = $net5Body -replace 'thread coop_publish\(\);', 'thread coop_chan_publish();'
$net7Body = $net7Body -replace '(?m)^(\s*)coop_orig_main\(\);', "`$1coop_apply_host_skill();`n`$1coop_barrier_now();`n`$1setcvar(`"coop_ver`", `"1.3 | `" + coop_modver());`n`$1coop_orig_main();"
# the puppet wears the soldier of the campaign of the map (the player looks different from map to map): done BEFORE the new functions are appended
$net7Body = $net7Body -replace 'puppet character\\russian_winter_trench_volskey::main\(\);', 'coop_dress(puppet);'
$net7Body = $net7Body -replace 'character\\russian_winter_trench_volskey::precache\(\);', 'coop_precache_character();'
$net7Body += @"

coop_grenade_count()
{
    g = getentarray("grenade", "classname");
    n = 0;
    for (i = 0; i < g.size; i++)
    {
        if (distance(g[i].origin, level.player.origin) < 160)
            n = n + 1;
    }
    return n;
}

coop_watch_grenade()
{
    for (;;)
    {
        level.player waittill("grenade_fire", gr, wn);
        level.coop_evc = level.coop_evc + 1;
        level.coop_evt = level.coop_evc * 8 + 2;
    }
}

// a grenade of the local player appeared next to him (it left the hand; a held/cooked grenade appears only when thrown): the throw event for the puppet's
// animation, its start (relative to him), its velocity (two samples 0.05 s apart) and its kind go to the partner. The notify "grenade_fire" never came here.
coop_grenade_watch()
{
    while (!isdefined(level.player))
        wait 0.1;
    for (;;)
    {
        wait 0.05;
        g = getentarray("grenade", "classname");
        for (i = 0; i < g.size; i++)
        {
            if (isdefined(g[i].coop_seen))
                continue;
            g[i].coop_seen = true;
            if (distance(g[i].origin, level.player.origin) > 150)
                continue;
            level.coop_evc = level.coop_evc + 1;
            level.coop_evt = level.coop_evc * 8 + 2;
            thread coop_grenade_track(g[i]);
        }
    }
}

coop_grenade_track(best)
{
    pl = level.player.origin;
    o0 = best.origin;
    kind = 0;
    if (isdefined(best.model))
    {
        mn = tolower(best.model);
        for (k = 0; k + 5 <= mn.size; k++)
        {
            if (getsubstr(mn, k, k + 5) == "smoke")
                kind = 1;
        }
    }
    vel = (0, 0, 0);
    for (t = 0; t < 8; t++)
    {
        wait 0.05;
        if (!isdefined(best))
            return;
        o1 = best.origin;
        vel = ((o1[0] - o0[0]) * 20, (o1[1] - o0[1]) * 20, (o1[2] - o0[2]) * 20);
        if (length(vel) > 80)
            break;
        o0 = o1;
    }
    if (length(vel) <= 80)
        return;
    k = coop_char_kind();
    gw = "";
    if (isdefined(best.model))
    {
        gm = tolower(best.model);
        for (q = 0; q + 6 <= gm.size; q++)
        {
            s6 = getsubstr(gm, q, q + 6);
            if (s6 == "potato")
                gw = "potato";
            if (s6 == "german")
                gw = "stielhandgranate";
            if (s6 == "russia")
                gw = "rgd-33russianfrag";
            if (s6 == "britis")
                gw = "mk1britishfrag";
            if (s6 == "mk2fra")
                gw = "fraggrenade";
        }
    }
    if (kind == 1)
    {
        wn = "smoke_grenade_american";
        if (k == "british_a" || k == "british_n")
            wn = "smoke_grenade_british";
        else if (k == "russian")
            wn = "smoke_grenade_russian";
    }
    else
    {
        wn = "fraggrenade";
        if (k == "british_a" || k == "british_n")
            wn = "mk1britishfrag";
        else if (k == "russian")
            wn = "rgd-33russianfrag";
        if (gw != "")
            wn = gw;
    }
    coop_send_grenade(wn, (o0[0] - pl[0], o0[1] - pl[1], o0[2] - pl[2]), vel);
}

// a grenade of the partner: the engine's own projectile is tried first (magicbullet with the grenade weapon)
coop_grenade_recv()
{
    while (!isdefined(level.player))
        wait 0.1;
    for (;;)
    {
        wait 0.05;
        gn = coop_pop_grenade();
        if (gn == "")
            continue;
        go = coop_grenade_org();
        gv = coop_grenade_vel();
        base = level.coop_puppet_origin;
        if (!isdefined(base))
            continue;
        st = (base[0] + go[0], base[1] + go[1], base[2] + go[2]);
        thread coop_fake_grenade(gn, st, gv);
    }
}

// libya: both players drive a tank of their own. The first one is the level's (the host sits in it), the guest sits in a second one spawned here.
// The partner's tank is a plain model that follows his reports (a vehicle entity cannot be moved from script: .origin is ignored and an empty one sinks).
coop_tank_main()
{
    if (getcvar("mapname") != "libya" && getcvar("mapname") != "88ridge")      // the two missions where the player drives a tank of his own
        return;
    // as soon as the level has seated the player in its tank (about 3 s after the start), not after a fixed long wait
    pa = undefined;
    for (n = 0; n < 150 && !isdefined(pa); n++)
    {
        wait 0.1;
        if (isdefined(level.playervehicle) && isdefined(level.playervehicle.classname) && level.playervehicle.classname == "script_vehicle")
            pa = level.playervehicle;
    }
    if (!isdefined(pa))
        return;
    // the level's own set-up threads (playerturretsetup, apply_player_rumble: "wait 1", then they read level.playervehicle) must run first:
    // a swap before that leaves them with no player vehicle (thousands of script runtime errors on the guest PC, 0.9.25-0.9.27)
    wait 1.8;
    guest = (coop_is_guest() == 1 || getcvar("coop_tank_guest") == "1");
    mine = pa;
    if (guest)
    {
        r = anglestoright(pa.angles);
        o = (pa.origin[0] + r[0] * 400, pa.origin[1] + r[1] * 400, pa.origin[2] + 20);
        pb = spawnvehicle(pa.model, "coop_tank", "crusader_player", o, pa.angles);
        if (!isdefined(pb))
            return;
        // what the level's own vehicle setup does for a vehicle placed in the map
        pb.vehicletype = "crusader_player";
        pb.simpletype = "tank";
        pb thread [[level.vehicleInitThread["crusader_player"][pa.model]]]();
        pb maps\_vehicledrive::setup_vehicle_tank();
        // no AI aiming/firing (the engine ignores a spawnflags assignment): the attack loops end when these two are false
        pb.script_turret = false;
        pb.script_attackai = false;
        wait 0.5;
        // the same MG set-up as the level's own playerturretsetup(): the player's MG, no second one
        newarray = [];
        for (i = 0; i < pb.mgturret.size; i++)
        {
            pb.mgturret[i].isplayerturret = true;
            if (i == 1)
                pb.mgturret[i] delete();
            else
                newarray[newarray.size] = pb.mgturret[i];
        }
        pb.mgturret = newarray;
        mine = pb;
        pa useby(level.player);
        for (k = 0; k < 30 && isdefined(pa getvehicleowner()); k++)
            wait 0.1;
        pa maps\_vehicledrive::vehicle_exit();
        mine thread maps\_vehicledrive::vehicle_wait(true);
        wait 0.3;
        // the entry above reads the owner at once: on a slow PC the seat is not taken yet and it then treats it as an exit. Make sure.
        for (k = 0; k < 20; k++)
        {
            ow = mine getvehicleowner();
            if (isdefined(ow) && ow == level.player)
                break;
            mine useby(level.player);
            wait 0.3;
        }
        if (!isdefined(level.playervehicle) || level.playervehicle != mine)
        {
            level.playervehicle = mine;
            level.playeronvehicle = true;
            mine thread maps\_vehicledrive::vehicle_enter();
        }
        iprintln("COOPT seated after " + k + " retries");
        // the level's own tank is not used any more: the partner's model stands in its place
        pa hide();
    }
    // The copy of the partner tank is a static model with the detailed Crusader mesh: a real script_vehicle
    // looked wrong (blocky model, spawned inside the player, fell through the ground) and its engine-driven
    // tracks could not make up for that.
    ghost = spawn("script_model", (pa.origin[0] + 600, pa.origin[1], pa.origin[2]));
    ghost setmodel("xmodel/vehicle_crusader2_static");
    thread coop_tank_publish(mine);
    thread coop_tank_follow(ghost);
    iprintln("COOPT tanks ready, guest=" + guest);
}

coop_tank_publish(tank)
{
    tank.coop_fires = 0;
    tank thread coop_tank_count_fire();
    for (;;)
    {
        wait 0.05;
        if (!isdefined(tank))
            return;
    coop_send_tank(tank.origin, tank.angles, level.player.angles, tank.coop_fires);   // the view angles are the turret direction in a tank (tag_turret does not exist on this model)
    }
}

coop_tank_count_fire()
{
    self endon("death");
    for (;;)
    {
        self waittill("turret_fire");
        self.coop_fires = self.coop_fires + 1;
    }
}

coop_tank_follow(ghost)
{
    base = ghost.origin;
    baseang = ghost.angles;
    ghost hide();
    lastfire = coop_tank_fire();
    shown = 0;
    spd = 0;
    lp = undefined;
    lt = 0;
    snd = 0;
    nextdust = 0;
    // Movement of the partner's tank copy. The vehicle is simulated by the engine, so the copy is driven by
    // moveTo() and NOT by writing .origin (that made the engine fight it: the constant shaking) and not by
    // rotateTo() while moving (that fought the steering: the copy jumped around). moveTo with a 0.2 s
    // interpolation lets the engine drive at the real speed with its own acceleration and steering, so the
    // hull turns because it is going there - a tracked vehicle cannot slide sideways, which is what made the
    // old 0.06 s version look like ice. A stationary hull turn (the partner rotating in place, the position
    // barely changes) is the only case where we rotate the hull ourselves.
    for (;;)
    {
        wait 0.05;
        if (coop_tank_age() > 3000)
        {
            // the partner's tank is gone (or no packets): its engine falls silent
            if (snd != 0)
            {
                ghost stoploopsound();
                snd = 0;
            }
            continue;
        }
        p = coop_tank_pos();
        if (coop_peer_solo())
        {
            // solo test only: the copy stands 500 units east (the height is the partner's own: a ground trace here hit other vehicles and made it jump)
            p = (p[0] + 500, p[1], p[2]);
        }
        if (!shown)
        {
            shown = 1;
            ghost.origin = p;
            ghost rotateTo(coop_tank_ang(), 0.1);
            ghost show();
        }

        // speed of the partner's tank from the packets (units a second, smoothed): the engine sound (idle / moving) and the dust of the tracks follow it
        now = gettime();
        if (!isdefined(lp))
        {
            lp = p;
            lt = now;
        }
        else if (now - lt >= 100)
        {
            sp = distance((p[0], p[1], 0), (lp[0], lp[1], 0)) / ((now - lt) / 1000);
            spd = spd * 0.6 + sp * 0.4;
            lp = p;
            lt = now;
        }
        want = snd;
        if (spd > 35 || (snd == 2 && spd > 20))
            want = 2;
        else
            want = 1;
        if (want != snd)
        {
            snd = want;
            ghost stoploopsound();
            if (isdefined(level.coop_tank_snd))
            {
                if (snd == 2)
                    ghost playloopsound(level.coop_tank_snd[1]);
                else
                    ghost playloopsound(level.coop_tank_snd[0]);
            }
        }
        if (snd == 2 && isdefined(level.coop_fx_tread) && now >= nextdust)
        {
            nextdust = now + 120;
            gy = anglestoforward((0, ghost.angles[1], 0));
            gr = anglestoright((0, ghost.angles[1], 0));
            for (sd = -1; sd <= 1; sd += 2)
            {
                dp = ghost.origin + (gr[0] * 62 * sd - gy[0] * 75, gr[1] * 62 * sd - gy[1] * 75, 6);
                playfx(level.coop_fx_tread, dp, (0, 0, 1));
            }
        }

        cur = ghost.origin;
        d = distance((cur[0], cur[1], 0), (p[0], p[1], 0));

        // a jump no tank can drive (teleport, respawn, a new mission): snap instead of racing there
        if (d > 260)
        {
            ghost.origin = p;
            ghost rotateTo(coop_tank_ang(), 0.1);
            continue;
        }

        // The engine drives the copy to the packet position over 0.2 s, so it travels at the partner's real
        // speed with the engine's own acceleration and braking (the old 0.06 s window made it dart to every
        // packet - that was the ice). The hull angle is still ours: moveTo steers the vehicle along the path but
        // does not turn the body, and leaving the rotation out made the copy drive sideways.
        // the recoil of the partner's shot (coop_tank_kick): the hull rocks back and settles, pushed a little the other way
        hl = coop_tank_ang();
        kk = coop_tank_kick(ghost, hl[1]);
        ghost moveTo((p[0] + kk[2], p[1] + kk[3], p[2]), kk[4]);
        ghost rotateTo((hl[0] + kk[0], hl[1], hl[2] + kk[1]), kk[4]);
        f = coop_tank_fire();
        if (f != lastfire)
        {
            lastfire = f;
            ghost.coop_kick = gettime();
            thread coop_tank_shot(ghost, p, coop_tank_turret());
        }
    }
}

// the rocking of the copy of the partner's tank after his shot: [0] extra pitch, [1] extra roll, [2] [3] shift of the hull (x y), [4] the time the engine
// takes to get there. The kick acts along the direction the cannon points relative to the hull: a shot ahead rocks the nose, a shot to the side the roll.
// It fades out within one second.
coop_tank_kick(ghost, hullyaw)
{
    r = [];
    r[0] = 0;
    r[1] = 0;
    r[2] = 0;
    r[3] = 0;
    r[4] = 0.2;
    if (!isdefined(ghost.coop_kick))
        return r;
    dt = (gettime() - ghost.coop_kick) / 1000;
    if (dt >= 1.0)
        return r;
    fade = 1 - dt;
    osc = sin(dt * 576) * fade;
    ta = coop_tank_turret();
    rel = ta[1] - hullyaw;
    r[0] = 0 - 2.5 * cos(rel) * osc;
    r[1] = 2.5 * sin(rel) * osc;
    tf = anglestoforward((0, ta[1], 0));
    sh = 0 - 10 * fade * fade;
    r[2] = tf[0] * sh;
    r[3] = tf[1] * sh;
    r[4] = 0.1;
    return r;
}

// a shot of the partner's tank: muzzle fire and an explosion where the shell hits; damages enemies only
coop_tank_shot(ghost, p, ang)
{
    // The partner cannon. The crusader model has NO tags at all for us: gettagangles("tag_turret") and
    // gettagorigin("tag_flash") return undefined and the calls threw a script error (the console.log showed the
    // source line instead of a value), which is why there was no shot at all. So the muzzle is computed from the
    // hull origin along the turret direction - the turret direction now comes from the partner view angles,
    // which are exactly where the cannon points while he drives the tank.
    o = ghost.origin;
    // the copy is a static model: its barrel is one piece with the hull and cannot turn, so the shot must
    // come from where that barrel points, otherwise the flash hangs in the air beside the tank
    fw = anglestoforward(coop_tank_ang());
    mz = (o[0] + fw[0] * 118, o[1] + fw[1] * 118, o[2] + 78 + fw[2] * 118);        // the barrel tip (220 was far beyond the real barrel: measured on a screenshot)
    back = (o[0] - fw[0] * 60, o[1] - fw[1] * 60, o[2] + 20);                        // dust blown back by the blast

    ghost playsound("crusader_fire");
    if (isdefined(level.coop_fx_tankflash))
        playfx(level.coop_fx_tankflash, mz, fw);
    // no blow-back dust: on the static model it hung over the turret as a yellow sand cloud
    if (isdefined(level.coop_fx_tankdist))
        playfx(level.coop_fx_tankdist, mz);
    iprintln("COOPT shot hull=" + o[0] + " " + o[1] + " " + o[2] + " mz=" + mz[0] + " " + mz[1] + " " + mz[2] + " fw=" + fw[0] + " " + fw[1] + " " + fw[2]);

    en = (mz[0] + fw[0] * 12000, mz[1] + fw[1] * 12000, mz[2] + fw[2] * 12000);
    tr = bullettrace(mz, en, false, undefined);
    hit = tr["position"];
    wait 0.4;
    if (isdefined(level.coop_fx_tankhit))
        playfx(level.coop_fx_tankhit, hit, (0, 0, 1));
    else if (isdefined(level.coop_fx_boom))
        playfx(level.coop_fx_boom, hit);
    earthquake(0.3, 0.6, hit, 700);
    ai = getaiarray("axis");
    for (i = 0; i < ai.size; i++)
    {
        if (!isdefined(ai[i]) || !isalive(ai[i]))
            continue;
        d = distance(ai[i].origin + (0, 0, 32), hit);
        if (d < 300)
            ai[i] doDamage(int(600 * (1 - d / 300)) + 20, hit);
    }
}
// START BARRIER: both PCs load a mission at different speeds, so the mission would begin at different moments on the two PCs (scripted scenes then
// diverge for good). This is called from main() BEFORE the mission's own script and does not wait: the bridge pauses the game at once (not one frame of
// the world runs, no sound), shows its own notice window and lets go when the partner has reached the same point in the same mission (or after 20 s).
// Off with the console variable coop_barrier "off"; nothing happens without a linked partner.
coop_barrier_now()
{
    if (getcvar("coop_barrier") == "off")
        return;
    if (coop_link_active() == 0)
        return;
    coop_barrier_begin(getcvar("mapname"), getcvarint("g_gameskill"), getcvarfloat("coop_barrier_delay"));    // the delay is a dev setting (pretend to load slower)
}

// the partner's grenade: our own flight (gravity, bounces off what bullettrace finds), then an explosion with fire/dust, sound, a shake and damage for ENEMIES only
// (the engine's magicbullet handles bullets only; the local player and friends are never hurt by it)
coop_fake_grenade(name, st, vel)
{
    smoke = 0;
    if (name == "smoke_grenade_american" || name == "smoke_grenade_british" || name == "smoke_grenade_russian" || name == "smoke_grenade_german")
        smoke = 1;
    // the throw animation of the puppet needs a moment to bring the arm forward: the grenade leaves the hand later than the event arrives
    wait 0.45;
    mdl = getweaponmodel(name);
    if (smoke)
        mdl = "xmodel/projectile_us_smoke_grenade";
    ent = spawn("script_model", st);
    if (isdefined(mdl))
        ent setmodel(mdl);
    // diagnostics of the smoke-grenade spin bug (coop_debug 1): weapon, model, start and where it comes to rest
    if (isdefined(mdl))
        coop_dbg("COOPG fake " + name + " model " + mdl + " smoke=" + smoke + " start " + st + " vel " + vel);
    else
        coop_dbg("COOPG fake " + name + " has no weapon model, smoke=" + smoke);
    pos = st;
    v = vel;
    rest = 0;
    t = 0;
    while (t < 3.4)
    {
        wait 0.05;
        t = t + 0.05;
        if (rest)
            continue;
        v = (v[0], v[1], v[2] - 40);
        np = (pos[0] + v[0] * 0.05, pos[1] + v[1] * 0.05, pos[2] + v[2] * 0.05);
        tr = bullettrace(pos, np, false, undefined);
        if (tr["fraction"] < 1)
        {
            n = tr["normal"];
            dp = v[0] * n[0] + v[1] * n[1] + v[2] * n[2];
            v = ((v[0] - 2 * dp * n[0]) * 0.4, (v[1] - 2 * dp * n[1]) * 0.4, (v[2] - 2 * dp * n[2]) * 0.4);
            tp = tr["position"];
            pos = (tp[0] + n[0] * 2, tp[1] + n[1] * 2, tp[2] + n[2] * 2);
            if (length(v) < 60)
            {
                rest = 1;
                v = (0, 0, 0);
                coop_dbg("COOPG " + name + " at rest after " + t + " s at " + pos + " angles " + ent.angles);
            }
        }
        else
            pos = np;
        ent moveTo(pos, 0.05);
    }
    if (name == "potato")
    {
        // the potato of the Russian missions is not a weapon: it just lands and is gone after a while
        wait 6;
        ent delete();
        return;
    }
    coop_dbg("COOPG " + name + " ends after " + t + " s, rest=" + rest + ", angles " + ent.angles);
    if (smoke)
        ent playsound("smokegrenade_explode_default");
    else
        ent playsound("grenade_explode_default");
    if (smoke)
    {
        if (isdefined(level.coop_fx_smoke))
            playfx(level.coop_fx_smoke, pos);
        if (isdefined(level.coop_fx_smoke2))
            playfx(level.coop_fx_smoke2, pos);
    }
    else
    {
        gs = bullettrace(pos + (0, 0, 24), pos - (0, 0, 40), false, undefined);
        surf = "";
        if (isdefined(gs["surfacetype"]))
            surf = gs["surfacetype"];
        coop_dbg("COOPG explosion surface='" + surf + "' at " + pos);
        if (isdefined(level.coop_fx_boom_hard) && (surf == "asphalt" || surf == "concrete" || surf == "brick" || surf == "rock" || surf == "metal" || surf == "plaster"))
            playfx(level.coop_fx_boom_hard, pos);
        else if (isdefined(level.coop_fx_boom))
            playfx(level.coop_fx_boom, pos);
        else if (isdefined(level.coop_fx_rifle))
            playfx(level.coop_fx_rifle, pos, (0, 0, 1));
        earthquake(0.25, 0.6, pos, 400);
        ai = getaiarray("axis");
        for (i = 0; i < ai.size; i++)
        {
            if (!isdefined(ai[i]) || !isalive(ai[i]))
                continue;
            d = distance(ai[i].origin + (0, 0, 32), pos);
            if (d < 260)
                ai[i] doDamage(int(320 * (1 - d / 260)) + 10, pos);
        }
    }
    wait 0.1;
    ent delete();
}

coop_chan_publish()
{
    while (!isdefined(level.player))
        wait 0.1;
    level.coop_evc = 0;
    level.coop_evt = 0;
    r = coop_ping(7777);
    thread coop_enemy_watch();
    thread coop_vehicle_watch();
    thread coop_kill_recv();
    thread coop_level_watch();
    thread coop_health_hud();
    thread coop_flag_sync();
    thread coop_trigger_watch();
    thread coop_grenade_recv();
    thread coop_grenade_watch();
    thread coop_tank_main();
    mpress = 0;
    lasttg = 0;
    lastdead = 0;
    lastwi = -1;
    awi = -1;
    lastammo = -1;
    lastclip = -1;
    autorl = 0;
    maxclip = 0;
    lastbp = 0;
    mgt = 0;
    lastrl = 0;
    level.coop_fc = 0;
    for (;;)
    {
        wn = level.player getcurrentweapon();
        wi = coop_weapon_id(tolower(wn));
        // on a placed machine gun: the partner sees the gunner pose (ids 28 stand / 29 prone: the mg42_bipod_* entries of the table)
        if (coop_mounted_mg())
        {
            wi = 28;
            if (level.player getstance() == "prone")
                wi = 29;
        }
        if (wi != lastwi)
        {
            lastwi = wi;
            if (wi == 0 && isdefined(wn) && wn != "" && tolower(wn) != "none")
                iprintln("COOPN weapon not in the table: [" + wn + "]");
        }
        // death: event type 5 once per death; the peer then loads its last save too (loadgame_continue), so both restart from a checkpoint
        dead = 0;
        if (isdefined(level.player.health) && level.player.health <= 0)
            dead = 1;
        if (dead && !lastdead && !isdefined(level.coop_induced))
        {
            level.coop_evc = level.coop_evc + 1;
            level.coop_evt = level.coop_evc * 8 + 5;
        }
        if (!dead)
            level.coop_induced = undefined;
        lastdead = dead;
        if (level.player ismeleeing())
        {
            if (!mpress)
            {
                mpress = 1;
                level.coop_evc = level.coop_evc + 1;
                level.coop_evt = level.coop_evc * 8 + 1;
            }
        }
        else
            mpress = 0;
        an = level.player getplayerangles();
        // a shot event for every real shot (the ammunition of the held weapon went down; reload and weapon change do not count):
        // event = ((shot counter * 256) + pitch + 90) * 8 + 3
        ammo = level.player getammocount(wn);
        shot = 0;
        if (wi == awi && ammo < lastammo && lastammo - ammo < 10)
            shot = lastammo - ammo;
        if (wi >= 28)
        {
            // a placed MG fires about 20 rounds a second: the rounds are counted by TIME while the button is held (one per pass of this loop lost shots whenever a pass took longer than 50 ms)
            shot = 0;
            if (level.player attackbuttonpressed())
            {
                mgnow = gettime();
                if (mgt == 0 || mgnow - mgt > 200)
                {
                    shot = 1;
                    mgt = mgnow;
                }
                else
                {
                    shot = int((mgnow - mgt) / 50);
                    if (shot > 6)
                        shot = 6;
                    mgt = mgt + shot * 50;
                }
            }
            else
                mgt = 0;
        }   // several rounds can fall between two samples of a fast weapon: the counter below carries all of them
        clip = -1;
        if (level.player getweaponslotweapon("primary") == wn)
            clip = level.player getweaponslotclipammo("primary");
        else if (level.player getweaponslotweapon("primaryb") == wn)
            clip = level.player getweaponslotclipammo("primaryb");
        if (!isdefined(clip))
            clip = -1;
        // reload: the moment of the reload key press while the magazine is not full (the clip only grows at the END of a reload, which looked late on the puppet)
        if (wi != awi)
            maxclip = clip;
        if (clip > maxclip)
            maxclip = clip;
        // reload: only when the game will really do it. The clip grows only at the END of a reload, so the key
        // press is used, but the weapon's own rule is respected: a bolt action rifle fed by stripper clips
        // (Lee Enfield: 10 in the magazine, 5 per reload) refuses a reload unless a whole clip fits, so the puppet
        // must not animate a reload the game rejected. rq = how many rounds one reload adds.
        rq = 1;
        if (wn == "enfield" || wn == "enfield_scope" || wn == "enfield_sniper")
            rq = 5;
        bp = level.player buttonpressed("r");   // buttonpressed takes KEY names (a single character is the key itself): R is the default reload key
        if (!isdefined(bp))
            bp = 0;
        rl = 0;
        if (bp && !lastbp && wi == awi && clip >= 0 && maxclip - clip >= rq && gettime() - lastrl >= 700)
        {
            rl = 1;
            lastrl = gettime();
        }
        // the magazine ran empty and the game reloads by itself (reserve left): show the reload a moment after the last shot
        if (wi == awi && lastclip > 0 && clip == 0 && ammo > 0)
            autorl = gettime() + 350;
        lastbp = bp;
        awi = wi;
        lastammo = ammo;
        lastclip = clip;
        if (rl && !shot && wi > 0)
        {
            level.coop_evc = level.coop_evc + 1;
            level.coop_evt = level.coop_evc * 8 + 4;
        }
        if (autorl > 0 && gettime() >= autorl)
        {
            autorl = 0;
            level.coop_evc = level.coop_evc + 1;
            level.coop_evt = level.coop_evc * 8 + 4;
        }
        if (shot && wi > 0)
        {
            level.coop_fc = (level.coop_fc + shot - 1) % 999 + 1;
            pb = int(an[0] + 90.5);
            if (pb < 0)
                pb = 0;
            if (pb > 180)
                pb = 180;
            level.coop_evt = (level.coop_fc * 256 + pb) * 8 + 3;
        }
        po = level.player.origin;
        st = 0;
        sn = level.player getstance();
        if (sn == "crouch")
            st = 1;
        else if (sn == "prone")
            st = 2;
        if (!level.player isonground() && st == 0)
            st = 3;
        // on a placed machine gun the player is not "on the ground" for the engine: that must not read as a jump
        if (st == 3 && isdefined(level.coop_mgon) && level.coop_mgon)
            st = 0;
        if (coop_player_link() > 0)
            st = 4;
        // F8 (the bridge): go to the partner, 60 units beside him; alive and on foot only
        if (coop_pop_teleport() && !dead)
        {
            tpv = coop_peer_state();
            level.player setorigin((tpv[0] + 60, tpv[1], tpv[2] + 8));
        }
        hp = level.player.health;
        if (!isdefined(hp))
            hp = 500;
        mh = getcvarint("g_player_maxhealth");
        if (mh < 1)
            mh = 500;
        hp = int(hp * 100 / mh);
        if (hp < 0)
            hp = 0;
        if (hp > 100)
            hp = 100;
        // a failed mission (coop_mission_failed in _utility sets the request): the death event takes priority over the shot/melee events that this loop
        // writes (the mission failed while the player was firing at an ally: the plain write was overwritten in the same pass) and stays for 8 packets
        if (isdefined(level.coop_failreq))
        {
            level.coop_failreq = undefined;
            level.coop_evc = level.coop_evc + 1;
            level.coop_failevt = level.coop_evc * 8 + 5;
            level.coop_failhold = 8;
        }
        if (isdefined(level.coop_failhold) && level.coop_failhold > 0)
        {
            level.coop_evt = level.coop_failevt;
            level.coop_failhold = level.coop_failhold - 1;
        }
        coop_publish_state(po, an[1], st, level.coop_evt, wi, coop_player_parent(), hp);
        wait 0.05;
    }
}
"@
$net7Body = $net7Body -replace 'rx = getcvarfloat\("coop_px"\);', 'pv = coop_peer_state(); rx = pv[0];'
$net7Body = $net7Body -replace 'ry = getcvarfloat\("coop_py"\);', 'ry = pv[1];'
$net7Body = $net7Body -replace 'getcvarfloat\("coop_pyaw"\)', 'coop_peer_yaw()'
# the puppet's height comes from the PEER's own height (a trace from 48 above to 96 below it: the floor under him, also inside a building),
# not from a trace starting 512 above the LOCAL player, which hit the roof when the partner was inside a building
$net7Body = $net7Body -replace 'bulletTrace\(\(rx, ry, po\[2\] \+ 512\), \(rx, ry, po\[2\] - 1024\)', 'bulletTrace((rx, ry, pv[2] + 48), (rx, ry, pv[2] - 96)'
$net7Body = $net7Body -replace 'gz = po\[2\];', 'gz = pv[2];'
$net7Body = $net7Body -replace 'puppet moveTo\(\(rx, ry, gz\), 0\.1\);', "if (coop_peer_stance() >= 3)`n                gz = pv[2];`n            if (!isdefined(puppet.coop_veh))`n                puppet moveTo((rx, ry, gz), 0.1);"
$net7Body = $net7Body -replace 'rotateTo\(\(0, coop_peer_yaw\(\), 0\)', 'rotateTo((coop_pose_pitch(), coop_peer_yaw(), 0)'
$net7Body = $net7Body -replace 'puppet rotateTo\(\(coop_pose_pitch\(\), coop_peer_yaw\(\), 0\), 0\.1\);', "if (!isdefined(puppet.coop_veh))`n            puppet rotateTo((coop_pose_pitch(), coop_peer_yaw(), 0), 0.1);"
# the console log is switched on by the profile (seta logfile "2" in config.cfg of the TEST profile) so that compile errors are logged too; a script may not set it then
$net7Body = $net7Body -replace 'setcvar\("logfile", "2"\);', ''
# animation by speed: the puppet runs (the stock drone run loop) while it moves faster than ~50 units/s, else the calm idle pose; the speed is the
# puppet's own displacement, so nothing extra goes over the network
$net7Body = $net7Body -replace 'puppet thread coop_pose\(\);', 'puppet thread coop_pose_move();'
# TEST of a second animation "bank": the engine refuses a dynamic animation tree of more than 32 nodes, counted per script that uses the tree.
# A second script file (maps/coop_anims2.gsc) with its own #using_animtree("coop_puppet") may have its own budget. Switch: coop_bank2_test "on".
if (-not $Bank2) {
$net7Body = $net7Body + @'

coop_p2_ready()
{
    return false;
}

coop_p2_len(id)
{
    return 1000;
}

coop_p2_set(id)
{
}

coop_p2_shot(stance)
{
    return false;
}

coop_p2_back(id, ms)
{
}

coop_fxbody()
{
    return self;
}
'@
}
if ($Bank2) {
# the second body: its own animation tree (coop_puppet2), shown instead of the main puppet for the pistol poses that do not fit the 32-node limit of the first tree
$net7Body = $net7Body -replace 'puppet thread coop_pose_move\(\);', ('puppet thread coop_pose_move();' + [Environment]::NewLine + '    puppet thread coop_p2_init();')
$net7Body = $net7Body + @'

coop_mg_release(t)
{
    // the map gun comes back to its place, our copy of it goes
    if (isdefined(t) && isdefined(t.coop_org))
    {
        t.origin = t.coop_org;
        t.coop_org = undefined;
    }
    if (isdefined(self.coop_mgg))
    {
        self.coop_mgg delete();
        self.coop_mgg = undefined;
    }
}

coop_mg_aim()
{
    // The engine does not let a script aim a placed machine gun nobody holds (a target entity is never taken: logged "target set 0"), and a gun in the
    // hands of the body came out turned the wrong way. So while the partner operates a placed machine gun on this PC the map gun is moved out of sight (the
    // game's own scripts do the same, maps/beltot.gsc) and a copy of it (a script_model, the same model) stands in its place and is turned to where the
    // partner aims: yaw from his view, pitch from his last shot
    t = undefined;
    srch = 0;
    for (;;)
    {
        wait 0.05;
        if (!isdefined(self))
            return;
        wid = coop_peer_weapon();
        if (wid < 28 || wid > 32)
        {
            if (isdefined(t))
            {
                self coop_mg_release(t);
                t = undefined;
                self.coop_mgpitch = 0;
            }
            continue;
        }
        if (isdefined(t))
        {
            if (isdefined(self.coop_mgg))
            {
                pit = 0;
                if (isdefined(self.coop_mgpitch))
                    pit = self.coop_mgpitch;
                if (pit > 35)
                    pit = 35;
                if (pit < -35)
                    pit = -35;
                self.coop_mgg.angles = (pit, coop_peer_yaw(), 0);
            }
            continue;
        }
        if (gettime() < srch)
            continue;
        srch = gettime() + 500;
        best = 140;
        a = getentarray("misc_mg42", "classname");
        b = getentarray("misc_turret", "classname");
        for (i = 0; i < b.size; i++)
            a[a.size] = b[i];
        for (i = 0; i < a.size; i++)
        {
            if (!isdefined(a[i]))
                continue;
            if (isdefined(a[i] getturretowner()))
                continue;
            d = distance(a[i].origin, self.origin);
            if (d < best)
            {
                best = d;
                t = a[i];
            }
        }
        if (!isdefined(t))
            continue;
        t.coop_org = t.origin;
        g = spawn("script_model", t.origin);
        g setmodel("xmodel/weapon_mg42");
        g.angles = (0, coop_peer_yaw(), 0);
        self.coop_mgg = g;
        self.coop_mgpitch = 0;
        t.origin = t.origin + (0, 0, -1000);
        coop_log("COOPMG the placed machine gun at " + int(best) + " units from the partner is replaced by a copy that follows his aim");
    }
}

coop_p2_mgweapon(b, want)
{
    // the machine gun in the hands of the second body (ids 9-12), taken off again for every other pose
    if (want && !isdefined(self.coop_mgm))
    {
        // the world model of the placed gun (the weapon item itself is not precached by this mod, getweaponmodel would give nothing): precached in coop_precache_weapons
        mdl = "xmodel/weapon_mg42";
        b attach(mdl, "tag_weapon_right");
        self.coop_mgm = mdl;
        coop_log("COOPMG machine gun model attached to the gunner body");
    }
    else if (!want && isdefined(self.coop_mgm))
    {
        b detach(self.coop_mgm, "tag_weapon_right");
        self.coop_mgm = undefined;
    }
}

coop_p2_init()
{
    self.coop_p2 = 0;
    self thread coop_mg_aim();
    wait 1;
    b = spawn("script_model", self.origin);
    if (!isdefined(b))
        return;
    b setmodel(self.model);
    b makeFakeAI();
    b setcontents(0);
    maps\coop_anims2::setup(b);
    b hide();
    self.coop_b = b;
    thread coop_p2_follow(self, b);
    coop_log("COOPANIM second body ready");
}

coop_p2_ready()
{
    return isdefined(self.coop_b);
}

coop_p2_len(id)
{
    return maps\coop_anims2::length(id);
}

coop_p2_follow(a, b)
{
    for (;;)
    {
        wait 0.05;
        if (!isdefined(a))
        {
            if (isdefined(b))
                b delete();
            return;
        }
        if (!isdefined(b))
            return;
        if (a.coop_p2 != 0)
        {
            b.origin = a.origin;
            b.angles = a.angles;
        }
    }
}

coop_fxbody()
{
    // the body that is on screen now: effects (muzzle flash) must come from ITS weapon
    if (isdefined(self.coop_b) && isdefined(self.coop_p2) && self.coop_p2 != 0)
        return self.coop_b;
    return self;
}

coop_p2_shot(stance)
{
    // a shot with a pistol while the second body is shown: its own shooting clip, then back to its idle
    if (!isdefined(self.coop_b) || !isdefined(self.coop_p2) || self.coop_p2 == 0)
        return false;
    c = 0;
    if (stance == 1 && (self.coop_p2 == 1 || self.coop_p2 == 6))
        c = 7;
    else if (stance == 0 && self.coop_p2 == 4)
        c = 8;
    else if (self.coop_p2 == 9)
        c = 10;
    else if (self.coop_p2 == 11)
        c = 12;
    if (c == 0)
        return false;
    // fast fire (a placed MG: 20 shots a second): the firing clip is restarted at most every 120 ms so that it can play, and ONE thread (not one per shot,
    // each of which restarted the idle clip over the firing one) brings the body back to its idle 250 ms after the LAST shot
    now = gettime();
    if (!isdefined(self.coop_p2_ls) || now - self.coop_p2_ls >= 120)
    {
        self.coop_p2_ls = now;
        maps\coop_anims2::play(self.coop_b, c);
    }
    self.coop_p2_until = now + 250;
    if (!isdefined(self.coop_p2_bk))
    {
        self.coop_p2_bk = 1;
        self thread coop_p2_shotback(self.coop_p2);
    }
    return true;
}

coop_p2_shotback(id)
{
    for (;;)
    {
        wait 0.05;
        if (!isdefined(self))
            return;
        if (gettime() >= self.coop_p2_until)
            break;
    }
    self.coop_p2_bk = undefined;
    self.coop_p2_ls = undefined;
    if (isdefined(self.coop_b) && isdefined(self.coop_p2) && self.coop_p2 == id)
        maps\coop_anims2::play(self.coop_b, id);
}

coop_p2_back(id, ms)
{
    // after ms milliseconds the second body returns to the idle (or the run) it was in, unless the state changed meanwhile
    cur = self.coop_p2;
    wait (ms / 1000);
    if (isdefined(self.coop_b) && isdefined(self.coop_p2) && (self.coop_p2 == cur || self.coop_p2 == 3 || self.coop_p2 == 5))
    {
        self.coop_p2 = id;
        maps\coop_anims2::play(self.coop_b, id);
    }
}

coop_p2_set(id)
{
    if (!isdefined(self.coop_b) || id == self.coop_p2)
        return;
    b = self.coop_b;
    if (id == 0)
    {
        self coop_p2_mgweapon(b, false);
        b hide();
        self show();
        self.coop_p2 = 0;
        return;
    }
    if (self.coop_p2 == 0)
    {
        // body, head and weapon of the main puppet as they are now
        b detachAll();
        b setmodel(self.model);
        self.coop_mgm = undefined;
        n = self getattachsize();
        for (i = 0; i < n; i++)
            b attach(self getattachmodelname(i), self getattachtagname(i));
        b.origin = self.origin;
        b.angles = self.angles;
        maps\coop_anims2::play(b, id);
        self hide();
        b show();
    }
    else
        maps\coop_anims2::play(b, id);
    self coop_p2_mgweapon(b, false);
    self.coop_p2 = id;
}
'@
}
$net7Body += @"

coop_pose_pitch()
{
    return 0;
}

coop_pose_move()
{
    self endon("death");
    wait 0.1;
    last = self.origin;
    cur = -1;
    nextrestart = 0;
    lastev = coop_peer_event();
    busyuntil = 0;
    lastrl_anim = 0;
    reloadfree = 0;
    lastdbg = 0;
    lastw = -1;
    lastpz = 0;
    lastun = 0;
    lastmg = 0;
    jdown = 0;
    zero = 0;
    vhid = 0;
    self.coop_veh = undefined;
    self.coop_wmodel = getweaponmodel("ppsh");
    while (isdefined(self))
    {
        pstv = coop_peer_stance();
        if (pstv == 4 && coop_peer_parent() > 0 && !isdefined(self.coop_veh))
        {
            if (!isdefined(self.coop_vwait) || gettime() > self.coop_vwait)
            {
                v = coop_find_vehicle(coop_peer_state());
                if (isdefined(v))
                    self coop_ride(v);
                else
                    self.coop_vwait = gettime() + 3000;
            }
        }
        else if ((pstv != 4 || coop_peer_parent() == 0) && isdefined(self.coop_veh))
        {
            self unlink();
            self.coop_veh = undefined;
            r = coop_ping(7002);
        }
        wid = coop_peer_weapon();
        if (wid == 0)
            zero = zero + 1;
        else
            zero = 0;
        if (wid != lastw && (wid != 0 || zero >= 8))
        {
            lastw = wid;
            if (isdefined(self.coop_wmodel) && self coop_is_attached(self.coop_wmodel))
                self detach(self.coop_wmodel, "tag_weapon_right");
            self.coop_wmodel = undefined;
            if (wid > 0 && wid <= 27)
            {
                mdl = getweaponmodel(level.coop_weps[wid - 1]);
                if (isdefined(mdl))
                {
                    if (!(self coop_is_attached(mdl)))
                        self attach(mdl, "tag_weapon_right");
                    self.coop_wmodel = mdl;
                }
            }
        }
        // (the debug pose selector coop_anim 10-35 is gone: every animation name this script mentions counts against the engine limit of the animation tree)
        ev = coop_peer_event();
        if (ev != lastev)
        {
            lastev = ev;
            ty = ev % 8;
            t = 0;
            fw = anglestoforward((0, self.angles[1], 0));
            if (ty == 1)
            {
                self setflaggedanimknobrestart("coop_pose", %stand_aim_straight, 1, 0.1, 1);
                home = self.origin;
                self moveTo(home + (fw[0] * 45, fw[1] * 45, 0), 0.12);
                wait 0.15;
                self moveTo(home, 0.2);
                t = 400;
            }
            else if (ty == 2)
            {
                if (coop_peer_stance() == 2)
                    t = 900;
                else if (coop_peer_stance() == 1)
                {
                    self setflaggedanimknobrestart("coop_pose", %crouch_grenade_throw, 1, 0.1, 1);
                    t = int(getanimlength(%crouch_grenade_throw) * 1000);
                }
                else
                {
                    self setflaggedanimknobrestart("coop_pose", %stand_grenade_throw, 1, 0.1, 1);
                    t = int(getanimlength(%stand_grenade_throw) * 1000);
                }
            }
            else if (ty == 3)
            {
                spitch = (int((ev - 3) / 8) % 256) - 90;
                swid = coop_peer_weapon();
                nofire = 0;
                seye = self.origin + (0, 0, 60);
                sfw = (1, 0, 0);
                sfc = int(int((ev - 3) / 8) / 256);
                if (!isdefined(self.coop_fc))
                    self.coop_fc = sfc - 1;
                nsh = (sfc - self.coop_fc + 999) % 999;
                self.coop_fc = sfc;
                if (nsh < 1 || nsh > 60)
                    nsh = 1;
                if (swid > 27)
                    self.coop_mgpitch = spitch;
                if (swid > 0)
                {
                    syaw = self.angles[1];
                    if (coop_peer_solo())
                    {
                        pan = level.player getplayerangles();
                        syaw = pan[1];
                    }
                    sfw = anglestoforward((spitch, syaw, 0));
                    seye = self.origin + (0, 0, 60);
                    if (coop_peer_stance() == 1)
                        seye = self.origin + (0, 0, 40);
                    // the peer's bullets must never hit the local player: when the first thing on the line is the player, the bullet stops 80 units short of him
                    sst = seye + (sfw[0] * 30, sfw[1] * 30, sfw[2] * 30);
                    sen = seye + (sfw[0] * 3000, sfw[1] * 3000, sfw[2] * 3000);
                    shtr = bullettrace(sst, sen, true, self);
                    nofire = 0;
                    // geometry first (any distance, walls ignored): no bullet at all when the line passes within 130 units of the local player
                    ppo = level.player.origin;
                    pcn = (ppo[0], ppo[1], ppo[2] + 40);
                    prel = (pcn[0] - sst[0], pcn[1] - sst[1], pcn[2] - sst[2]);
                    ptt = prel[0] * sfw[0] + prel[1] * sfw[1] + prel[2] * sfw[2];
                    if (ptt > -60 && ptt < 3100)
                    {
                        pcl = (sst[0] + sfw[0] * ptt, sst[1] + sfw[1] * ptt, sst[2] + sfw[2] * ptt);
                        // 130 units for a rifle; a gun on the MG stand fires a fixed line, so 60 (the body of the local player) is enough: with 130 the bullets
                        // were switched off whenever the two players stood close together and the fire came in parts
                        nfr = 130;
                        if (swid > 27)
                            nfr = 60;
                        if (distance(pcl, pcn) < nfr)
                            nofire = 1;
                    }
                    if (isdefined(shtr["entity"]) && shtr["entity"] == level.player)
                    {
                        spos = shtr["position"];
                        if (distance(sst, spos) < 120)
                            nofire = 1;
                        else
                            sen = (spos[0] - sfw[0] * 80, spos[1] - sfw[1] * 80, spos[2] - sfw[2] * 80);
                    }
                    swpn = level.coop_weps[swid - 1];
                    if (swid > 27)
                        swpn = "bren";
                    for (k = 0; k < nsh && k < 6 && !nofire; k++)
                    {
                        magicbullet(swpn, sst, sen);
                        // a burst of many rounds is fired at once: waiting between them made the loop fall behind and the surplus was dropped
                        if (k + 1 < nsh && nsh <= 3)
                            wait 0.03;
                    }
                }
                // the gunner pose must be there with the FIRST shot: while the shots keep the loop busy the pose code below never runs
                if (swid >= 28 && swid <= 32 && coop_p2_ready() && coop_peer_stance() != 4)
                {
                    if (swid == 29 || swid == 31 || coop_peer_stance() == 2)
                        self coop_p2_set(11);
                    else
                        self coop_p2_set(9);
                }
                if (!isdefined(self.coop_shn))
                    self.coop_shn = 0;
                self.coop_shn = self.coop_shn + 1;
                bd = 0;
                if (isdefined(self.coop_p2))
                    bd = self.coop_p2;
                if (self.coop_shn % 40 == 1)
                    coop_log("COOPSHOT event " + self.coop_shn + " rounds " + nsh + " suppressed " + nofire + " weapon " + swid + " body " + bd);
                if (self coop_p2_shot(coop_peer_stance()))
                    wait 0.0;
                else if (coop_peer_stance() == 2)
                    self setflaggedanimknobrestart("coop_pose", %prone_shoot_straight, 1, 0.05, 1);
                else if (coop_peer_stance() == 1)
                    self setflaggedanimknobrestart("coop_pose", %crouch_shoot_straight, 1, 0.05, 1);
                else
                    self setflaggedanimknobrestart("coop_pose", %stand_shoot_straight, 1, 0.05, 1);
                t = 160;
                if (swid > 0)
                {
                    swn = level.coop_weps[swid - 1];
                    sfx = level.coop_fx_rifle;
                    // the gunner body (second body ids 9 / 11): neither its model nor the attached gun has a tag_flash (a missing tag is a script error), so the flash
                    // is placed in the air in front of the gun, along the line of fire
                    if (isdefined(self.coop_p2) && (self.coop_p2 == 9 || self.coop_p2 == 11))
                        {
                        fo = self.origin + (0, 0, 40);
                        if (isdefined(self.coop_mgg))
                            fo = self.coop_mgg.origin + (0, 0, 8);
                        playfx(sfx, (fo[0] + sfw[0] * 45, fo[1] + sfw[1] * 45, fo[2] + sfw[2] * 45), sfw);
                    }
                    else
                        playfxontag(sfx, self coop_fxbody(), "tag_flash");
                    if (swn == "mosin_nagant" || swn == "mosin_nagant_sniper" || swn == "kar98k" || swn == "kar98k_sniper" || swn == "enfield" || swn == "enfield_scope" || swn == "springfield")
                    {
                        // bolt action: the reload clip sped up stands in for working the bolt (placeholder until a real rechamber animation is found)
                        wait 0.3;
                        if (coop_peer_stance() == 2)
                            t = 300;
                        else if (coop_peer_stance() == 1)
                        {
                            self setflaggedanimknobrestart("coop_pose", %crouch_rechamber, 1, 0.1, 1);
                            t = int(getanimlength(%crouch_rechamber) * 1000);
                        }
                        else
                        {
                            self setflaggedanimknobrestart("coop_pose", %stand_rechamber, 1, 0.1, 1);
                            t = int(getanimlength(%stand_rechamber) * 1000);
                        }
                    }
                }
            }
            else if (ty == 4)
            {
                // the partner weapon decides the reload clip (this is a different thread than the pose code)
                ps = 0;
                if (lastw > 0 && lastw <= 32)
                {
                    pw = level.coop_weps[lastw - 1];
                    if (pw == "tt30" || pw == "luger" || pw == "colt" || pw == "webley")
                        ps = 1;
                }
                // spam guard: the peer's reload key can repeat faster than a clip lasts, and the game may refuse the
                // reload at all (Lee Enfield with 6..9 rounds, for instance). The animation must not restart on every
                // repeat: only once per 800 ms, and a rejected repeat must not even extend busyuntil.
                if (gettime() < reloadfree)
                {
                    // the reload clip that is playing has not finished yet: a new key press must not restart it.
                    // A fixed window is not enough - the Sten reload is longer than 800 ms, so a press at ~1/s
                    // restarted the clip in the middle (that is what was seen in tests). Wait for the clip.
                    wait 0.05;
                    continue;
                }
                // prone: no reload clip (the standing one would make the puppet get up); it stays in its prone pose
                if (coop_peer_stance() == 1 && ps && coop_p2_ready())
                {
                    self coop_p2_set(3);
                    t = coop_p2_len(3);
                    self thread coop_p2_back(1, t);
                }
                else if (coop_peer_stance() == 1)
                {
                    self setflaggedanimknobrestart("coop_pose", %reload_crouch_rifle, 1, 0.1, 1);
                    t = int(getanimlength(%reload_crouch_rifle) * 1000);
                }
                else if (coop_peer_stance() != 2 && ps && coop_p2_ready())
                {
                    self coop_p2_set(5);
                    t = coop_p2_len(5);
                    self thread coop_p2_back(4, t);
                }
                else if (coop_peer_stance() != 2)
                {
                    if (ps)
                        self setflaggedanimknobrestart("coop_pose", %pt_reload_stand_pistol, 1, 0.1, 1);
                    else
                        self setflaggedanimknobrestart("coop_pose", %reload_stand_rifle, 1, 0.1, 1);
                    t = int(getanimlength(%reload_stand_rifle) * 1000);
                }
                else
                    t = 300;
            }
            busyuntil = gettime() + t;
            if (ty == 4) reloadfree = busyuntil;   // the reload clip plays until then: a key press must not restart it
            cur = -1;
        }
        if (gettime() < busyuntil)
        {
            wait 0.05;
            continue;
        }
        o = self.origin;
        level.coop_puppet_origin = o;
        d = distance((o[0], o[1], 0), (last[0], last[1], 0));
        last = o;
        st = coop_peer_stance();
        mv = 0;
        thr = 12;
        if (st == 2)
            thr = 3;
        if (d > thr)
            mv = 1;
        key = st * 2 + mv;
        // no weapon in the partner's hands (his weapon id stays 0): the unarmed idle and run clips instead of the rifle-ready pose
        un = 0;
        if (lastw == 0)
            un = 1;
        // pistols hold a different pose in every stance: idle, run, crouch, prone, crawl and the reload are all
        // separate clips in the game, so the puppet has to use them instead of the rifle set
        ps = 0;
        if (lastw > 0 && lastw <= 32)
        {
            pw = level.coop_weps[lastw - 1];
            if (pw == "tt30" || pw == "luger" || pw == "colt" || pw == "webley")
                ps = 1;
        }
        // a mounted machine gun (weapon ids 28-32): the gunner pose, prone for the prone gun, standing for the others
        mgm = 0;
        if (lastw >= 28 && lastw <= 32 && coop_p2_ready() && st != 4)
            mgm = 1;
        now = gettime();
        pvj = coop_peer_state();
        dz = pvj[2] - lastpz;
        if (st == 3 && cur >= 6 && cur <= 7 && !jdown && dz < -3)
        {
            // the partner started to fall: the landing part of a jump
            self setflaggedanimknobrestart("coop_pose", %jump_down_56, 1, 0.15, 1.2);
            jdown = 1;
        }
        lastpz = pvj[2];
        if (key != cur || now >= nextrestart || un != lastun || mgm != lastmg)
        {
            lastun = un;
            lastmg = mgm;
            t = 600000;
            p2w = 0;
            if (mgm)
            {
                if (st == 2 || lastw == 29 || lastw == 31)
                    p2w = 11;
                else
                    p2w = 9;
            }
            else if (st == 3)
            {
                // airborne: a rising partner gets the leap clip (sped up), one that is already falling gets the jump-down clip
                jdown = 0;
                if (dz < -2)
                {
                    self setflaggedanimknobrestart("coop_pose", %jump_down_56, 1, 0.15, 1.2);
                    jdown = 1;
                }
                else
                    self setflaggedanimknobrestart("coop_pose", %jump_across_100, 1, 0.1, 1.5);
            }
            else if (st == 2)
            {
                if (mv)
                    self setflaggedanimknobrestart("coop_pose", %prone_crawl, 1, 0.2, 1);
                else if (un)
                    self setflaggedanimknobrestart("coop_pose", %wounded_crawlidle, 1, 0.2, 1);
                else
                    self setflaggedanimknobrestart("coop_pose", %prone_aim_idle, 1, 0.2, 1);
            }
            else if (st == 1)
            {
                if (un && mv)
                {
                    self setflaggedanimknobrestart("coop_pose", %crouchwalk_loop, 1, 0.2, 1);
                    t = int(getanimlength(%crouchwalk_loop) * 1000);
                }
                else if (un)
                    self setflaggedanimknobrestart("coop_pose", %unarmed_crouch_idle1, 1, 0.2, 1);
                else if (ps && coop_p2_ready() && mv)
                {
                    p2w = 6;
                    t = coop_p2_len(6);
                }
                else if (ps && coop_p2_ready())
                    p2w = 1;
                else
                    self setflaggedanimknobrestart("coop_pose", %crouch_shoot_straight, 1, 0.2, 1);
            }
            else if (st == 4)
                self setflaggedanimknobrestart("coop_pose", %stand_alert_2, 1, 0.2, 1);
            else if (mv)
            {
                if (un)
                {
                    self setflaggedanimknobrestart("coop_pose", %unarmed_run_russian, 1, 0.2, 1);
                    t = int(getanimlength(%unarmed_run_russian) * 1000);
                }
                else if (ps)
                {
                    self setflaggedanimknobrestart("coop_pose", %combat_run_fast_pistol, 1, 0.2, 1);
                    t = int(getanimlength(%combat_run_fast_pistol) * 1000);
                }
                else
                {
                    self setflaggedanimknobrestart("coop_pose", %drone_run_forward_1, 1, 0.2, 1);
                    t = int(getanimlength(%drone_run_forward_1) * 1000);
                }
            }
            else if (un)
                self setflaggedanimknobrestart("coop_pose", %standunarmed_idle_loop, 1, 0.2, 1);
            else if (ps && coop_p2_ready())
                p2w = 4;
            else if (ps)
                self setflaggedanimknobrestart("coop_pose", %pt_stand_alert_pistol, 1, 0.2, 1);
            else
                self setflaggedanimknobrestart("coop_pose", %stand_alert_2, 1, 0.2, 1);
            self coop_p2_set(p2w);
            cur = key;
            nextrestart = now + t;
        }
        if (st == 3)
            wait 0.08;
        else
            wait 0.25;
    }
}
"@
$net7Body += @"

coop_find_vehicle(org)
{
    pn = coop_peer_parent();
    if (pn > 0)
    {
        // the peer's parent is known by its entity number: look for exactly that entity among every kind of mover/model/vehicle
        // (a ride rig or a cut-scene mover is a script_model/script_origin, not a script_vehicle); never fall back to another one
        cls = [];
        cls[0] = "script_vehicle";
        cls[1] = "script_model";
        cls[2] = "script_origin";
        cls[3] = "script_brushmodel";
        for (ci = 0; ci < cls.size; ci++)
        {
            g = getentarray(cls[ci], "classname");
            for (i = 0; i < g.size; i++)
            {
                if (g[i] getentitynumber() == pn)
                    return g[i];
            }
        }
        return;
    }
    g = getentarray("script_vehicle", "classname");
    best = undefined;
    bd = 700;
    for (i = 0; i < g.size; i++)
    {
        d = distance(g[i].origin, org);
        if (d < bd)
        {
            bd = d;
            best = g[i];
        }
    }
    return best;
}

coop_ride(v)
{
    // the place to take is where the PEER really is (his reported position), not where the puppet currently stands: on a landing boat the
    // puppet's own spawn point comes from a ground trace and can be on the sea bed
    P = coop_peer_state();
    vo0 = v.origin;
    d = P - vo0;
    fw = anglestoforward(v.angles);
    rt = anglestoright(v.angles);
    up = anglestoup(v.angles);
    a = vectordot(d, fw);
    b = vectordot(d, rt);
    c = vectordot(d, up);
    bestk = -1;
    beste = 1000000;
    for (k = 0; k < 4; k++)
    {
        sx = 1;
        sy = 1;
        if (k == 1 || k == 3)
            sy = -1;
        if (k >= 2)
            sx = -1;
        self linkto(v, "", (a * sx, b * sy, c), (0, 0, 0));
        wait 0.1;
        want = P + (v.origin - vo0);
        e = distance(self.origin, want);
        if (e < beste)
        {
            beste = e;
            bestk = k;
        }
        self unlink();
    }
    r = coop_ping(8000 + bestk * 10 + int(beste / 10));
    if (bestk < 0 || beste > 120)
    {
        self.coop_vwait = gettime() + 5000;
        return;
    }
    sx = 1;
    sy = 1;
    if (bestk == 1 || bestk == 3)
        sy = -1;
    if (bestk >= 2)
        sx = -1;
    // own place: the owner's screenshot showed the puppet overlapping a seated soldier; pulled toward the centre line (the aisle). A shift
    // backwards put it behind the stern of the short landing boat (owner); the owner then asked for a little forward along the facing direction (+40)
    a2 = a + 40;
    b2 = b * 0.2;
    self.coop_veh = v;
    self linkto(v, "", (a2 * sx, b2 * sy, c), (0, 0, 0));
    wait 0.3;
    // diagnostics for tuning (bridge log): distance puppet-player, then the three offsets (+20000 keeps them positive)
    r = coop_ping(9000 + int(distance(self.origin, level.player.origin)));
    r = coop_ping(20000 + int(a2 * sx));
    r = coop_ping(20000 + int(b2 * sy));
    r = coop_ping(20000 + int(c));
}

coop_local_offset(veh, world)
{
    d = world - veh.origin;
    fw = anglestoforward(veh.angles);
    rt = anglestoright(veh.angles);
    up = anglestoup(veh.angles);
    return (vectordot(d, fw), 0 - vectordot(d, rt), vectordot(d, up));
}

coop_char_kind()
{
    m = getcvar("mapname");
    if (m == "duhoc_assault" || m == "duhoc_defend" || m == "hill400_assault" || m == "hill400_defend" || m == "silotown_assault" || m == "bergstein" || m == "rhine" || m == "newvillers")
        return "american";
    if (m == "beltot" || m == "breakout" || m == "crossroads")
        return "british_n";
    if (m == "libya" || m == "elalamein" || m == "eldaba" || m == "toujane" || m == "toujane_ride" || m == "matmata" || m == "88ridge" || m == "decoytown" || m == "decoytrenches")
        return "british_a";
    return "russian";
}

coop_precache_character()
{
    k = coop_char_kind();
    if (k == "american")
        character\american_ranger_normandy::precache();
    else if (k == "british_n")
        character\british_infantry_normandy::precache();
    else if (k == "british_a")
        character\british_infantry_africa::precache();
    else
        character\russian_winter_trench_volskey::precache();
}

coop_dress(p)
{
    k = coop_char_kind();
    if (k == "american")
        p character\american_ranger_normandy::main();
    else if (k == "british_n")
        p character\british_infantry_normandy::main();
    else if (k == "british_a")
        p character\british_infantry_africa::main();
    else
        p character\russian_winter_trench_volskey::main();
}

coop_is_attached(model)
{
    n = self getattachsize();
    for (i = 0; i < n; i++)
    {
        if (self getattachmodelname(i) == model && self getattachtagname(i) == "tag_weapon_right")
            return true;
    }
    return false;
}

coop_precache_weapons()
{
    precacheString(&"COOP_PARTNER");
    precacheString(&"COOP_WAITING");
    gk = coop_char_kind();
    if (gk == "british_a" || gk == "british_n")
        precacheItem("mk1britishfrag");
    else if (gk == "russian")
        precacheItem("rgd-33russianfrag");
    else
        precacheItem("fraggrenade");
    precacheItem("potato");
    precacheItem("stielhandgranate");
    level.coop_fx_rifle = loadfx("fx/muzzleflashes/rifleflash");
    if (getcvar("mapname") == "libya" || getcvar("mapname") == "88ridge" || getcvar("mapname") == "tankhunt")
    {
        // The real tank effects. The impact one is chosen per mission, exactly as the game's own *_fx.gsc
        // files do: tank_impact_libya on libya and on 88ridge (ridge88_fx.gsc), tank_impact_dirt on
        // duhoc_assault, tank_impact_night on the night maps. A generic dirt impact everywhere looked grey
        // where the host's shell kicks up yellow sand.
        level.coop_fx_tankflash = loadfx("fx/muzzleflashes/cruisader_flash");
        level.coop_fx_tankdust = loadfx("fx/dust/tank_dust_blowback_libya");
        level.coop_fx_tankdist = loadfx("fx/distortion/distortion_tank_muzzleflash");
        // the engine of the partner's tank (the game's own crusader aliases, looping: idle / moving) and the dust of its tracks
        level.coop_tank_snd = [];
        level.coop_tank_snd[0] = "crusader_idle_high";
        level.coop_tank_snd[1] = "crusader_engine_high";
        if (getcvar("mapname") == "libya" || getcvar("mapname") == "88ridge" || getcvar("mapname") == "ridge88")
            level.coop_fx_tread = loadfx("fx/dust/tread_dust_libya");
        else
            level.coop_fx_tread = loadfx("fx/dust/tread_dust_brown");
        if (getcvar("mapname") == "libya" || getcvar("mapname") == "88ridge" || getcvar("mapname") == "ridge88")
            level.coop_fx_tankhit = loadfx("fx/explosions/tank_impact_libya");
        else if (getcvar("mapname") == "decoytown" || getcvar("mapname") == "decoytrenches")
            level.coop_fx_tankhit = loadfx("fx/explosions/tank_impact_night");
        else
            level.coop_fx_tankhit = loadfx("fx/explosions/tank_impact_dirt");
    }
    if (getcvar("mapname") == "libya" || getcvar("mapname") == "88ridge")
        precacheModel("xmodel/vehicle_crusader2_static");
    if (getcvar("mapname") != "duhoc_assault")
    {
        level.coop_fx_boom = loadfx("fx/explosions/grenadeExp_dirt");
        level.coop_fx_boom_hard = loadfx("fx/explosions/grenadeExp_blacktop");   // the real grenade picks its effect by the surface: asphalt is smaller than the dirt cloud
        // the effect the real smoke grenade plays (projExplosionEffect of weapons/sp/smoke_grenade_*): one per nationality, only the one that is needed
        if (gk == "british_a" || gk == "british_n")
            level.coop_fx_smoke = loadfx("fx/props/brit_smoke_grenade");
        else
            level.coop_fx_smoke = loadfx("fx/props/american_smoke_grenade");
        // the darker body of the smoke (gfx_dark_smoke_atlas): the owner found the cloud too light with the props effect alone
        level.coop_fx_smoke2 = loadfx("fx/explosions/smoke_grenade");
    }
    if (gk == "british_a" || gk == "british_n")
        precacheItem("smoke_grenade_british");
    else if (gk == "russian")
        precacheItem("smoke_grenade_russian");
    else
        precacheItem("smoke_grenade_american");
    // the flying copy of the partner's smoke grenade uses the model of the real thrown one (the weapon model kept spinning after the landing)
    precacheModel("xmodel/projectile_us_smoke_grenade");
    // ONE flash effect only: a level has a small, fixed number of effect slots (duhoc_assault ran out of them with three: "G_FindConfigstringIndex: overflow")
    level.coop_weps = [];
    level.coop_weps[0] = "mosin_nagant";
    level.coop_weps[1] = "mosin_nagant_sniper";
    level.coop_weps[2] = "ppsh";
    level.coop_weps[3] = "pps42";
    level.coop_weps[4] = "tt30";
    level.coop_weps[5] = "svt40";
    level.coop_weps[6] = "kar98k";
    level.coop_weps[7] = "mp40";
    level.coop_weps[8] = "mp44";
    level.coop_weps[9] = "g43";
    level.coop_weps[10] = "luger";
    level.coop_weps[11] = "colt";
    level.coop_weps[12] = "m1garand";
    level.coop_weps[13] = "thompson";
    level.coop_weps[14] = "m1carbine";
    level.coop_weps[15] = "bar";
    level.coop_weps[16] = "bren";
    level.coop_weps[17] = "enfield";
    level.coop_weps[18] = "enfield_scope";
    level.coop_weps[19] = "g43_sniper";
    level.coop_weps[20] = "kar98k_sniper";
    level.coop_weps[21] = "panzerfaust";
    level.coop_weps[22] = "panzerschreck";
    level.coop_weps[23] = "springfield";
    level.coop_weps[24] = "sten";
    level.coop_weps[25] = "thompson_latewar";
    level.coop_weps[26] = "webley";
    // mounted machine guns (ids 28-32): only recognised, never precached here (a level with such a turret precaches them); the puppet shoots them with the bren
    level.coop_weps[27] = "mg42_bipod_stand";
    level.coop_weps[28] = "mg42_bipod_duck";
    level.coop_weps[29] = "mg42_bipod_prone";
    level.coop_weps[30] = "30cal_stand";
    level.coop_weps[31] = "30cal_prone";
    for (i = 0; i < 27; i++)
        precacheItem(level.coop_weps[i]);
    precacheModel("xmodel/weapon_mg42");
}

coop_weapon_id(name)
{
    for (i = 0; i < level.coop_weps.size; i++)
    {
        if (level.coop_weps[i] == name)
            return i + 1;
    }
    return 0;
}

coop_mounted_mg()
{
    // 1 while the local player operates a placed machine gun: getcurrentweapon() keeps returning the weapon he carried, so the turret entities are asked
    // (misc_mg42 / misc_turret, getturretowner()); checked five times a second, the turret list is refreshed every 4 seconds
    if (isdefined(level.coop_mgnext) && gettime() < level.coop_mgnext)
        return level.coop_mgon;
    level.coop_mgnext = gettime() + 200;
    if (!isdefined(level.coop_mgref) || gettime() >= level.coop_mgref)
    {
        level.coop_mgref = gettime() + 4000;
        level.coop_mglist = [];
        a = getentarray("misc_mg42", "classname");
        for (i = 0; i < a.size; i++)
            level.coop_mglist[level.coop_mglist.size] = a[i];
        a = getentarray("misc_turret", "classname");
        for (i = 0; i < a.size; i++)
            level.coop_mglist[level.coop_mglist.size] = a[i];
    }
    level.coop_mgon = 0;
    for (i = 0; i < level.coop_mglist.size; i++)
    {
        t = level.coop_mglist[i];
        if (!isdefined(t))
            continue;
        o = t getturretowner();
        if (isdefined(o) && o == level.player)
        {
            level.coop_mgon = 1;
            break;
        }
    }
    return level.coop_mgon;
}
"@
# the puppet shows the weapon the peer really holds (table above; precached at level start)
$net7Body = $net7Body -replace 'precacheItem\("ppsh"\);', ('precacheItem("ppsh");' + [Environment]::NewLine + '    coop_precache_weapons();')
$net7Body += @"

coop_flag_sync()
{
    // Mission flags (sending is on unless the console variable coop_flags is "off"; receiving is always on): (level.flag[name], set by flag_set in maps/_utility.gsc,
    // which this mod patches to call coop_flag_local) are mirrored to the partner. This thread sets the flags the partner reported.
    // A flag that was cleared locally after being set is a toggle: never set from outside.
    while (!isdefined(level.player))
        wait 0.1;
    if (!isdefined(level.coop_flr))
        level.coop_flr = [];
    if (!isdefined(level.coop_flbad))
        level.coop_flbad = [];
    if (!isdefined(level.coop_flwarn))
        level.coop_flwarn = [];
    if (!isdefined(level.coop_flwait))
        level.coop_flwait = [];
    pend = [];
    pendt = [];
    for (;;)
    {
        wait 0.25;
        // receiving is always on: flags only arrive when the partner sends them (coop_flags "off" there stops the sending)
        n = coop_pop_flag();
        while (n != "")
        {
            if (n == "coop_missionfailed")
            {
                // the partner failed the mission (not died): the death event that comes with it shows the fail screen here instead of killing this player
                level.coop_remote_fail = gettime();
            }
            else if (n == "entered trench" || n == "started charge")
            {
                // the trench sequence spawns the allies of the partner's area: a world that plays it while its own player is elsewhere crashes
                // ("not on a friendly chain"): these flags are set only by this player's own entry
                coop_dbg("COOPTRG flag " + n + " ignored (own entry only)");
            }
            else if (!coop_trigger_fire(n))
            {
                pend[pend.size] = n;
                pendt[pendt.size] = gettime();
            }
            n = coop_pop_flag();
        }
        np = [];
        npt = [];
        for (j = 0; j < pend.size; j++)
        {
            pn = pend[j];
            keep = 0;
            if (isdefined(level.flag) && isdefined(level.flag[pn]) && !isdefined(level.coop_flbad[pn]))
            {
                if (!level.flag[pn])
                {
                    // like the triggers: the world of a player who is far from the partner must not run ahead of him; the flag waits (10 min at most)
                    if (distance(coop_peer_state(), level.player.origin) > 1500 && gettime() - pendt[j] < 600000)
                    {
                        keep = 1;
                        // said once per flag: this hold used to be silent (a flag that did not take effect looked lost)
                        if (!isdefined(level.coop_flwait[pn]))
                        {
                            level.coop_flwait[pn] = 1;
                            coop_dbg("COOPTRG flag " + pn + " waits (this player is far from the partner; it is set when he comes within 1500 units)");
                        }
                    }
                    else
                    {
                        level.coop_flr[pn] = 1;
                        level.flag[pn] = true;
                        level notify(pn);
                        coop_flag_applied(pn);
                    }
                }
                else
                    coop_dbg("COOPTRG flag " + pn + " already set here: nothing to do");
            }
            else if (gettime() - pendt[j] < 600000)
            {
                keep = 1;
                // a received flag that this level does not declare (an unknown name, or declared later): say so once, it used to vanish without a line
                if (gettime() - pendt[j] > 5000 && !isdefined(level.coop_flwarn[pn]))
                {
                    level.coop_flwarn[pn] = 1;
                    coop_dbg("COOPTRG flag " + pn + " unknown here (no such flag in this level yet; kept for 10 minutes)");
                }
            }
            else
                coop_dbg("COOPTRG flag " + pn + " dropped: this level never declared it in 10 minutes");
            if (keep)
            {
                np[np.size] = pn;
                npt[npt.size] = pendt[j];
            }
        }
        pend = np;
        pendt = npt;
    }
}

coop_trigger_watch()
{
    // Trigger mirror (experiment, opt-in: console variable coop_triggers "on" on BOTH PCs; coop_debug 1 only logs "COOPTRG" lines).
    // Every trigger of the level gets a waiting thread. When the LOCAL PLAYER fires one, its key (class + origin) travels to the partner through
    // the flag channel, and the partner's script fires the trigger with the same key (trig notify("trigger", player)): its objectives, scenes and
    // spawns then advance too. A trigger fired this way is not sent back.
    dbg = 0;
    if (getcvarint("coop_debug") == 1)
        dbg = 1;
    mir = 0;
    if (getcvar("coop_triggers") == "on")
        mir = 1;
    if (!dbg && !mir)
        return;
    while (!isdefined(level.player))
        wait 0.1;
    wait 2;
    cls = [];
    cls[0] = "trigger_multiple";
    cls[1] = "trigger_once";
    cls[2] = "trigger_use";
    cls[3] = "trigger_radius";
    cls[4] = "trigger_lookat";
    cls[5] = "trigger_damage";
    cls[6] = "trigger_use_touch";
    cc = [];
    cc[0] = "m";
    cc[1] = "o";
    cc[2] = "u";
    cc[3] = "r";
    cc[4] = "l";
    cc[5] = "d";
    cc[6] = "t";
    level.coop_trig = [];
    level.coop_trigk = [];
    level.coop_trigb = [];
    // triggers whose effect shuts the partner out (the lid of the pipe in trainyard closes behind the player and the NPC that closes it is unloaded):
    // such a key is NEVER fired from the partner's message: the lid closes on this PC only when its own player fires the trigger (walks in)
    level.coop_gate = [];
    level.coop_gate[0] = "TRG_mtrigger_inpipe";
    level.coop_gate[1] = "TRG_mtrigger_trench";
    // downtown_sniper: these scenes use level.soldier1 / level.volsky / level.soldier4, actors that exist only after the local player has been to their spawners;
    // fired from the partner's message they ran on undefined actors (52000 script errors on the second PC, the objective loop never ended)
    level.coop_gate[2] = "TRG_mtrigger_floorclear";
    level.coop_gate[3] = "TRG_mhelmet_moment";
    // triggers that spawn or steer FRIENDLY actors (friendly chains, waves, floods): mirrored, they spawn the allies of the PARTNER's area in this PC's world,
    // and the engine stops with "Sentient actor_ally_... is following the sentient player ... that is not on a friendly chain" (guest crash in trainyard 22:42:10).
    // Such a trigger (by targetname) is neither sent nor fired.
    level.coop_deny = [];
    level.coop_deny[0] = "friendly_wave";
    level.coop_deny[1] = "flood_spawner";
    level.coop_deny[2] = "trigger_friendlyvolume";
    level.coop_deny[3] = "friendly_forcetrigger";
    level.coop_deny[4] = "new_friendlies";
    level.coop_deny[5] = "friendly_spawn";
    level.coop_deny[6] = "escortchain";
    level.coop_deny[7] = "delayed_spawner";
    level.coop_deny[8] = "drone_allies";
    level.coop_deny[9] = "flood_and_secure";
    n = 0;
    for (c = 0; c < cls.size; c++)
    {
        a = getentarray(cls[c], "classname");
        for (i = 0; i < a.size; i++)
        {
            o = a[i].origin;
            nm = "";
            if (isdefined(a[i].targetname))
                nm = a[i].targetname;
            // point triggers (radius) are named by their origin; brush triggers have the origin 0 0 0 (every one would get the same key),
            // so they are named by their targetname; a brush trigger without a targetname cannot be named and is not mirrored
            // (several brush triggers can share an origin such as 0 0 -10000, so a brush trigger is named by its targetname when it has one)
            key = "";
            hasorg = 0;
            if (o[0] != 0 || o[1] != 0 || o[2] != 0)
                hasorg = 1;
            if (c == 3 && hasorg)
                key = "TRG_" + cc[c] + coop_trig_num(o[0]) + "_" + coop_trig_num(o[1]) + "_" + coop_trig_num(o[2]);
            else if (nm != "")
                key = "TRG_" + cc[c] + nm;
            else if (hasorg)
                key = "TRG_" + cc[c] + coop_trig_num(o[0]) + "_" + coop_trig_num(o[1]) + "_" + coop_trig_num(o[2]);
            // several triggers share a targetname (26 "objective_hint" triggers in one level): the n-th one with the same key gets "_n"
            // (the entity order of a level is the same on both PCs)
            if (key != "")
            {
                same = 0;
                for (q = 0; q < n; q++)
                {
                    if (level.coop_trigb[q] == key)
                        same++;
                }
                level.coop_trigb[n] = key;
                if (same > 0)
                    key = key + "_" + (same + 1);
            }
            else
                level.coop_trigb[n] = "";
            for (d = 0; d < level.coop_deny.size; d++)
            {
                if (nm == level.coop_deny[d])
                    key = "";
            }
            level.coop_trig[n] = a[i];
            level.coop_trigk[n] = key;
            thread coop_trigger_one(a[i], key, dbg, mir, cls[c], nm);
            n++;
        }
    }
    coop_dbg("COOPTRG watching " + n + " triggers, mirror " + mir);
    thread coop_enemy_count();
}

coop_enemy_count()
{
    // the number of living actors of each side every 10 s (coop_debug 1): the two PCs' logs show how far their worlds differ
    for (;;)
    {
        wait 10;
        ax = getaiarray("axis");
        al = getaiarray("allies");
        coop_dbg("COOPEN axis " + ax.size + " allies " + al.size);
    }
}

coop_trig_num(v)
{
    k = int(v);
    if (k < 0)
        return "n" + (0 - k);
    return "" + k;
}

coop_trigger_one(trig, key, dbg, mir, cn, nm)
{
    for (;;)
    {
        trig waittill("trigger", who);
        isme = 0;
        if (isdefined(who) && isdefined(level.player) && who == level.player)
            isme = 1;
        echo = 0;
        if (isdefined(trig.coop_rt) && gettime() - trig.coop_rt < 1500)
            echo = 1;
        // only the FIRST firing of a trigger is mirrored: zone checks that fire every second (indoors / outdoors) stay local, scene starts happen once.
        // (the stock script may delete the trigger right after it fires: nothing below reads a field of the trigger except the one we set first)
        first = 1;
        if (isdefined(trig) && isdefined(trig.coop_done))
            first = 0;
        if (isdefined(trig))
            trig.coop_done = 1;
        if (mir && isme && !echo && first && key != "")
            coop_send_flag(key);
        if (dbg && first)
        {
            if (nm == "")
                nm = "-";
            coop_dbg("COOPTRG " + key + " " + cn + " name=" + nm + " player=" + isme + " remote=" + echo + " t=" + gettime());
        }
        return;
    }
}

coop_trigger_wait_near(trig, key)
{
    // up to 10 minutes; dropped when the local player fires the trigger himself
    for (n = 0; n < 1200; n++)
    {
        if (!isdefined(trig))
            return;
        if (isdefined(trig.coop_done))
            return;
        if (distance(coop_peer_state(), level.player.origin) <= 1500)
        {
            trig.coop_rt = gettime();
            trig notify("trigger", level.player);
            coop_dbg("COOPTRG released " + key + ": this player came near the partner, fired");
            return;
        }
        wait 0.5;
    }
}

coop_trigger_gated(trig, key)
{
    // up to 10 minutes: the key is dropped as soon as the local player has fired the trigger himself (it is never fired from the partner's message)
    for (n = 0; n < 1200; n++)
    {
        if (!isdefined(trig))
            return;
        if (isdefined(trig.coop_done))
        {
            coop_dbg("COOPTRG released " + key + ": fired here by this player");
            return;
        }
        // (no release by distance: the pipe of trainyard starts right beside the spawn, so "near the partner" is also "outside the pipe" - the lid must close
        // for this PC only when its own player walks in)
        wait 0.5;
    }
}

coop_trigger_refire(trig, key)
{
    // opt-in (coop_trig_retry "on"): the script of this PC may not have been waiting for the trigger yet when it was fired (a notify is not kept),
    // so it is fired once more after 3 s
    wait 3;
    if (isdefined(trig))
    {
        trig.coop_rt = gettime();
        trig notify("trigger", level.player);
        coop_dbg("COOPTRG refire " + key);
    }
}

coop_enemysync_trigger(trig, key)
{
    // coop_enemysync (default on, "off" in the profile switches it off): a trigger that spawns ENEMIES only (every spawner it reaches is axis, none is allied) is
    // fired at once from the partner's message, also when this PC's player is far from the partner: the enemies of the partner's area then exist on this PC
    // too (their deaths are mirrored by the kill events). Triggers that spawn allies keep the distance rule (friendly chains: see coop_deny).
    // The spawners are found by the CLASS of the entities the trigger targets (actor_axis_* / actor_ally_*; getspawnerarray().team was not set on them),
    // following a chain of triggers two steps deep.
    if (getcvar("coop_enemysync") == "off")
        return 0;
    if (!isdefined(trig))
        return 0;
    if (!isdefined(trig.target))
    {
        coop_dbg("COOPTRG " + key + " not enemy-only: the trigger has no target (it only starts a script or sets a flag)");
        return 0;
    }
    r = coop_enemy_targets(trig.target, 0);
    if (r[0] > 0 && r[1] == 0)
    {
        coop_dbg("COOPTRG " + key + " spawns " + r[0] + " enemy spawners only: fired at once (coop_enemysync)");
        return 1;
    }
    coop_dbg("COOPTRG " + key + " not enemy-only: target " + trig.target + ", axis spawners " + r[0] + " allied " + r[1] + ", entities " + r[2] + ", first class " + r[3]);
    return 0;
}

coop_enemy_targets(name, depth)
{
    // [0] axis spawners, [1] allied spawners, [2] entities found, [3] the class of the first one
    r = [];
    r[0] = 0;
    r[1] = 0;
    r[2] = 0;
    r[3] = "-";
    e = getentarray(name, "targetname");
    r[2] = e.size;
    for (k = 0; k < e.size; k++)
    {
        if (!isdefined(e[k]) || !isdefined(e[k].classname))
            continue;
        cn = e[k].classname;
        if (k == 0)
            r[3] = cn;
        if (issubstr(cn, "actor_axis"))
            r[0] = r[0] + 1;
        else if (issubstr(cn, "actor_ally"))
            r[1] = r[1] + 1;
        else if (depth < 2 && isdefined(e[k].target) && issubstr(cn, "trigger"))
        {
            q = coop_enemy_targets(e[k].target, depth + 1);
            r[0] = r[0] + q[0];
            r[1] = r[1] + q[1];
            r[2] = r[2] + q[2];
        }
    }
    return r;
}

coop_trigger_fire(key)
{
    if (!isdefined(level.coop_trigk))
        return 0;
    fired = 0;
    found = 0;
    sdone = 0;
    for (i = 0; i < level.coop_trigk.size; i++)
    {
        if (key != "" && level.coop_trigk[i] == key && isdefined(level.coop_trig[i]))
        {
            found++;
            if (isdefined(level.coop_trig[i].coop_done))
                sdone++;
            else
            {
                gated = 0;
                for (g = 0; g < level.coop_gate.size; g++)
                {
                    if (level.coop_gate[g] == key)
                        gated = 1;
                }
                if (gated)
                {
                    thread coop_trigger_gated(level.coop_trig[i], key);
                    coop_dbg("COOPTRG held " + key + " (never fired from the partner; only this player's own entry fires it)");
                }
                else if (distance(coop_peer_state(), level.player.origin) > 1500 && !coop_enemysync_trigger(level.coop_trig[i], key))
                {
                    // this PC's player is far from the partner (still at the spawn, say): the script of this world must not run ahead of him - the
                    // allies it spawns for the partner's area try to follow a player who is not on their friendly chain and the engine stops
                    // ("Sentient actor_ally_... is following the sentient player ... that is not on a friendly chain", guest crashes in trainyard).
                    // The key waits until this player is within 1500 units of the partner or fires the trigger himself.
                    thread coop_trigger_wait_near(level.coop_trig[i], key);
                    coop_dbg("COOPTRG waits " + key + " (this player is far from the partner)");
                }
                else
                {
                    level.coop_trig[i].coop_rt = gettime();
                    level.coop_trig[i] notify("trigger", level.player);
                    fired = 1;
                    if (getcvar("coop_trig_retry") == "on")
                        thread coop_trigger_refire(level.coop_trig[i], key);
                }
            }
        }
    }
    if (fired)
        coop_flag_applied(key);
    else if (found > 0)
        coop_dbg("COOPTRG recv " + key + " alreadyfired=" + sdone + " of " + found);
    else if (!isdefined(level.flag) || !isdefined(level.flag[key]))
        coop_dbg("COOPTRG recv " + key + " unknown here (no trigger, no flag)");
    return found;
}

coop_health_hud()
{
    // the partner's state is shown by the HUD menu next to the compass (tools/patch_menu.py patches ui/hud.menu): it reads these dvars,
    // so it hides and shows together with the compass
    setcvar("coop_partner_on", "0");
    setcvar("coop_partner_low", "0");
    setcvar("coop_partner", "");
    lastseq = -1;
    lastt = 0;
    shown = 0;
    lasthp = -1;
    for (;;)
    {
        wait 0.1;
        // the partner died: the survivor dies too, so the engine itself shows the original death screen and then reloads the checkpoint (both PCs hold
        // the host's checkpoint in every file it may load). The death is marked as induced: it is not reported back to the partner.
        if (coop_pop_peer_death())
        {
            wait 0.45;
            if (isdefined(level.coop_remote_fail) && gettime() - level.coop_remote_fail < 4000)
            {
                // the partner FAILED the mission: this player does not die, the screen greys out with the engine's own fail screen and the shared checkpoint is loaded
                level.coop_remote_fail = undefined;
                level.coop_induced = 1;
                coop_log("COOPFAIL the partner failed the mission: the engine's fail screen here (no death)");
                maps\_utility::missionFailedWrapper();
                coop_death_started();
                continue;
            }
            level.coop_induced = 1;
            for (kt = 0; kt < 6; kt++)
            {
                if (isdefined(level.player.health) && level.player.health > 0)
                    level.player doDamage(level.player.health + 1000, level.player.origin);
                wait 0.5;
                if (!isdefined(level.player.health) || level.player.health <= 0)
                    break;
            }
            if (!isdefined(level.player.health) || level.player.health <= 0)
                coop_death_started();
        }
        sq = coop_peer_seq();
        if (sq != lastseq)
        {
            lastseq = sq;
            lastt = gettime();
        }
        // hidden at once while the local player is dead (the compass goes with the death screen) and 2 s after the partner's packets stop
        ownhp = level.player.health;
        if (!isdefined(ownhp))
            ownhp = 1;
        if (ownhp <= 0 || gettime() - lastt > 2000)
        {
            if (shown)
            {
                setcvar("coop_partner_on", "0");
                setcvar("coop_partner", "");   // the value item has no on-test of its own: blank it so the figure never stays without its label
                lasthp = -1;
                shown = 0;
            }
            continue;
        }
        hp = coop_peer_health();
        if (hp != lasthp)
        {
            lasthp = hp;
            setcvar("coop_partner", hp + "%");
            if (hp <= 30)
                setcvar("coop_partner_low", "1");
            else
                setcvar("coop_partner_low", "0");
        }
        if (!shown)
        {
            setcvar("coop_partner_on", "1");
            shown = 1;
        }
    }
}

coop_level_watch()
{
    for (;;)
    {
        wait 0.5;
        s = coop_pending_level();
        if (s != "")
            changelevel(s, false);
    }
}

coop_key_alias(k)
{
    // downtown_sniper: the enemy sniper stands in one of five windows, chosen at random on EACH PC (script_noteworthy building1_floor1 on one PC,
    // building2_floor3 on the other); the five names mean the same character, so they share one key
    if (k == "building1_floor1" || k == "building1_floor2" || k == "building1_floor3" || k == "building2_floor1" || k == "building2_floor3")
        return "enemy_sniper";
    return k;
}

coop_vehicle_watch()
{
    // map-placed enemy machines that an objective counts (the tanks and the 88s of 88ridge / libya): both PCs have the same entities in the same order, so
    // "group name + index" names one of them on both PCs. When one dies here the partner's copy is blown up too (test switch coop_keykill "on").
    while (!isdefined(level.player))
        wait 0.1;
    level.coop_vkeys = [];
    wait 3;
    grp = [];
    grp[0] = "enemytank";
    grp[1] = "flak88s";
    cnt = 0;
    for (gi = 0; gi < grp.size; gi++)
    {
        ents = getentarray(grp[gi], "script_noteworthy");
        for (i = 0; i < ents.size; i++)
        {
            key = grp[gi] + "_" + i;
            level.coop_vkeys[key] = ents[i];
            ents[i] thread coop_vehicle_death(key);
            cnt++;
        }
    }
    if (cnt > 0)
        coop_log("COOPVEH tracking " + cnt + " map machines (enemytank, flak88s)");
}

coop_vehicle_death(key)
{
    self waittill("death");
    if (isdefined(self.coop_remote))
        return;
    if (getcvar("coop_keykill") != "on")
        return;
    if (!isdefined(self))
        return;
    coop_send_kill_key(self.origin, key);
    coop_log("COOPVEH destroyed here: " + key);
}

coop_npc_key(e)
{
    // the name a script gives a single character (script_noteworthy of its spawner, else the targetname); "" = none
    k = "";
    if (isdefined(e.script_noteworthy))
        k = e.script_noteworthy;
    else if (isdefined(e.targetname))
        k = e.targetname;
    return coop_key_alias(k);
}

coop_key_seen(k, e)
{
    // diagnostics of the kill-by-key test: every distinct key is written once to coop5.log
    if (k == "" || isdefined(level.coop_kseen[k]))
        return;
    level.coop_kseen[k] = 1;
    tn = "-";
    sn = "-";
    an = "-";
    if (isdefined(e.targetname))
        tn = e.targetname;
    if (isdefined(e.script_noteworthy))
        sn = e.script_noteworthy;
    if (isdefined(e.animname))
        an = e.animname;
    coop_dbg("COOPKEY seen key=" + k + " targetname=" + tn + " script_noteworthy=" + sn + " animname=" + an);
}

coop_key_pending_hit(e, k)
{
    // a kill-by-key that arrived before this character was spawned here: it dies as soon as it appears (60 s at most)
    for (j = 0; j < level.coop_kpend.size; j++)
    {
        if (level.coop_kpend[j] == k && gettime() - level.coop_kpendt[j] < 60000)
        {
            level.coop_kpend[j] = "";
            e.coop_remote = true;
            e dodamage(e.health + 500, e.origin);
            coop_dbg("COOPKEY pending kill applied at its spawn: " + k);
            return;
        }
    }
}

coop_enemy_watch()
{
    while (!isdefined(level.player))
        wait 0.1;
    ents = [];
    poss = [];
    keys = [];
    level.coop_keymax = [];
    level.coop_kseen = [];
    if (!isdefined(level.coop_kpend))
    {
        level.coop_kpend = [];
        level.coop_kpendt = [];
    }
    for (;;)
    {
        wait 0.25;
        // test switch: kill events of single named characters carry their key (coop_keykill "on" on BOTH PCs)
        kon = 0;
        if (getcvar("coop_keykill") == "on")
            kon = 1;
        ne = [];
        np = [];
        nk = [];
        for (i = 0; i < ents.size; i++)
        {
            e = ents[i];
            if (isdefined(e) && isalive(e))
            {
                ne[ne.size] = e;
                np[np.size] = e.origin;
                nk[nk.size] = keys[i];
            }
            else if (isdefined(e))
            {
                if (!isdefined(e.coop_remote))
                {
                    k = keys[i];
                    // only a key that never had two living characters at once is a single character (a crowd shares its key)
                    if (kon && k != "" && isdefined(level.coop_keymax[k]) && level.coop_keymax[k] == 1)
                        coop_send_kill_key(poss[i], k);
                    else
                        coop_send_kill(poss[i]);
                }
            }
        }
        ents = ne;
        poss = np;
        keys = nk;
        ai = getaiarray("axis");
        cnt = [];
        for (i = 0; i < ai.size; i++)
        {
            k = coop_npc_key(ai[i]);
            if (k != "")
            {
                if (!isdefined(cnt[k]))
                    cnt[k] = 0;
                cnt[k] = cnt[k] + 1;
            }
        }
        for (i = 0; i < ai.size; i++)
        {
            k = coop_npc_key(ai[i]);
            if (k != "" && (!isdefined(level.coop_keymax[k]) || cnt[k] > level.coop_keymax[k]))
                level.coop_keymax[k] = cnt[k];
            if (!isdefined(ai[i].coop_tracked))
            {
                ai[i].coop_tracked = true;
                ents[ents.size] = ai[i];
                poss[poss.size] = ai[i].origin;
                keys[keys.size] = k;
                if (kon)
                {
                    coop_key_seen(k, ai[i]);
                    if (k != "")
                        coop_key_pending_hit(ai[i], k);
                }
            }
        }
    }
}

coop_kill_recv()
{
    while (!isdefined(level.player))
        wait 0.1;
    if (!isdefined(level.coop_kpend))
    {
        level.coop_kpend = [];
        level.coop_kpendt = [];
    }
    for (;;)
    {
        wait 0.1;
        n = coop_kill_count();
        while (n > 0)
        {
            v = coop_pop_kill();
            kk = coop_kill_key();
            n = n - 1;
            kon = 0;
            if (getcvar("coop_keykill") == "on")
                kon = 1;
            if (kon && kk != "")
            {
                if (isdefined(level.coop_vkeys) && isdefined(level.coop_vkeys[kk]))
                {
                    ve = level.coop_vkeys[kk];
                    if (isdefined(ve) && !isdefined(ve.coop_remote))
                    {
                        ve.coop_remote = true;
                        ve notify("death");
                        coop_log("COOPVEH kill by key applied: " + kk);
                    }
                    continue;
                }
                // test: the kill names a single character: kill the living one with that key here (no distance, no position fallback)
                hits = 0;
                hit = undefined;
                ai = getaiarray("axis");
                for (i = 0; i < ai.size; i++)
                {
                    if (coop_npc_key(ai[i]) == kk)
                    {
                        hits++;
                        hit = ai[i];
                    }
                }
                if (hits == 1)
                {
                    hit.coop_remote = true;
                    hit dodamage(hit.health + 500, v);
                    coop_dbg("COOPKEY kill by key applied: " + kk);
                }
                else if (hits == 0)
                {
                    level.coop_kpend[level.coop_kpend.size] = kk;
                    level.coop_kpendt[level.coop_kpendt.size] = gettime();
                    coop_dbg("COOPKEY kill by key waits, nobody with this key is alive here yet: " + kk);
                }
                else
                    coop_dbg("COOPKEY kill by key ambiguous here (" + hits + " alive with this key), nothing killed: " + kk);
                continue;
            }
            radius = 220;
            test = 0;
            if (v[0] == 0 && v[1] == 0 && v[2] == 0)
            {
                v = level.player.origin;
                radius = 2500;
                test = 1;
            }
            if (coop_peer_solo() && !test)
                continue;
            best = undefined;
            bd = radius;
            ai = getaiarray("axis");
            for (i = 0; i < ai.size; i++)
            {
                d = distance(ai[i].origin, v);
                if (d < bd)
                {
                    bd = d;
                    best = ai[i];
                }
            }
            if (isdefined(best))
            {
                best.coop_remote = true;
                best dodamage(best.health + 500, v);
            }
        }
    }
}
"@
# heartbeat: the bridge sends a packet every 40 ms even when the peer stands still, so coop_peer_seq() not changing for 5 s means the link is
# gone: the puppet is hidden (reversible, no respawn) and shown again when packets return
$net7Body = $net7Body -replace '(?m)^(\s*)(lastx = rx;)(\r?\n\s*lasty = ry;\r?\n\s*still = 0;\r?\n\s*i = 0;)', "`$1lastsq = coop_peer_seq();`n`$1quiet = 0;`n`$1hidden = 0;`n`$1`$2`$3"
$net7Body = $net7Body -replace '(?m)^(\s*)ry = pv\[1\];(\r?\n\s*\r?\n\s*if \(rx != lastx)', "`$1ry = pv[1];`n`$1sq = coop_peer_seq();`n`$1if (sq != lastsq)`n`$1{`n`$1    lastsq = sq;`n`$1    quiet = 0;`n`$1    if (hidden)`n`$1    {`n`$1        puppet show();`n`$1        hidden = 0;`n`$1        iprintln(`"COOPN peer is back`");`n`$1    }`n`$1}`n`$1else`n`$1{`n`$1    quiet = quiet + 1;`n`$1    if (quiet == 100 && !hidden)`n`$1    {`n`$1        puppet hide();`n`$1        hidden = 1;`n`$1        iprintln(`"COOPN peer lost`");`n`$1    }`n`$1}`$2"

$net7Body = $net7Body -replace 'lastsq = coop_peer_seq\(\);', "lastsq = coop_peer_seq();`n    lastst = 0;"
$net7Body = $net7Body -replace 'if \(rx != lastx \|\| ry != lasty\)', "pst = coop_peer_stance();`n        go = 0;`n        if (rx != lastx || ry != lasty || pst >= 3 || lastst >= 3)`n            go = 1;`n        lastst = pst;`n        if (go)"

$coop = if ($Probe -eq 'puppet') { $puppetBody }
        elseif ($Probe -eq 'follow') { $followBody }
        elseif ($Probe -eq 'char') { $charBody }
        elseif ($Probe -eq 'net') { $netBody }
        elseif ($Probe -eq 'net2') { $net2Body }
        elseif ($Probe -eq 'net3') { $net3Body }
        elseif ($Probe -eq 'net4') { $net4Body }
        elseif ($Probe -eq 'net5') { $net5Body }
        elseif ($Probe -eq 'net6') { $net6Body }
        elseif ($Probe -eq 'net7') { $net7Body }
        else { $probeBody }
Write-Host "probe variant: $Probe"

# The animation budget: a script may mention at most 28 distinct %animation names (the engine refuses a dynamic animation tree of size greater than 32:
# the T-pose of 0.7.6 came from 31 names). The check runs on the script that is about to be injected, so a clip added by mistake stops the build.
$animNames = @([regex]::Matches($coop, '%([A-Za-z][A-Za-z0-9_]*)') | ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique)
Write-Host ("animation names mentioned by the co-op script: {0} (limit 28)" -f $animNames.Count)
if ($animNames.Count -gt 28) { throw ("the co-op script mentions {0} distinct animation names, the limit is 28 (more breaks the animation tree: T-pose): {1}" -f $animNames.Count, ($animNames -join ', ')) }

# Diagnostic lines ("COOPT shot ...", "COOPN ...") reach console.log through iprintln, which also prints them on the
# player's screen. In the shipped mod they are silent: with the test switch coop_debug 1 coop_dbg writes a "script: ..." line to coop5.log (never to the screen).
if ($Probe -eq 'net7') {
    $coop = $coop -replace 'iprintln\("COOP', 'coop_dbg("COOP'
    $coop += @"

coop_dbg(msg)
{
    if (getcvarint("coop_debug") == 1)
        coop_log(msg);
}
"@
}

$final = $patched + $coop

# every playable single-player level (not movies, slideshows, credits)
$allLevels = @('88ridge','beltot','bergstein','breakout','cityhall','crossroads','decoytown','decoytrenches','demolition','downtown_assault','downtown_sniper','duhoc_assault','duhoc_defend','elalamein','eldaba','hill400_assault','hill400_defend','libya','matmata','moscow','newvillers','rhine','silotown_assault','tankhunt','toujane','toujane_ride','trainyard')
if ($Levels -contains 'all') { $Levels = $allLevels }
$entries = [ordered]@{}
$entries[$Level] = $final
foreach ($lv in $Levels) {
    if ($lv -eq $Level) { continue }
    $o = Get-FromIwd $GameDir "maps/$lv.gsc"
    $m = [regex]::Match($o, '(?m)^main\s*\(\s*\)')
    if (-not $m.Success) { Write-Warning "no main() in $lv.gsc: skipped"; continue }
    $entries[$lv] = $o.Remove($m.Index, $m.Length).Insert($m.Index, 'coop_orig_main()') + $coop
}
# the stock maps/_utility.gsc with two hooks: flag_set reports a new flag to the partner (coop_flag_local), flag_clear marks it as a toggle
$util = Get-FromIwd $GameDir "maps/_utility.gsc"
$um = [regex]::Match($util, '(?s)flag_set\s*\(msg\)\s*\{\s*level\.flag\[msg\]\s*=\s*true;\s*level notify\s*\(msg\);')
$uc = [regex]::Match($util, '(?s)flag_clear\s*\(msg\)\s*\{\s*level\.flag\[msg\]\s*=\s*false;\s*level notify\s*\(msg\);')
if ($um.Success -and $uc.Success) {
    $util = $util.Insert($uc.Index + $uc.Length, "`n`tcoop_flag_cleared(msg);")
    $util = $util.Insert($um.Index + $um.Length, "`n`tcoop_flag_local(msg);")
    $util += @"

coop_flag_local(msg)
{
	if (getcvar("coop_flags") == "off")
		return;
	if (isdefined(level.coop_flr) && isdefined(level.coop_flr[msg]))
		return;
	if (isdefined(level.coop_flbad) && isdefined(level.coop_flbad[msg]))
		return;
	coop_send_flag(msg);
}

coop_flag_cleared(msg)
{
	if (!isdefined(level.coop_flbad))
		level.coop_flbad = [];
	level.coop_flbad[msg] = 1;
}
"@
    # a mission FAILED by its script (a protected ally died, an objective timer ran out, an ally was shot): the engine reloads only this PC. The partner is
    # told with the same event as a death (he is killed by the script, both load the shared checkpoint).
    $uf = [regex]::Match($util, '(?s)missionFailedWrapper\s*\(\s*\)\s*\{\s*if\s*\(\s*level\.missionfailed\s*\)\s*return;\s*level\.missionfailed\s*=\s*true;')
    if ($uf.Success) {
        $util = $util.Insert($uf.Index + $uf.Length, "`n`tcoop_mission_failed();")
        $util += @"

coop_mission_failed()
{
	if (!isdefined(level.coop_evc))
		return;
	if (isdefined(level.coop_induced))
		return;
	level.coop_failreq = 1;
	coop_send_flag("coop_missionfailed");
	coop_log("COOPFAIL the mission failed here: the partner is told (the same event as a death)");
}
"@
    } else { Write-Warning "maps/_utility.gsc: missionFailedWrapper not found, a failed mission reloads only one PC" }
    $entries['_utility'] = $util
} else { Write-Warning "maps/_utility.gsc: flag_set / flag_clear not found, the flag mirror has no sender" }
Write-Host ("levels in the mod: " + ($entries.Keys -join ' '))

$modDir = Join-Path $TestDir 'mods\coop'
New-Item -ItemType Directory -Force -Path $modDir | Out-Null
$iwdPath = Join-Path $modDir "z_coop$Suffix.iwd"
if (Test-Path $iwdPath) { Remove-Item $iwdPath -Force }

# Two delivery options, same payload:
#   1. mods\coop\z_coop.iwd  + `+set fs_game mods/coop`   (needs launch arguments)
#   2. main\zz_coop.iwd       - an archive loaded after the stock iw_*.iwd, so its
#      maps/<level>.gsc overrides the stock one with no cvars and no arguments at all.
#      The game sorts main\*.iwd by name, so "zz_" must stay last alphabetically.
function Write-Iwd([string]$path, $entries) {
    $zip = [IO.Compression.ZipFile]::Open($path, [IO.Compression.ZipArchiveMode]::Create)
    try {
        foreach ($k in $entries.Keys) {
            $entry = $zip.CreateEntry("maps/$k.gsc", [IO.Compression.CompressionLevel]::Optimal)
            $s = $entry.Open()
            $w = New-Object IO.StreamWriter($s, (New-Object Text.UTF8Encoding($false)))
            $w.Write($entries[$k]); $w.Flush(); $w.Close()
        }
        # extra files (the in-game menu: ui/main.menu, ui/menus.txt, ui/coop.menu), stored at their relative path
        if ($script:ExtraDir) {
            Get-ChildItem $script:ExtraDir -Recurse -File | ForEach-Object {
                $rel = $_.FullName.Substring($script:ExtraDir.Length).TrimStart('\') -replace '\\', '/'
                $x = $zip.CreateEntry($rel, [IO.Compression.CompressionLevel]::Optimal)
                $xs = $x.Open()
                $bytes = [IO.File]::ReadAllBytes($_.FullName)
                $xs.Write($bytes, 0, $bytes.Length); $xs.Close()
            }
        }
    } finally { $zip.Dispose() }
}

# net5 (the release) carries the in-game Co-op menu: the CO-OP button in the main menu and its screen, patched from
# the game's own menu files by tools\patch_menu.py (every anchor asserted: a changed stock file fails the build)
$script:ExtraDir = $null
if ($Probe -eq 'net5' -or $Probe -eq 'net6' -or $Probe -eq 'net7') {
    $menuOut = Join-Path $env:TEMP ("cod2coop-menu-" + [Guid]::NewGuid().ToString('N'))
    $pmArgs = @(); if ($MainMenuOnly) { $pmArgs += '--mainonly' }
    . (Join-Path $PSScriptRoot 'patch_menu.ps1')
    Invoke-PatchMenu -Game $GameDir -Out $menuOut -MainOnly ([bool]$MainMenuOnly)
    # the CO-OP screen texts are NOT in this archive: main\zz_coop_lang.iwd carries the file of the game's language (tools\make_lang_iwd.py)
    if (-not $Install) { python -I (Join-Path $PSScriptRoot 'make_lang_iwd.py') $GameDir $TestDir }   # the installer writes the language archive itself
    # the own animation tree of the puppet (stock xanim clips, our own list: prone, crawl, rechamber, jump ... inside the engine's limit of 32 top-level nodes)
    New-Item -ItemType Directory -Force -Path (Join-Path $menuOut 'animtrees') | Out-Null
    Copy-Item (Join-Path $PSScriptRoot 'animtrees\coop_puppet.atr') (Join-Path $menuOut 'animtrees\coop_puppet.atr')
    if ($Bank2) { Copy-Item (Join-Path $PSScriptRoot 'animtrees\coop_puppet2.atr') (Join-Path $menuOut 'animtrees\coop_puppet2.atr') }   # the second tree (bank 2 test, dev only)
    $script:ExtraDir = $menuOut
}

if ($Probe -eq 'net7' -and $Bank2) {
    $bank2gsc = @'
#using_animtree("coop_puppet2");

setup(ent)
{
    ent useAnimTree(#animtree);
}

play(ent, id)
{
    // the pistol clips of the game's own soldiers (generic_human): the pt_ clips belong to the player-body skeleton and stand tilted on the puppet
    if (id == 1)
        ent setflaggedanimknobrestart("coop_p2", %pistol_crouchaim_idle, 1, 0.2, 1);
    else if (id == 3)
        ent setflaggedanimknobrestart("coop_p2", %pistol_crouch_reload, 1, 0.1, 1);
    else if (id == 4)
        ent setflaggedanimknobrestart("coop_p2", %boat_pistol_stand_idle, 1, 0.2, 1);
    else if (id == 5)
        ent setflaggedanimknobrestart("coop_p2", %pistol_stand_reload, 1, 0.1, 1);
    else if (id == 6)
        ent setflaggedanimknobrestart("coop_p2", %pistol_crouchrun_loop_forward_1, 1, 0.2, 1);
    else if (id == 7)
        ent setflaggedanimknobrestart("coop_p2", %pistol_crouchshoot_straight, 1, 0.05, 1);
    else if (id == 8)
        ent setflaggedanimknobrestart("coop_p2", %pistol_standshoot_straight, 1, 0.05, 1);
    // a mounted machine gun (the gunner clips of the game's own AI): stand and prone, aim and fire
    else if (id == 9)
        ent setflaggedanimknobrestart("coop_p2", %standmg42gunner_aim_forward_breathingidle, 1, 0.2, 1);
    else if (id == 10)
        ent setflaggedanimknobrestart("coop_p2", %standmg42gunner_fire_forward_level, 1, 0.05, 1);
    else if (id == 11)
        ent setflaggedanimknobrestart("coop_p2", %pronemg42gunner_aim_forward_level, 1, 0.2, 1);
    else if (id == 12)
        ent setflaggedanimknobrestart("coop_p2", %pronemg42gunner_fire_forward_level, 1, 0.05, 1);
}

length(id)
{
    if (id == 3)
        return int(getanimlength(%pistol_crouch_reload) * 1000);
    if (id == 5)
        return int(getanimlength(%pistol_stand_reload) * 1000);
    if (id == 6)
        return int(getanimlength(%pistol_crouchrun_loop_forward_1) * 1000);
    return 600000;
}
'@
    $entries['coop_anims2'] = $bank2gsc
}
Write-Iwd $iwdPath $entries

$mainIwd = Join-Path $TestDir "main\zz_coop$Suffix.iwd"
New-Item -ItemType Directory -Force -Path (Split-Path $mainIwd) | Out-Null   # the installer builds into a fresh folder
if (Test-Path $mainIwd) {
    try { Remove-Item $mainIwd -Force }
    catch { Write-Warning "cannot replace $mainIwd (the game holds it open); build with a new -Suffix instead" }
}
Write-Iwd $mainIwd $entries
if ($script:ExtraDir) { Remove-Item $script:ExtraDir -Recurse -Force -ErrorAction SilentlyContinue }   # the scratch menu files

$sha = (Get-FileHash $iwdPath -Algorithm SHA256).Hash
Write-Host "mod iwd: $iwdPath"
Write-Host "size:    $((Get-Item $iwdPath).Length) bytes"
Write-Host "sha256:  $sha"
Write-Host "game:    $((Get-FileHash (Join-Path $GameDir 'CoD2SP_s.exe') -Algorithm SHA256).Hash)"

# steam_appid.txt lets the CEG wrapper accept a start outside the Steam client.
Set-Content -Path (Join-Path $TestDir 'steam_appid.txt') -Value '2630' -Encoding ASCII
Write-Host "steam_appid.txt written"
