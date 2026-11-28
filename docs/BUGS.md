# Open bugs - Call of Duty 2 co-op

Every entry has a reproduction and the log line that shows it. Build identity:
`CoD2SP_s.exe` sha256 `AB17A89CC3722AE80EDC1F4483CACBCAE08B0373B9128FE4EC053EBC79062281`.

---

## Open bug index 2026-11-15 (the list below it is the long history; read this part first)

| id | symptom | repro / evidence | status |
|---|---|---|---|
| B9 | libya on the guest PC: hundreds of `script runtime error` lines `entity 1 is not a script_vehicle` (`maps/libya.gsc` line 202, `self getspeedmph()`) | guest `console.log`, `logs/evidence-2026-11-15.md` E4; the guest's two reports of the same session disagree (1539 error lines vs "0 errors"), the log is not truncated between sessions | origin **unresolved**; assumed: the stock `player_tank_rumble` thread of the level's own tank runs on after the swap. Check: new guest libya session with a fresh `console.log`; fix idea: stop/delete the abandoned tank after the swap |
| B10 | the host skipped its start barrier on `duhoc_assault`; the guest (it had skipped the briefing and was already waiting) timed out after 20 s | host `coop5.log` has no `barrier: begin` for `duhoc_assault`, guest `barrier: GO (timeout, no partner report)` 13:53:54 (E2) | open; assumed cause: `coop_link_active()` (host) needs a state packet in 5 s or a hello in 10 s; a waiting guest sends only `R`/`V`. Fix idea: refresh the liveness stamp on `R` and `V` packets |
| B11 | first failed checkpoint transfer (`checkpoint sent: libya ... FAILED`) at 12:51:10 | guest `coop5.log` (E7) | open; assumed: the receiver was in a level change |
| B12 | the host sends about half as many state packets as the guest (`tx=17919 rx=8112`) | guest stats line 12:57:26 (E7) | open; assumed: packets stop while the game is paused (barrier/load) |
| smoke grenade | the partner's smoke grenade keeps spinning after landing | owner, 2026-11-14/15 | open |
| grenade damage | damage of the partner's frag grenade to enemies never confirmed (no enemy within 260 units in any test) | `COOPG enemy at distance` lines, owner test 2026-11-14 | unverified |
| tank visuals | partner tank: static turret, tracks move while it stands, shot effect is a stand-in | owner 2026-11-15 | open, after the release |
| standing animation | owner queued a rework of the standing pose (no description yet) | owner 2026-11-15 | queued |
| B4, B6 | savegame written under the original script hangs; changing the video mode crashes | see the long history below | open (workarounds) |

## B1 - the puppet does not move in the world  (CLOSED 2026-11-13 17:27)

**Root cause:** `puppet.origin = (...)` does not relocate a `script_model` in this build. The script
field is written and reads back, so the write looked successful while the renderer never followed it.
Moving with the engine's own `moveTo()` works.

**Fix:** the `net` variant in `tools/make_mod.ps1` moves with `puppet moveTo((x, y, playerZ), 0.2)` -
the same mover the shipped scripts use (109 call sites).

**Evidence**
- `logs/bridge-net-move-20261113-172724.log`: 500 packets received and applied, values changing
  (`apply 500 (pkt 500): -2093.88 -722.41 101.60 str='-2093.881'`).
- Owner, in game, 17:27: "да перемещается по кругу" - the soldier follows the packet circle.
- The read path (`getcvarfloat` on a string we rewrote in place) is therefore **valid**; the earlier
  suspicion that the engine parses a separate float cache was wrong for this build.

**What also had to be fixed to get here**
- The GSC variant that creates and reads `coop_px/py/pz` was missing from the repo; without it the
  failing build could not be rebuilt. It is now the `net` variant of `tools/make_mod.ps1`.
- Re-injecting into a running game is impossible with the same DLL name: `LoadLibrary` returns the
  existing handle and `DllMain` does not run again. Worse, the first copy's UDP thread keeps the port
  bound after its work thread died ("cvars not resolved") and silently eats the packets. The bridge now
  reads its port from `coop_port.txt`, and a second copy can be injected under a new file name.
- `cl` could not overwrite the injected `cod2net5.dll` (locked by the running game) and `build.ps1`
  reported success anyway. It now fails loudly and tells you to build under another name.

---

## B1 (history) - original report and the two hypotheses

**Report:** owner, 2026-11-13: "он не двигается в мире" (it does not move in the world).

**Repro**
1. `switch-install.ps1 -Mode test`, launch via `steam://rungameid/2630`, Mission Select -> Red Army Training.
2. Inject `bridge\cod2net5.dll`.
3. `python tools\send_live.py` (UDP 127.0.0.1:27100, 20 Hz, circle).

**Observed:** the second soldier exists with a head but stays where it is. The bridge log proved the
values changed:

```
pkt 1100: -2046.542 -748.066 101.600
apply 1100 (pkt 1100): -2046.54 -748.07 101.60 str='-2046.542' foff=-1
```

**Not established at the time:** whether the game's script saw the new value, or saw it but did not
move the entity.

**Two candidate causes** (as written before the fix)
1. `getcvarfloat()` does not re-parse `cvar->string` (a parsed-float side table only the engine's own
   setter updates). We rewrote the string directly.
2. `puppet.origin = (...)` does not refresh a `script_model`'s render position; CoD2 normally moves such
   entities with `moveTo()` / `moveX()` / `moveZ()`.

**Outcome:** cause 2 was correct, cause 1 was not. Both are worth keeping in mind for other titles.

---

## B7 - the GSC readback channel is empty, so removal cannot be verified

The `net2` variant writes what it read into `coop_rx` with `setcvar("coop_rx", "" + rx)`. The bridge
resolves that cvar and logs it, but always reads `''`:

```
gsc readback coop_rx='' (cvar string now '-2248.155')
```

