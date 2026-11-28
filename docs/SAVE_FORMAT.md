# Savegame (.svg) structure, as far as it was read (2026-11-16, research; nothing here is game content)

Question asked: can the checkpoint carry the world state so that the guest takes the host's mission progress (objectives, scenes) without reloading?
Method: a copy of `moscow-5.svg` (2.43 MB, an autosave) was opened read-only; nothing in the game folder was changed.

## What is known (read from the file)
- Header (first ~1.2 KB): a version word (0x99), a checksum, sizes, the map name (`moscow`), the build string, the game language, the levelshot name, `AUTOSAVE_NOGAME`, the save's own path.
- Then a zlib stream (starts at byte 1240; 235 KB packed -> 756 KB): entity records (classname, targetname such as `auto8417`, origin, model), a string pool (the script's strings: flag names such as `friendly_wave_spawn_enabled`, objective texts, hint names), and the script variable data.
- After it a different kind of block (not zlib): length-prefixed pieces that contain the text of script functions of the level (so the engine saves the running threads together with the code they run).
- The same entity records give a stable way to name a trigger on both PCs (the targetname/auto-number comes from the map file).

## What is NOT known / the answer
- The values (flag true/false, where each script thread stands, objective states) are serialized VM structures. Reading them needs the layout of the engine's script-variable heap (Ghidra work on the save/load functions); applying them to a RUNNING script is impossible from GSC (a thread cannot be moved to another line).
- The only flag seen in the whole moscow session was `friendly_wave_spawn_enabled`: that chain's progress is not kept in flags, so even a perfect flag reader would not move the objective marker.
- Conclusion: "read the progress out of the save and apply it live" is not practical. What does work: loading the host's save on the guest (the guest takes the whole world, as after a death), or mirroring the triggers the host fires (the observer `coop_trigger_watch`, switched on by `coop_debug 1`, logs them as `COOPTRG` lines for that purpose).
