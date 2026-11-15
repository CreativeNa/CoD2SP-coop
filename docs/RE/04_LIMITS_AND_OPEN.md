# Limits found in the engine and the scripts, and what is open

Everything below was observed in the game (single player 1.3); each line was confirmed in a log or on screen.

## Script language and built-ins

- `vector * int` is a runtime error; compute the components. Variables first assigned in one `if` block are undefined in a sibling block.
- `buttonpressed` takes **key names**; weapon slots are only `"primary"` / `"primaryb"`; `getammocount` is the total ammunition of the current weapon.
- `getarraykeys` does not exist; `magicbullet` handles only weapons of type bullet (not grenades; `MagicGrenade` needs an actor).
- The notify `grenade_fire` never arrives for the single-player player; a thrown grenade is found as an entity of class `grenade` near the player.
- HUD text needs `precacheString` at level load; effects (`loadfx`) and models must be loaded at level load: too many effects overflow the
  configstrings (`G_FindConfigstringIndex: overflow`, seen on `duhoc_assault` with three extra effects).
- `doDamage(health, origin)` with the player as the target kills the player; use `getaiarray("axis")` and damage actors only.
- `setcvar` cannot change internal dvars ("Internal Dvars cannot be changed by script"); `timescale` is locked without cheats, `sv_cheats` is read-only,
  `g_gameskill` is read-only for the console and is reset by the engine at the level start; writing the dvar from the bridge works for `g_gameskill`
  (read by the level script afterwards) and for `cl_paused`, but not for `timescale`.
- `iprintln` writes its text into `console.log` as a "Non-localized Game Message" warning, which is how diagnostics from script are read; `println` is not logged.
- A level script cannot `wait` before `maps\_load::main()`: the engine starts the level's vehicles in the meantime and the turrets fail
  with `bad weaponinfo` (the wait in `main()` was tried for the start barrier).

## Animation trees

The engine error "Cannot dynamically use an anim tree of size greater than 32" counts the **distinct animation names the script mentions**, not the size
of the tree file: 26 names worked, 28 failed with the same tree file. On the error the puppet stays in the T-pose. The own tree is
`tools/animtrees/coop_puppet.atr` (a plain text list of stock xanim names); the script currently mentions 26 names, so a new clip needs an old one removed.
Clips of the multiplayer player-body skeleton (`pb_*`) do not fit the AI models (a crouch walk looks like climbing a ladder).

## Vehicles

A `script_vehicle` cannot be linked (`does not currently support linkTo`), its `.origin` assignment is ignored and an empty one sinks through the
ground; only the engine's own path/driver movement works. `spawnvehicle(model, targetname, type, origin, angles)` works; the new vehicle
needs the same set-up the level gives its own (init thread of the vehicle type, `setup_vehicle_tank`, a player MG).

## Saves

`loadgame autosave/<name>` loads by name; after a death the engine alone restarts the level from its start. The save is the `.svg` file in
`main\players\<id>\save\autosave` (about 2 MB). `sv_lastSaveGame` holds the path the engine reloads.

## Open

- A turning turret and moving tracks for the partner's tank (the vehicle entity cannot be moved from script; a model has no turret bone).
- Real mounted-gun firing of the partner; a walking animation for an unarmed crouch.
- Slowing the game down while a PC waits is not possible; the start barrier pauses it.
- Shared AI between the two PCs (enemies are simulated on each PC; kills are mirrored).