**Cause (verified by the log):** GSC string concatenation of an empty string with a float does not
produce a number, and `setcvar` stores the empty result. The cvar itself is fine - it is a valid cvar_t
at a sane address, next to `coop_px/py/pz`.

**Consequence:** the only channel for GSC-side state is `iprintln`, which reaches the screen and, with
`logfile 2`, `console.log`. The test profile has no `logfile`, so "did the puppet delete itself" was
judged by eye - the same mistake that produced the wrong B1 conclusion.

**Fix to try next (cheap):** copy the string instead of converting it -
`setcvar("coop_rx", getcvar("coop_px"))`; write literal markers (`setcvar("coop_rx", "removed")`)
before `delete()`. Then every branch is visible in `coop5.log` without touching the console.

**Outcome (2026-11-13): closed.** The string-copy theory above was only part of it. The bridge could not
find `coop_rx` at all: `ResolveCvar` required a numeric string at `+8`, but the cvar holds words
(`none`, `spawned`, `removed`), so `readback cvar` was `00000000` and every readback was `''`. Allowing
text found the cvar, but it also matched copies of the bridge's own log text in memory (they contain a
quote); those are now skipped. The real struct is `007863AC` (same `0x24` stride after `coop_pz`).
Evidence from the live process: `[27105] gsc readback coop_rx='removed'`. The owner also saw the puppet
disappear. Traps found on the way: a stale bridge instance keeps its UDP port and logs freed memory, so
use a new port per injection (log lines are now tagged `[port]`); only a game restart gives a clean process.
Not shown yet: one clean run of spawn, movement and removal read through a single tagged instance.

**Second candidate for the missing removal (not needed, removal works):** `hide()` on a `script_model` is used by shipped scripts
(`wall_after hide();`), but if it throws on this entity type the thread dies before `delete()`.

---

## B2 - T-pose, no animation

`script_model` carries no animation tree; `makeFakeAI()` is accepted by the engine
(`COOP8 step4 makeFakeAI ok`) but adds none, so the body renders in bind pose. The head attaches fine,
which proves the model has a skeleton.

**Route:** spawn a real AI actor from an `actor_*` entity instead of a `script_model`, or find the engine
call that gives a fake actor an animation tree.

