# The co-op bridge: design and wire protocol

The single-player exe has one client slot, so a second human cannot "join". Instead each PC runs its own copy of the mission and the two
copies exchange the partner's state; the partner is shown as a scripted soldier (the **puppet**) driven from script. Everything is
done with three parts that never modify the game's files:

| Part | Where | Role |
|---|---|---|
| **proxy** `d3d9.dll` | game folder (`proxy/d3d9proxy.c`) | loaded by the game instead of the system `d3d9.dll`; forwards `Direct3DCreate9`; loads a private live copy of the bridge into `CoD2SP_s.exe` only; keeps stubs for the script built-ins so the bridge can be reloaded while the game runs; crash/hang reports |
| **bridge** `cod2net5.dll` | game folder (`bridge/cod2net5.c`) | UDP/TCP link, script built-ins (`coop_*`, see [03_SCRIPT_BUILTINS.md](03_SCRIPT_BUILTINS.md)), mission follow, shared checkpoints, start barrier, window/mouse, the menu status line |
| **mod** `main\zz_coop.iwd` | built on the player's PC (`tools/make_mod.ps1`) | the game's own level scripts with a wrapper in front of `main()` (the co-op script body), the CO-OP menu, the animation tree |

## Roles

The first PC to press **HOST** announces itself (UDP beacons on 27202). The guest (**JOIN**, address `auto`) learns the host's address from them.
The host announces the mission it is in; the guest starts the same one itself (console `map` or, inside a mission, the script's `changelevel`).
A briefing map (`slideshow_*` / `movie_*`) is announced as the mission that follows it (`level.levelToLoad` of the game's briefing scripts), so the guest
starts loading at once.

## UDP 27201 (all numbers as text)

| Packet | Meaning |
|---|---|
| `x y z yaw seq us stance evt weapon parent health` | the player state, about every 40 ms (a heartbeat even when standing). `stance`: 0 stand, 1 crouch, 2 prone, 3 air, 4 in a vehicle. `evt` = `counter*8 + type`: 1 melee, 2 grenade throw, 3 shot (`((shot counter*256)+pitch+90)*8+3`), 4 reload, 5 death. `weapon` is a table index, `parent` the entity number of the vehicle the player is linked to, `health` in percent |
| `K x y z` | an enemy died at this place (the other PC kills the nearest enemy there) |
| `H 1` | hello of a guest that has not started a mission yet |
| `E …` | echo (round-trip time) |
| `M <map> [<skill>]` | the host's mission (and difficulty `g_gameskill` 0..3) |
| `F <flag>` | a mission flag that was set (optional mirror) |
| `G <weapon> ox oy oz vx vy vz` | a thrown grenade: weapon name, start offset from the thrower, velocity (sent twice, the receiver drops the copy) |
| `T ox oy oz ax ay az tx ty tz fire` | the player's tank: position, hull angles, turret angles, shot counter (about 20 per second) |
| `R <map> <skill>` | "this PC is in that mission and ready" (start barrier), repeated until the mission changes |
| `V <version>` | the bridge version; a different one shows "VERSION MISMATCH" |
| `level <name>` | request to change the level |

Dev-only commands (`C anim`, `C fade`, `C loadgame`) are accepted from the local PC only.

## TCP 27203: shared checkpoints

The host's bridge watches `main\players\<id>\save\autosave\*.svg`, and sends every new autosave (`COOPSAVE <name> <size>\n` + bytes); both
PCs do it (symmetric). The receiver accepts a connection **only from the linked partner's address** (or from itself in the solo test), validates
the name (letters, digits, `_`, `-`) and the size, and writes `coopshared.svg` plus the files the engine may reload from. After a death each
PC loads the newest known checkpoint by name (`loadgame autosave/<name>`) about 3.5 s after the engine's own death screen starts; the
surviving player is killed by script so that the engine shows its death screen.

## Start barrier

The script's `coop_barrier_now()` runs in `main()` before the mission's own script and does not wait. The bridge writes `cl_paused = 1`
(an engine variable: the engine itself keeps it at 1 while a level loads and clears it when the level goes live, so the bridge sets it again),
shows its own click-through notice window, sends `R`, and releases when the partner's `R` for the same map arrives (20 s at most).
Time scaling (`timescale`) and waiting inside `main()` do not work (see [04_LIMITS_AND_OPEN.md](04_LIMITS_AND_OPEN.md)).

## The puppet

A `script_model` with the campaign's character model, `makeFakeAI`, an own animation tree and the weapon model attached to `tag_weapon_right`.
It is moved with `moveTo` toward the reported position (height from a ground trace, or linked to the vehicle the partner is in), plays stand/crouch/prone/crawl/
run/jump/unarmed clips, and re-plays the partner's events (shot flash and a safe bullet that never hits the local player, reload, melee, grenade).
Grenades of the partner are simulated by script (gravity, bounces, fuse, explosion effect, damage to enemies only) because `magicbullet` only handles bullets.

## Tanks (`libya`, `88ridge`)

The level's own player tank is seated by the stock script. The guest gets a second tank made with `spawnvehicle` and set up like the level does
(init thread, `setup_vehicle_tank`, `script_turret`/`script_attackai` off so the AI does not fire it, the player MG), seated with
`vehicle_wait(true)` (the binocular view and HUD). Each PC sends its own tank in `T` packets; the partner's tank is a plain
`vehicle_crusader2_static` model that is moved directly (a vehicle entity ignores `.origin` and does not support `linkTo`). The partner's shot is
a blast at the muzzle plus an explosion where the turret line hits, damaging enemies only.

## Difficulty

The guest never chooses a difficulty, and `g_gameskill` is read-only for the console. The bridge writes the host's value into the dvar from `main()`
before the level's own script reads it (`maps\_gameskill`: `level.gameSkill = getcvarint("g_gameskill")`).
