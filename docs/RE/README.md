# Engine research and bridge design

| Document | About |
|---|---|
| [01_ENGINE_AND_CODE.md](01_ENGINE_AND_CODE.md) | the engine of `CoD2SP_s.exe`: client slot, entities, script and dvar entry points, animation-tree limit (addresses are for the recorded exe build) |
| [02_COOP_BRIDGE.md](02_COOP_BRIDGE.md) | how the co-op works: proxy, bridge, mod, wire protocol, checkpoints, barrier, puppet, tanks |
| [03_SCRIPT_BUILTINS.md](03_SCRIPT_BUILTINS.md) | the `coop_*` functions the bridge adds to the script language |
| [04_LIMITS_AND_OPEN.md](04_LIMITS_AND_OPEN.md) | limits found in the engine and the scripts, open problems |

Nothing here contains game files or code extracted from the game; only addresses, names and behaviour that were observed. The memory dump and the
decompiler output used for the research are not part of the repository (`tools/re/ghidra` has the headless-decompiler helper scripts).