**Outcome (2026-11-13): mostly closed, variant `net3`.** The tree is bound explicitly, the way the shipped
`maps\_drones.gsc` does it: `#using_animtree("fakeshooters");` before the function, then
`puppet useAnimTree(#animtree);` and a thread that loops
`self setFlaggedAnimKnob("coop_idle", %stand_alert_1); self waittillmatch("coop_idle", "end");`.
`setFlaggedAnimKnob` leaves the origin alone, so `moveTo` still drives the puppet. The level loads
without a script error. Owner, watching the game (not a log): the puppet moves in a circle with an idle
animation, but the rifle points at the ground and the right elbow is higher than the left. That is
probably the pose itself (`stand_alert_*` is the drones' alert idle with a lowered weapon); not checked.
No log evidence for animation: GSC cannot report which anim plays.

**Update (`net4`, 2026-11-13): works, with a live pose selector.** The GSC polls the cvar `coop_anim`
(created as `"0000000000"`) and restarts the matching animation; `python tools\send_anim.py N --port P`
makes the bridge write it, so poses are compared without restarting the game. Owner's observations
(watching the game, no log): 0 `stand_alert_1` weapon lowered; 1 `stand_alert_2` calm idle with small
movements (good default); 2 `stand_alert_3` stands, weapon at low ready, looks around; 3
`stand_aim_straight` and 4 `crouch_aim_straight` hold the weapon cleanly; 5 `stand_shoot_auto_straight`
fires in bursts standing; 7 `crouch_shoot_auto_straight` crouches and fires (jerky, loops fast); 8
`reload_stand_rifle` plays once and freezes in an odd pose (a rifle reload on a PPSh); 6
`stand_shoot_straight` shows no change (guess: an additive single-shot anim, not checked). Default: 1.

A wrong turn worth keeping: for a while poses did not switch at all. I blamed pose 8 killing the GSC
thread; the real cause was the bridge. `ResolveCvar` in text mode returned a copy of its own log line
(it had no quote in it) and `coop_anim` was written into that, not into the cvar. `coop_anim` is always
digits, so it is now resolved in numeric mode.

What fixed the rest:
- *No weapon in the hands.* The aim poses put the hands on a rifle, but a bare `script_model` has none.
  `precacheItem("ppsh")` in `main()` plus `puppet attach(getWeaponModel("ppsh"), "tag_weapon_right")`,
  the same call as the shipped `drone_allies_assignWeapon`.
- *"Elevator" and puppet floating.* The ground `bulletTrace` had no ignore-entity, so with a target
  inside the puppet's body (standing still) the trace hit the puppet's head and the puppet climbed.
  The in-loop trace now ignores the puppet. `send_live.py --still` stands in one place with a 0.3 unit
  wobble, because the GSC removes a puppet whose position has not changed for 30 s.
- *Trap in the bridge.* The first `coop_anim` write went into a string inside the exe image
  (`0056CEF8`, the shared literal `"0"`) and briefly turned it into `3`. The bridge now refuses to
  write below `0x01000000` and the GSC creates the cvar as a 10-char heap string.

---

## B3 - the puppet floats above the ground  (CLOSED 2026-11-13 17:36, ground trace in the net2 variant, owner-confirmed)

The sender transmits a constant `z` (`101.600`), so the model hangs where the terrain is lower (visible in
the owner's screenshot above a truck). **Fix:** send the player's z (already logged by the probe) or trace
the ground in GSC with `bullettrace`.

---

## B4 - loading a savegame written under the original script hangs the level

**Repro:** replace `maps/moscow.gsc` via `main\zz_coop.iwd`, then load an autosave produced before the
replacement (`main\players\123\save\autosave\moscow-1.svg`). The level load stops.

**Evidence:** `logs/freeze-console-1558.log` ends at the load stage
(`dvar set ui_campaign`, `g_gameskill 1`, `sv_cheats 0`, `dvar set nextmap`) and the process reports
`Responding=False`. Frame frozen, main thread waiting, no script error printed.

**Workaround used:** start missions fresh, clear `sv_lastSaveGame`, keep autosaves out of the test install.
**Not proven:** whether a save written *with* the patched script loads correctly - that must be tested after
B1, not assumed.

---

## B5 - a diagnostic DLL crashed the game (fixed practice, kept as a warning)

**Evidence:** WER event 1000 at 2026-11-13 16:33:57, faulting module `cod2diag.dll`, exception `0xc0000005`
inside a full-memory scan that dereferenced pointers without SEH guards. For a few minutes this was
mistaken for the owner closing the game.

**Rule:** diagnostics must scan narrowly (a unique value string), read in chunks with a guarded copy, and
never write before a second invariant confirms the candidate struct. Assume any diagnostic DLL can kill the
game - check WER before blaming the owner.

---

## B6 - crash when changing the video mode in-game (environment)

**Report:** owner, 2026-11-13 17:16: "вылет после изменения разрешения на full hd".

**Context:** the **original** install, no mod archive, no injected DLL, fresh profile `12312`.
So this is an environment/doctor issue, not our code.

**Evidence**
- Event 1000: `CoD2SP_s.exe` faulting module `gfx_d3d_x86_s.dll_unloaded`, `0xc0000005`, offset `0x343d9`.
- Dump `%LOCALAPPDATA%\CrashDumps\CoD2SP_s.exe.42520.dmp` (51.9 MB, 17:16:19), parsed with
  `tools/minidump_scan.py`:
  - exception address `0x0EED43D9` = **old** renderer mapping `0x0EEA0000` + `0x343D9`;
  - the live module list has `gfx_d3d_x86_s.dll` at `0x0DC70000` - i.e. it was **unloaded and reloaded
    at a different base**, and the crash happened in the mapping that no longer exists;
  - 11 stack words still point into the dead range (`0x0EED4350`/`0x0EED43D9`), plus `nvd3dum.dll`
    (NVIDIA user-mode D3D driver) frames.
- Display: 1920x1080 **@200 Hz**; profile config had `r_mode "1024x768"`, `r_fullscreen "1"`,
  `r_displayRefresh "60 Hz"`, `r_aaSamples "4"`.

**Diagnosis:** switching the video mode makes the engine restart its renderer: it unloads
`gfx_d3d_x86_s.dll` and loads it again, while something still executes in (or calls into) the old image.
Use-after-unload, not a content bug.

**Workaround (until proven otherwise)**
- Never change the video mode from the in-game menu during tests. Set the mode before launch.
- `r_mode "1920x1080"` + `r_fullscreen "1"` is the combination verified working on this machine
  (session 1); `1024x768 / 800x600 / 640x480` fullscreen are known to fail here.
- Windowed (`r_fullscreen 0`) avoids the mode switch entirely and is the sane default for two-copy runs.
- The desktop runs at 200 Hz while the game asks for `60 Hz`; mismatched refresh is a candidate
  aggravating factor, not yet isolated.

**Not established:** whether the crash reproduces on a clean profile with a mode that the driver
advertises, and whether the 200 Hz desktop matters.

---

## Closed / not bugs

| item | why it is not a bug |
|---|---|
| `Application error` -> "Failed to find Steam." | CEG requires a Steam-created process; launch through Steam |
| `Run In Safe Mode?` after a kill | CoD2 reacts to the unclean exit; quit through the menu |
| game pauses when it loses focus | engine behaviour; needs a focus guard for two copies |
| `PrintWindow` returns black | the game draws through `D3DProxyWindow` (DXGI flip); capture the foreground screen |
| `DirectInputCreate*` fails on this host | CoD2 never uses DirectInput (no `dinput` imports) |

---

## Open items after v0.9.3 (not bugs yet, each needs a test)

| item | repro / how to check | status |
|---|---|---|
| script channel `coop_publish_state` / `coop_peer_*` | probe `net6`/`net7`, JOIN 127.0.0.1, `tx ... (script channel)` in `coop5.log` | verified on one PC 2026-11-13 (23:38, 23:46); two PCs untested |
| auto discovery across Radmin between two PCs | guest PC: JOIN with `auto`; log `discovery: host found at ...` | verified 2026-11-14 00:07 (guest forwarded log: JOIN to the host's Radmin address) |
| crash/hang report in a real game, false hang during level load | load a level, look for `coop_hang_*.txt` | untested |
| a script call landing in the ~1 s live-reload gap | swap the bridge file while GSC calls in a loop | untested |
| puppet lag while moving 0.5-0.7 s | measure with `stats` RTT lines and the mirror test | owner's estimate only |
| borderless window: back buffer 1920x1071 stretched to 1920x1080 | cosmetic; mouse may be off by up to 9 px at the bottom edge | owner saw no problem |
| menu status line (`coop_status`, "Status:" in the CO-OP screen) lags after HOST/JOIN/STOP | owner 2026-11-13 ~00:00: still a noticeable delay after the polls were cut to 0.2-0.3 s and the dvars are found by the engine lookup; the bridge log shows the command applied at once, so the remaining delay is between the bridge write and the menu redraw, or in the dvar being created only at the first click (`coop_cmd`/`coop_ip` are not found before it) | OPEN, owner: "decide later" |
| peer lost / back (heartbeat, puppet hidden after 5 s without packets, shown again) | `coop_mute.txt` in the game folder freezes the peer state for the script (log "debug: coop_mute.txt present/removed"); owner saw the puppet disappear and return with the "COOPN peer lost / is back" lines, 2026-11-13 23:51-23:52 | verified by the owner on one PC; a real disconnect between two PCs untested |
| guest status line says "no answer from the host" while the guest is still in the menu (no mission, so nothing is sent) | seen 2026-11-14 00:07-00:10, guest log "waiting for a mission" | fixed in 0.9.6: "JOIN: host found, start a mission" (not yet seen in the game) |
| host status said "waiting for the guest" although the guest had found it and was waiting for the mission | owner 2026-11-14 ~00:20: the host learned the guest only from its first pose packet | fixed in the bridge (next release, built, **untested on two PCs**): the guest sends "H 1" every second after JOIN until its first pose packet, the host learns the address from it and shows "HOST: guest found, start a mission" |

---

## Puppet actions: stand-ins that MUST be replaced later (owner, 2026-11-14: "good enough for now")

The puppet uses the `fakeshooters` animation tree (only tree that works on this model; `generic_human` gives a T-pose, cause unknown). That tree has no jump, prone, throw, melee or crouch-walk animation, so:

| action | what it does now | what is wrong | replace with |
|---|---|---|---|
| jump (stance 3) | idle pose, the puppet's height follows the peer's real height | no animation, looks like a lift | a real jump animation (find one that works on this model/tree) |
| prone (stance 2) | idle pose, model pitched 75 degrees about the feet | only roughly lying | a prone animation (`prone_*` exist in `generic_human`) |
| crouch while moving | crouch pose, the puppet slides | no crouch-walk | `crouchrun_loop_forward` (`generic_human` / `drones` tree) |
| melee (event type 1) | short lunge forward and back with an aim pose | not a rifle-butt strike | `aim2melee_right` / `melee_*` |
| grenade throw (event type 2) | arm-up aim pose, then `self magicgrenade(...)` from the puppet's hand after 0.3 s, real grenade, 2.5 s fuse | no throwing arm motion; in a real two-PC game it lands where the peer's throw direction points and can hurt the local player | `grenader_throw` / `crouch_grenade_throw` |

**Cause of the generic_human T-pose (found 2026-11-14 in `main\console.log`): "Cannot dynamically use an anim tree of size greater than 32" - a script cannot `useAnimTree` a big tree (generic_human has 2624 names) on an entity; the call raised a runtime error in the pose thread, so no animation was ever set.** Only small trees work on a script_model (fakeshooters, drones, ...). None of the small trees has prone, throw or melee, so the real fix is to spawn a real friendly actor as the puppet (the engine gives actors the big trees statically) (ENGINE.md, "Consequence for enemy awareness"). A live animation tester exists: `anim 10..35` over UDP to the bridge plays one `fakeshooters` animation on the puppet (list in `logs/` notes: 10 stand_aim_up ... 35 stand_alert_3); numbers >= 10 in `coop_anim` are a debug override in `coop_pose_move`.

### Facts learned on the way (all verified by console log / game)
- Script errors are written to `main\console.log` only if `logfile 2` is set **in the profile** (`seta logfile "2"` in the test profile's `config.cfg`); a script may then NOT call `setcvar("logfile", ...)` (runtime error "Internal Dvars cannot be changed by script"). The log is flushed when the game exits.
- `magicgrenade(start, end, fuse)` is an entity METHOD (`guy magicgrenade(...)`); calling it as a function = "unknown function".
- `getentarray("grenade", "classname")` returns live grenades; `grenade_fire` is NOT delivered to the player in SP; `getammocount` for grenades did not change on a throw.
- `getstance()` works in SP ("stand"/"crouch"/"prone"); airborne is detected with a short `bulletTrace` below the player.
- In PowerShell here-strings a replacement written with `` `n `` inside SINGLE quotes puts a literal backtick-n into the script (compile error).

### TODO (owner, 2026-11-14): new animations if no good existing ones can be found

If prone, jump, throw and melee cannot be got from existing animations (a script_model can only use small trees; an actor puppet is the other route), author our own: write a generator for CoD2 `xanim` files (bone rotations per frame for the soldier skeleton; the format must be reverse-engineered from the game's own xanim files and the decompiled loader, there is no converter in the Steam install), register the names by shipping our own text `animtrees/fakeShooters.atr` in `zz_coop.iwd`, and play them like any other anim of that tree. Cheap stop-gap for the jump: blend existing anims with weights (`setanim` weights). Estimated about a day for the packer plus one animation.

### Facts from the all-levels work (2026-11-14)
- Steam asks the user to confirm launch arguments (`+map x`) every time, so levels cannot be started by URL unattended; level changes are done from inside a running mission (`tools/level_walk.py`: UDP `level <name>` to the bridge, the script calls `changelevel(name, false)`). A level load ends at a "click to continue" screen the player must click.
- The engine dvar `ClickToContinue` (checked in the mouse poll 0x44F0A0) makes the engine post a mouse click EVERY frame, not only on the loading screen: never leave it set (it makes the menus unusable).
- `MagicGrenade only supports actors.` (runtime error on a script_model): no thrown grenade from the puppet, ever, without a real actor.
- `getattachsize` / `getattachmodelname` / `getattachtagname` are used to attach/detach the puppet's weapon only when needed ("failed to detach" / "already attached" are runtime errors that kill the thread).
- Open: vehicle levels (tank, car, plane): `level.player` is the vehicle's occupant; the puppet there is placed on the ground trace and stands, the stance codes (`isonground`) read "air". Needs: detect "player is in a vehicle" and show the peer differently (hide, or a vehicle model).

### TODO (owner, 2026-11-14): weapon-dependent animations

The weapon in the puppet's hands is now correct (27 weapons, empty hands when unknown), but the poses are always the rifle poses of the `fakeshooters` tree:
- **No weapon:** needs an unarmed idle/run (the puppet now stands with empty hands in a rifle pose).
- **Moving with a pistol:** needs pistol idle and run animations (`pistol_*` / `pb_*pistol*` exist in `generic_human` and `multiplayer` trees, which a script_model cannot use; see the anim tree size limit in ENGINE.md). Same route as the other missing animations: an actor puppet, or our own xanim files.

### Vehicles (2026-11-14)
Detection: `coop_player_link()` (bridge, reads entity 0 + 0x1C8; logs the parent entity and classname on every change). Seen: decoytown links the player to a `script_vehicle` (entity 1095) for about 27 s. The survey of the other vehicle levels (toujane_ride, eldaba, matmata, toujane, beltot, demolition, duhoc_assault) was started and stopped; the link calls found in the stock scripts: playerlinktodelta/absolute to a ride rig or vehicle tag (decoytown "tag_playerride", eldaba "tag_guy05", matmata "tag_player_passenger", toujane "tag_playerride", toujane_ride "tag_player"), linkto a plane tag (demolition), a cut-scene mover (duhoc_assault, moscow). Open: the puppet takes the peer's offset relative to the nearest vehicle in OUR world; if the two worlds' vehicles are not at the same place the puppet is linked to the wrong vehicle or floats; deleting a non-key NPC rider and taking his seat (owner's idea) is not done.

### TODO (owner, 2026-11-14): vehicle seating, try better variants later
The puppet is inside the player's vehicle (linked with the player's own local offset plus the test shift), but it stands in the idle pose at an arbitrary spot of the vehicle. Better variants to try later: (1) take the seat of a non-key NPC rider (owner's idea): find the AI linked to that vehicle, delete it (never key or named characters), put the puppet on its tag (`tag_guyNN`); (2) use the peer's own tag name (read from the engine link node at +8/+0xC, `tag_playerride` etc.) with a small side offset so the puppet sits next to the real player; (3) a sitting pose if one can be found/made (own xanim, see the animation TODO); (4) check every riding level one by one (decoytown is the only one verified): toujane_ride, eldaba, matmata, toujane, beltot, demolition (plane), duhoc_assault (cut-scene mover: no vehicle).

### Ride survey and owner observations (2026-11-14)
`tools/ride_walk.py` (change level, wait, read the link events) over the riding levels, bridge log:
- decoytown: linked to `script_vehicle` 1095 (27 s), puppet rode inside the vehicle (owner: OK).
- eldaba: `script_vehicle` 741, matmata: `script_vehicle` 52, toujane: `script_vehicle` 971 - puppet link calibrated (variant 1, error < 10) and released on unlink.
- toujane_ride: the player is already in the truck when the level starts (only the unlink at the end is seen). Owner's screenshot: the puppet overlapped a seated soldier -> the puppet now takes an own place: 70 units farther back along the vehicle and pulled toward the centre line (aisle).
- beltot, demolition: no link in the first 90 s (the ride is later in the mission; not verified). duhoc_assault: first the landing boat (`script_vehicle` 4), later a cut-scene `script_model` (1574).
- duhoc_assault landing boat (owner): before the landing the puppet did not exist for him, after the landing it "teleported" to him. Cause: the puppet's spawn point comes from a ground trace, over water that is the sea bed, and the first ride link took that wrong point as the place. Fixed: the link takes the peer's REPORTED position and the parent is searched by entity number among script_vehicle / script_model / script_origin / script_brushmodel (no fallback to another entity). Owner after the fix: the puppet is in the boat behind him.
- toujane, sitting on the tank armour (owner): the puppet hangs in the air to the right of the player. Open: the tank is a different seat type (the offset/local axes of a turret-mounted ride); fix later.

### TODO (owner, 2026-11-14): the puppet sits at the CENTRE of the vehicle and needs polishing
State accepted by the owner: while the peer is in or on a vehicle the puppet is linked to that vehicle and stands near its centre (the offset is the peer's own place pulled toward the centre line, +40 units along the facing direction: `coop_ride()` in `tools/make_mod.ps1`). It is inside the vehicle on decoytown (distance puppet-player 43 units, local offsets 7 / 22 / height in the bridge log), in the landing boat of duhoc_assault, on the truck of toujane_ride. Needs polishing: a real seat per vehicle type (tag of a seat or a non-key NPC's place), a sitting pose, no clipping into soldiers or the hull, the tank-armour ride of toujane (the puppet hangs in the air to the right of the player), and verification of beltot, demolition and the later duhoc_assault rides. Diagnostics stay in the script: after every ride link the bridge log shows `coop_ping(9000 + distance puppet-player)` and `coop_ping(20000 + offset)` for the three local offsets.

### Ride pass, level by level (owner + bridge log, 2026-11-14 late)
| level | what was checked | result | to do |
|---|---|---|---|
| decoytown | link to `script_vehicle` 1095 | puppet in the vehicle (distance 43); owner: leave it | polish |
| eldaba | link to `script_vehicle` 741 | puppet in the vehicle (distance 25, offsets 46 / 22); owner: position OK for now | polish |
| matmata | link to `script_vehicle` 52 | puppet linked (variant 3, distance 13, i.e. almost at the player's seat); owner: leave it | polish: it nearly overlaps the player |
| toujane | first minute: no link (the armour ride comes later); on foot the solo test puppet used to swing round with the camera (test shift tied to the view direction) -> now a FIXED world shift (150 units east, facing west) | owner: leave it | the tank-armour ride itself: puppet hangs in the air to the right of the player; polish |
| toujane_ride | player starts in the truck; a vehicle later in the middle/end of the level | later part not tested (owner: skip for now) | test the late ride |
| beltot, demolition, duhoc_assault (cut-scene rigs) | no link in the first minutes / cut-scene movers | not tested | test when playing through the missions |

## Puppet shooting (item 4, solo test verified 2026-11-14; second PC not yet)
- Shot = the held weapon's total ammo (`getammocount`) went down; carried in the event field as `((counter*256)+pitch+90)*8+3`; the puppet fires `magicbullet` from the weapon in the peer's aim direction (in solo mode: the local player's aim, otherwise the bullets would hit the player), plays the shoot pose and a muzzle flash on `tag_flash` (rifle / pistol / thompson fx by weapon class).
- Reload = event type 4 at the moment of the reload KEY press while the magazine is not full. `buttonpressed` takes KEY names (decompiled): "r" is hard-coded, so a rebound reload key is not seen. The magazine (`getweaponslotclipammo`, slots only "primary"/"primaryb") grows only at the END of a reload, so it cannot be used for the start.
- Bolt action: the stock reload clip at 3x speed after each shot stands in for working the bolt (placeholder: find or make a real rechamber animation). Bullets have no tracer; puppet bullets can hurt the local player (friendly fire when aimed at him).
- Shots: a counter (not a flag) carries every round fired between two samples (fast weapons drop several rounds per 50 ms); the puppet fires them all. Auto-reload: when the magazine reaches 0 with reserve ammo the reload event is sent 0.35 s later. Puppet bullets are stopped short of the local player (bullettrace).
- To check on two PCs: muzzle flash position with a real guest, shot direction/pitch, friendly fire.

## TODO (later): real mounted guns instead of the stand-in
- Case: mission `toujane_ride` ("Armored Car Escape"): it starts in a surrounded house with a stationary MG on the wall that the player mans; later an armored car arrives and the player fires its second MG. Other levels have mounted MGs too (newvillers).
- Now (0.9.17): weapon ids 28+ are recognised, a mounted/linked player fires "shots" while the attack button is held, the partner's puppet answers with a bren shot (sound, bullet, flash) from where it stands. This is a stand-in: sound and flash come from the puppet, not from the gun. Not confirmed to work in the armored car / wall MG case (the owner said "does not work yet").
- Right way: make the REAL turret on the other PC fire. Script built-ins exist in this exe (names from `_re/out4/script_builtins.txt`): `getturretowner`, `useturret`, `stopuseturret`, `startfiring`, `stopfiring`, `shootturret`, `fireturret`, `settargetentity`, `setturrettargetvec`, `setturrettargetent`, `setmode`, `isfiringturret`, `isturretactive`, `getturret`. Plan: (1) on the sender find the turret the local player uses (`getturretowner` == player over `getentarray` of the turret classes) and mark "mounted" in the packet; (2) on the receiver find the turret nearest to the partner's position (within ~200 units), aim it at a script_origin placed along the partner's aim, `setmode`/`startfiring` while shot events keep coming, `stopfiring` after 0.2 s without one; same for the vehicle MG (`fireturret`, `setturretnode`).
- Unknown (to find by trying): which mode and target a turret needs to fire by script in this version; whether a turret that has an owner accepts commands. A one-PC test is hard because the player stands behind the same gun: add a debug switch that ignores the owner check in solo mode, otherwise two PCs are needed.

## Partner grenades (0.9.23, owner test 2026-11-14)
- Fixed in solo mirror: packet duplicates (each G packet is sent twice, the second is dropped now), smoke vs frag choice (model name compared in lower case: `xmodel/projectile_us_smoke_grenade`), throw clip standing (`stand_grenade_throw`), grenade spawned 0.45 s after the throw event.
- OPEN, check again on the second PC: the SMOKE grenade of the partner still spins on the spot after landing (frag stopped spinning after the duplicate fix); smoke effect not confirmed visible; damage to enemies unverified (no enemy was within the 260 radius in any test; log lines `COOPG enemy at distance`). The `iprintln("COOPG ...")` debug lines are still in the mod and must go before the final version. Prone throw shows no clip (grenade only); crouch walking has no animation by choice.

## Tank missions - recon (2026-11-14)
- matmata has NO player-driven tank (on foot + passenger rides). Player-driven tank levels: `libya` (`maps\_crusader_player`, `level.playervehicle`, `vehicle_wait(true)` -> `useby(level.player)`) and `88ridge`.
- Built-ins that exist in this exe (script_builtins.txt): `spawnvehicle` 0x4ECF50, `makevehicleusable`, `getvehicleowner`, `precachevehicle`; script side: `self useby(level.player)`. Plan (owner: "own tank for the guest: drives and shoots"): spawn a second vehicle of the player type on the guest PC, seat the guest, mirror position/hull/turret yaw to the host as a model, fire events as for the puppet. Risks: scripts of the level are written for one player vehicle (`level.playervehicle`), enemy AI targets only that one.

## libya tanks - owner notes after the 0.9.24 two-PC test (2026-11-15)
- Works: tanks did not sink during the first minutes, no self-fire. Not heard: the guest's shot sound (now `crusader_fire` played at the partner's model, 0.9.25 test build). Open: the tracks of the partner's tank keep moving while it stands still; the muzzle effect is a stand-in (small explosion), "make another effect later"; seating of the guest was slow (fixed wait of 8 s replaced by waiting for the level's own seating).

## Start barrier (experiment, 2026-11-15, OFF by default: console variable `coop_barrier` = "on")
Goal: no desync at mission start (the two PCs load at different speeds). Done and verified in the solo mirror: R packets ("R <map> <skill>", `coop_ready_set`, `coop_peer_ready`, `coop_link_active`), the wait loop and the release ("COOPB go (partner ready)"), reset of "ready" on every mission change/reload.
NOT working: slowing the game while waiting. Findings: `setcvar("timescale")` -> "Internal Dvars cannot be changed by script"; console `timescale` -> "timescale is cheat protected", `sv_cheats` is "read only"; writing the float directly into the dvar struct (+8) changes what scripts read (0.05) but the game keeps running at full speed (8 script steps of 0.05 s took 0.5 s). Last attempt (untested): also raise `sv_cheats` (+8) for the slow-down. Fallback idea: hold the player and the AI by script (freezecontrols, ignoreme, no slow time) until the partner is ready, or let the barrier only ensure both PCs enter the mission together (hold the loading, not the world).
Temporary test settings in the profile (config.cfg): `coop_barrier on`, `coop_barrier_delay <s>` (pretend to load slower), `coop_tank_guest 1` (the host acts as the guest in the tank test); remove after tests.

## Start barrier - WORKS (2026-11-15, owner solo-mirror test, release 0.9.28)
Final form: `coop_barrier_now()` is called from main() before the mission's script and does not wait; the bridge writes cl_paused = 1 at once (not a frame of the world runs, no sound), shows its own click-through notice window in the style of the game's dialogs ("CO-OP" gold header, white serif text, gold lines), sends "R <map> <skill>" and releases when the partner reports the same mission and difficulty, or after 20 s. While a level loads the engine itself keeps cl_paused at 1 and clears it when the level goes live: the bridge puts it back ("the engine dropped the pause, set again"). Slowing time (timescale) and waiting inside main() were tried and do not work (see the notes above). On by default; switch off with the console variable `coop_barrier "off"`. A partner on a release without the barrier makes the other PC wait the full 20 s per mission (the version warning is shown too). Not yet tried with two real PCs.

## Faster mission start (2026-11-15, not yet released)
Measured on the guest PC (0.9.28): JOIN -> mission script 20.5 s, of which 11.9 s is the host not having started a mission and ~6 s is the guest's own load after the host's announcement. Changes: the host announces the mission at EVERY change (not only each 2 s) and also from the briefing slide show (`slideshow_<mission>` already names the mission), so the guest starts loading it up to ~6 s earlier; the guest acts on a new announcement at the next 200 ms tick instead of the next 1 s check. The notice for different difficulties was removed again (owner). Engine load time itself (about 9 s, single-threaded precache) is not touched. Untested with two PCs.

## TODO (owner, 2026-11-15): standing animation
"Анимацию стояния после этой проблемы сделаем" - the puppet's standing (idle) animation is to be reworked after the current mission-start sync problem is settled (0.9.29 two-PC test). Details of what is wrong are still to be asked for.

## Difficulty is now taken from the host (2026-11-15, not yet released)
Guest-PC analysis (answer-30): the guest never picks a difficulty, `map <level>` keeps whatever g_gameskill it had (1 on a fresh game), so both PCs always played one mission on different difficulties (different enemy accuracy/damage); the barrier then only said "another difficulty". Now the host's "M <map> <skill>" announcement carries g_gameskill and the guest sets it (console `g_gameskill N`, latched, takes effect with the level) right before it follows the mission. Log line on the guest: "follow: difficulty X here, Y on the host: setting it ...". Side effect: the guest's own saved difficulty changes to the host's (g_gameskill is saved in its profile). Untested with two PCs.
Measured (0.9.29, guest): JOIN->mission 18.0 s (20.5 s on 0.9.28): discovery 0.96 s, waiting for the host to start 11.0 s (the player), host signal -> map 2.2 s, precache 3.8 s (engine, single thread); the bridge's share is now negligible.

## Guest PC: why `install` failed there (2026-11-15, answer-31)
Not a bug of the installer: on the guest PC the agent's sandbox denies WRITES to protected paths (C:\Program Files (x86)\...) to 32-bit child processes (windows error 5, errno 13), while 64-bit processes can write there. CoD2Coop.exe is 32-bit. The agent ran the installer against a hardlinked copy of the game folder and copied the result: the output is the installer's own. A normal user on a normal desktop is not affected (the installer works on the developer PC from Explorer-like launches). Added a hint for error 5 (run as administrator, antivirus / controlled folder access).

## Guest PC load time (2026-11-15, answers 33-36)
Guest "Load time" 8.4-9.8 s vs 1.7-4.3 s on the developer PC. Ruled out by measurement on the guest PC: disk (1289 MB/s, no cache effect), Defender (excluded), HVCI/memory integrity (off: no change), GPU preference (RTX 4050 already used), power plan, r_aaSamples 4 vs 1, r_fullscreen 1 vs 0, com_hunkMegs 512 vs 200. The load holds about 2 cores; the guest CPU reports 2.5 GHz (host 3.4 GHz); the Russian data set (localized_russian_iw00..07, loc_language 6) is the one untested difference. So the extra ~5 s is hardware, not tunable.
What the mod does about it: the guest must START loading as early as possible. The host now announces the mission that FOLLOWS a briefing (the briefing scripts' level.levelToLoad: movie_africa->decoytrenches, movie_eastern->moscow, movie_europe->duhoc_assault, slideshow_bergstein->bergstein, slideshow_caen->beltot, slideshow_earlygrad->tankhunt, slideshow_lategrad->downtown_assault, slideshow_libya->libya, slideshow_rhine->rhine, slideshow_tunisia->toujane_ride) at the moment the host's briefing starts, and the guest loads the mission at once, skipping the briefing: its slower load runs in parallel with the host's briefing (3.7-5.3 s) instead of after it. Expected: the barrier wait drops from ~5 s to about 0-1 s. Untested with two PCs.
Also pending: the difficulty sync now writes g_gameskill from main() (the console refuses it).

## TODO (owner, 2026-11-15): check later if needed
The owner usually skips the briefing, so the briefing head-start gains little. If the barrier wait at the mission start (about 4 s on the host) turns out to bother players: measure how much of the guest's load is our own script addition (compare "Load time" with and without zz_coop.iwd on the same mission) and trim it.

## Scope (owner, 2026-11-15)
Only tanks are driven by a player in this project; trucks, planes and boats are not controlled. The partner's ride in them stays as it is (linked to the vehicle).

## Owner's two-PC test (2026-11-15, after 0.9.30/0.9.31)
Death: OK. Tanks: OK except external visual defects (to be done AFTER the release: the partner tank's model/turret/tracks/effects). Mission start: almost simultaneous, no big desync expected at the start; shared checkpoints remove the rest. Nothing else open from this test.

## Puppet without a weapon (2026-11-15, owner test, released in 0.7.4; the test switch coop_un is removed from the script)
Done: the partner's weapon id staying 0 switches the puppet to unarmed clips: standing `standunarmed_idle_loop`, running `unarmed_run_russian`, crouching still `unarmed_crouch_idle1`, crouching and moving `crouchwalk_loop`, lying still `wounded_crawlidle`. The potato (a harmless joke throwable of the Russian missions) and every other grenade are chosen by the model of the thrown grenade; the potato does not explode. Owner: standing/running OK, potato OK, crouch walking accepted "for now".
TO IMPROVE LATER:
- Crouch walking unarmed: no unarmed walking clip exists in the game; `crouchwalk_loop` is the rifle crouch walk, so the arms keep a "holding a rifle" pose. (The `pb_crouch_walk_forward_unarmed` / `pb_crouch_run_forward_unarmed` clips are for the multiplayer player-body skeleton and look like climbing a ladder on this model: do not use.) Idea: an own clip is not possible; maybe blend `standunarmed_walk_plain` upper body with the crouch legs, or accept.
- Lying and moving unarmed still uses the rifle crawl `prone_crawl`; no unarmed prone crawl in the game.
- Armed crouch moving has no step animation at all (the owner chose the static aiming pose earlier; the old crouch-run did not stop when he stopped).
- The unarmed run is the same speed for walking and running; `standunarmed_walk_plain` / `unarmed_walk_slow` exist if a walk is wanted (needs a walk/run speed split in coop_pose_move).
- The animation tree is at the limit: the script mentions 26 distinct animation names (26 worked, 28 failed). Any new clip needs an old one removed (stand_alert_1/3, stand_aim_straight are candidates).
- The test switch `coop_un` was removed from coop_pose_move (2026-11-16).

## 88ridge: the tank of the second player (2026-11-15, owner: "forgot to add it")
88ridge uses the same player tank (`crusader_player`, `level.playervehicle`, the level calls `maps\libya::...` helpers), so the libya tank logic now runs on both missions: the guest gets a second tank (spawnvehicle), the partner's tank is a model that follows, no soldier puppet on these two missions (coop_net2, coop_precache_weapons, coop_tank_main gates). Not run by the author on 88ridge yet; the stock 88ridge has no playerturretsetup(), the copy's MG set-up is the same as on libya (player MG, second MG deleted).

## Version numbering (owner, 2026-11-15)
The first PUBLIC release is v0.7.4 (repository CreativeNa/CoD2SP-coop). It was built from the private source that was numbered 0.9.33. The private line now CONTINUES from 0.7.4: the next private release is 0.7.5. Old private tags/releases v0.9.x stay as they are (v0.9.33 == 0.7.4 in content).

## B13 (2026-11-16, owner screenshot): the version line "1.2 | 0.7.x" was doubled in the CO-OP screen (fixed: the copy in coop.menu is removed, the one in main.menu stays; not yet seen in the game)
Cause (read from the code, not yet confirmed in the game): the line is drawn twice. `tools/patch_menu.ps1` adds it to `ui/main.menu` (item `coop_verline`, `main_text`, rect 590 470) and also fills `__COOPVER__` in `launcher/menu/coop.menu`;
inside the CO-OP screen the main menu stays under it, so both copies are visible and overlap. Fix idea: keep one copy (drop the one in `coop.menu`, or hide `coop_verline` while the CO-OP screen is open).

## Animation limit: the 32 nodes are per TREE, a second tree has its own budget (test 2026-11-20, 127.0.0.1)
- Test 1 (a second script file with the SAME `#using_animtree("coop_puppet")` and 12 more `%` names): the engine counted all names of both scripts against ONE tree (28 + 12 = 40): `Cannot dynamically use an anim tree of size greater than 32` at the first `useAnimTree` of the level script: T-pose from the start. A second script does NOT give a second budget.
- Test 2 (a second script with its OWN tree `#using_animtree("coop_puppet2")` and `animtrees/coop_puppet2.atr`, 12 clips): all 12 clips played, no size error: a second tree has a second budget of 32. Switching an entity between trees: bank 1 -> bank 2 worked (a T-pose flash for one moment); the way BACK to tree 1 failed (`anim 'X' in animtree '(null)' does not belong to the entity's animtree '(null)'`, the puppet kept the second tree's idle): plain `useAnimTree`, and `stopUseAnimTree(); wait 0.1; useAnimTree` both.
- Idea that avoids the way back: TWO puppet entities, each with its own tree for good (locomotion on one, the extra poses on the other), show one and hide the other, move both. Not built. The test code is behind `make_mod.ps1 -Bank2` (dev only, never in the installer or a release; the installer does not carry `coop_puppet2.atr`).

## Environment (2026-11-25): RivaTuner/Afterburner OSD crashes CoD2SP_s.exe (`0xC00000FD` in `RTSSHooks.dll`)

**Symptom:** the game dies after 10-30 s, or immediately when PgUp / Delete is pressed (RTSS hotkeys). WER names
`mssrsx.m3d` with `0xc0000005`, but the proxy's own crash report is the useful one:
`Exception: 0xC00000FD EXCEPTION_STACK_OVERFLOW, At: RTSSHooks.dll+0xAE377`, called from `d3d9.dll` -> `nvd3dum.dll`.

**Not our code:** reproduced with `d3d9.dll` (the proxy) and `zz_coop.iwd` both renamed away - a plain game + RTSS
crashes the same way. Neither the bridge nor the mod is in the stack.

**Workaround (applied here, 2026-11-25):** a per-application RTSS profile that disables its hooking:
copy `tools/rtss-cod2sp.cfg` to `C:\Program Files (x86)\RivaTuner Statistics Server\Profiles\CoD2SP_s.exe.cfg`
(needs administrator; the same as RTSS GUI: Add `CoD2SP_s.exe` -> Application detection level = None). Afterwards
`Get-Process CoD2SP_s | % Modules` shows no `RTSSHooks` at all and the crashes stop.

**Note for support:** "crashes when I press PgUp" is this, not the co-op mod. Ask for `CoD2Coop.exe logs`: the
crash report inside names the faulting module.
