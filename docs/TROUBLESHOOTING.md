# Troubleshooting / Типичные ошибки

Collect logs first (`CoD2Coop.exe logs`); the main log of the mod is `coop5.log` in the game folder, the engine's log is `main\console.log`.
Сначала соберите логи (`CoD2Coop.exe logs`); основной лог мода — `coop5.log` в папке игры, лог движка — `main\console.log`.

| Symptom / Симптом | Cause and fix / Причина и что делать |
|---|---|
| Installer: "NOT an expected build" / "не та сборка" | Only the Steam 1.3 (`AB17A89C…2281`) and retail 1.3 (`64E7ADCA…5A48`) single-player exes are accepted. Check the game version. |
| Installer: "the game is running" | Close the game (it keeps `main\zz_coop.iwd` open) and run again. |
| Installer: "could not build/write … windows error 5" | Access denied to the game folder. Run `CoD2Coop.exe` as administrator; check that an antivirus / "controlled folder access" is not blocking it. |
| Installer: "the mod could not be built" + build log | PowerShell must be available and not blocked by policy; send the printed log lines. |
| Installer: "a different d3d9.dll is in the game folder" | ReShade or similar: it is not touched, the in-game CO-OP menu cannot connect. Move that file away or use the `host`/`join` commands of `CoD2Coop.exe`. |
| No CO-OP button in the main menu | The mod archive is not installed (`CoD2Coop.exe check`), or the game was updated. Run Install again with the game closed. |
| Status stays "looking for the host" | The host is not in HOST mode, Radmin VPN is off, or the firewall blocks UDP 27201/27202. Type the host's address instead of `auto`. |
| Status "no answer from the host" | Firewall on the host (private **and** public network), or the host closed the game. |
| "VERSION MISMATCH - update both PCs" | The two PCs have different mod versions. Install the same release on both. With an older partner the barrier waits 20 s per mission. |
| Black screen with a gold "Waiting for the other player" panel at a mission start | Normal: the other PC is still loading (up to 20 s, then the mission starts anyway). |
| The guest is in another mission | The guest follows the host automatically; if "follow" was switched off in the CO-OP screen, switch it on or load the mission by hand. |
| After a death one PC loads an older checkpoint | Shared checkpoints are copied over TCP 27203; check the firewall for that port. See `coop5.log`: "checkpoint received/sent". |
| The mission loads slowly (9 s and more) | The engine's own load is CPU-bound (two cores). Add the game folder to the antivirus exclusions; a slow laptop CPU cannot be tuned further. |
| The game crashed or froze | Look for `coop_crash_*.txt` / `coop_hang_*.txt` in the game folder and send them with the logs. |
| Script error window "script compile error" | Send `main\console.log`; reinstall the same release on both PCs. |
