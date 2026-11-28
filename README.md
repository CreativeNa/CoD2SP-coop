# CoD2 Co-op — two players through the single-player campaign

Two-player co-op for the **single-player campaign of Call of Duty 2** (Steam appid 2630, `CoD2SP_s.exe`, build
`pc_1.3_1_1`; the retail 1.3 build works too).

Both players run the campaign on their own PC. The host's game is the authority for the mission and the enemies; everything
the second player needs - the partner's body, the enemies' deaths, health and movement, the shared checkpoint - travels over
a small state channel on the game's own engine.

Nothing here is a second client slot and nothing touches the multiplayer: the co-op lives inside `CoD2SP_s.exe` only.

## What works

- **Mission follow**: the guest loads the mission the host starts, and the next one in campaign order when it is finished.
- **The partner's body**: dressed for the campaign, poses, weapon, jumps, grenades, tanks and the placed machine gun.
- **Enemies**: the host decides, the guest's copies follow - deaths, health, body positions and, in the current development
  build, the animation.
- **Deaths and checkpoints**: when one player dies, both go back to the same checkpoint, and the host's save file is sent to
  the guest over the same channel.
- **Start barrier**: both PCs wait for each other at the start of a mission instead of one running ahead.

## Requirements

- Call of Duty 2 for Windows, single-player (`CoD2SP_s.exe`), Steam or retail 1.3.
- Windows 10/11, PowerShell 5.1 (the installer uses it to build the mod from your own game files).
- Both PCs able to reach each other: Radmin VPN (recommended, it survives NAT) or one local network.
- The game firewall prompt: allow `CoD2SP_s.exe` on **private and public** networks.

The repository contains no game files, no binaries and no extracted game scripts: the installer builds the mod from the
files already on your disk.

## Install and play

1. Download the release zip and unpack it.
2. Run `CoD2Coop.exe` on **each** PC and choose **Install the mod** (the game must be closed).
3. Start Radmin VPN, then start the game through Steam.
4. Host: main menu -> **CO-OP** -> **HOST**, then start a **new** mission.
   Guest: main menu -> **CO-OP** -> leave `auto` in the address field -> **JOIN**.

The full step-by-step, with what to do when something goes wrong:
[`docs/HOW_TO_PLAY.md`](docs/HOW_TO_PLAY.md).

## Documentation

| file | what |
|---|---|
| [`docs/HOW_TO_PLAY.md`](docs/HOW_TO_PLAY.md) | install, host/join, in-game behaviour (EN/RU) |
| [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) | connection refused, firewall, version mismatch, black screen |
| [`docs/ANTIVIRUS.md`](docs/ANTIVIRUS.md) | why an antivirus reacts to a DLL that is injected into a game, and what to do |
| [`docs/SECURITY.md`](docs/SECURITY.md) | what the peer is allowed to send, and what is checked before it is applied |
| [`docs/ENGINE.md`](docs/ENGINE.md) | engine notes: what the script layer can and cannot do (addresses are bound to the exe hash in `docs/identity.json`) |
| [`docs/SAVE_FORMAT.md`](docs/SAVE_FORMAT.md) | what a checkpoint file is, and what is validated before it is written |
| [`docs/BUGS.md`](docs/BUGS.md) | the open-bug index and the history of what was fixed |
| [`docs/NPC_KEYS.md`](docs/NPC_KEYS.md) | how an actor gets a stable name across two PCs |
| [`docs/RELEASE_INTEGRITY.md`](docs/RELEASE_INTEGRITY.md) | how to check that a release zip is the one that was built |
| [`docs/releases/`](docs/releases) | release notes, one file per version |

## Build from source

Both halves are built for x86 (the game is 32-bit):

```
powershell -ExecutionPolicy Bypass -File tools\build.ps1            # bridge + loader
powershell -ExecutionPolicy Bypass -File tools\build_proxy.ps1      # d3d9 proxy
powershell -ExecutionPolicy Bypass -File tools\build_release.ps1    # the release exe + zip
```

`tools\make_mod.ps1` builds the script mod from a copy of the game's own `moscow.gsc` - that is what the installer runs on a
player's PC. `tools\verify.ps1` is a quick check that the tree still produces a working mod.

Visual Studio Build Tools with the C++ workload and Python 3 are needed for the release build.

## Layout

| path | what |
|---|---|
| `bridge/` | the in-process bridge `cod2net5.c`, the thread table, the script built-in interface `coopapi.h`, the loader |
| `proxy/` | the `d3d9.dll` proxy: loads the bridge into the game, live reload, crash and hang reports |
| `launcher/` | `CoD2Coop.exe` (installer/launcher), the CO-OP screen, the text for the second player's assistant |
| `tools/` | build scripts, the mod builder, the menu patcher, the string generator, the log collector, the bridge security test |
| `docs/` | the documentation listed above |
| `probe/` | a small DirectInput probe used while researching input handling |

## License

[MIT](LICENSE).

Call of Duty is a trademark of Activision. This project is not affiliated with or endorsed by Activision or Infinity Ward;
it ships no game content and expects a legally owned copy of the game.
