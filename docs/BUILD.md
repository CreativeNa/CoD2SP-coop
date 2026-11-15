# Build from source

The repository contains sources, build scripts and identity data. **No game files, no binaries, no extracted game scripts.**

## Requirements

| What | Why |
|---|---|
| Windows 10/11 x64 | the game is 32-bit and runs under WOW64 |
| Call of Duty 2 single player 1.3 (Steam or retail) | the mod is bound to the exe build (hashes in `docs/identity.json`); a different build needs new addresses |
| Visual Studio 2022 Build Tools, C++ workload, **x86** (`vcvars32.bat`) | builds the bridge, the proxy and the installer |
| Python 3 | `tools/embed_blob.py`, `tools/make_coop_str.py`, the release check in `build_release.ps1` |
| PowerShell 5.1 | all scripts |
| the game started once | creates `main\players\<id>` and the profile config |

`powershell -ExecutionPolicy Bypass -File tools\verify.ps1` checks the machine and prints the expected exe hash.

## Build

```
powershell -ExecutionPolicy Bypass -File tools\build.ps1            # bridge\cod2net5.dll (+ loader)
powershell -ExecutionPolicy Bypass -File tools\build_proxy.ps1 -Test # proxy\d3d9.dll and its test without the game
powershell -ExecutionPolicy Bypass -File tools\build_release.ps1     # dist\release\CoD2Coop.exe + zip
```

`build_release.ps1` builds the bridge, builds the script mod from the **stock** scripts of a game folder that has no `zz_coop*.iwd` (a
quick check of the result is included), builds the proxy, embeds the bridge, the proxy and the PowerShell build scripts into `launcher\blobs.h`,
and compiles `launcher\cod2coop.c` into the one exe. The version is the `VERSION` define of `launcher/cod2coop.c` (and the string of
`bridge/cod2net5.c`).

## Develop in a test copy

Copy the game folder (for example to `Call of Duty 2 - coop-test`) and build the mod straight into it with the game closed:

```
powershell -ExecutionPolicy Bypass -File tools\make_mod.ps1 -GameDir "<game>" -TestDir "<test copy>" -Probe net7 -Levels all
```

(copy `bridge\cod2net5.dll` over the one in the game folder: the proxy reloads the bridge while the game runs). `tools\test_levels.ps1` loads
levels one after another and reports script errors from `main\console.log`.

## How the mod archive is made

`tools/make_mod.ps1` reads the stock level scripts from the game's own `main\iw_*.iwd`, renames each `main()` to `coop_orig_main()` and puts
a wrapper in front of it (the co-op script body), patches `maps/_utility.gsc` (flag hooks) and the menus (`tools/patch_menu.ps1`: the CO-OP
button and the partner health read-out), adds the own animation tree (`tools/animtrees/coop_puppet.atr`) and writes
`main\zz_coop.iwd`. The installer runs this on the player's PC, so no game content is distributed.

## Layout

| Path | What |
|---|---|
| `launcher/` | the installer/launcher (`cod2coop.c`), its README, the CO-OP menu (`menu/coop.menu`) and strings |
| `bridge/` | the in-process bridge `cod2net5.c` (network, script built-ins, window/mouse, checkpoints, barrier), `coopapi.h`, the loader |
| `proxy/` | the `d3d9.dll` proxy (loads the bridge, live reload, crash and hang reports) and its test |
| `tools/` | build scripts, the mod builder, menu patcher, string generator, log collector, engine research helpers (`tools/re`) |
| `docs/` | documentation; `docs/RE` — engine research and the bridge design |
