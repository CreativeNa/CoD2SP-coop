> **Status of this document.** It is the engine research that the co-op was built on. Its early sections (items 1-16 and the "Verdict") ask whether the
> exe can host a second client slot; the answer was "not as a switch", which is why the final design does **not** add a second client:
> the partner is a scripted puppet driven by a bridge DLL (see [02_COOP_BRIDGE.md](02_COOP_BRIDGE.md)). The later sections (script/dvar entry
> points, the full decompilation notes, the animation-tree limit) are what the bridge and the mod use.

# ENGINE.md - can the CoD2 SP exe host a second client slot?

Bound to `CoD2SP_s.exe` SHA-256 `AB17A89CC3722AE80EDC1F4483CACBCAE08B0373B9128FE4EC053EBC79062281`
(verified: `sha256sum` of the install exe gives exactly this). Module base `0x00400000`, SizeOfImage `0x18CA000`,
TimeDateStamp `0x442CA3F7`, sections `.text` (VA 0x1000, one huge section holding code and data) and `.rsrc`.
Addresses below are virtual addresses in the running image. Ghidra's default names (`FUN_...`) are used; the
engine names come from the strings next to each function.

Method: `tools\cod2mem.exe <pid> --dump <file>` (read-only, ReadProcessMemory; 0 unreadable pages) dumped the image
of the running process; imported into Ghidra 12.2 headless as raw x86:LE:32 at 0x400000 (`tools/ghidra/DecompAt.java`
decompiles the function containing an address; Ghidra's auto-analysis found almost no xrefs on the raw dump, so
code references to strings were located by scanning the dump for the 4-byte string address). Needed JDK 25
(portable copy in `C:\ghidra\jdk`) and `-Dapplication.settingsdir=C:/ghidra/settings`.
The memory dump and the decompiler output are derived from the game and are not in this repository.

Note: error strings are prefixed by a `\x15` byte, so code references point to string-address minus 1.

## Findings

| # | Claim | Status |
|---|-------|--------|
| 1 | `SV_Startup` is `FUN_00448eb0` (0x448eb0). Its `svs.initialized` error string is at 0x579b14, referenced at 0x448ebb. The `unable to allocate svs.clients` string (0x579ae8) has no direct 4-byte reference anywhere in the image (searched), so it is reached some other way. | verified (function and string); string use for the alloc check unknown |
| 2 | It allocates the client array with a constant: `malloc(0xAA08)` (0xAA08 = 43,528 bytes), zero-fills it, stores the pointer in global `0x825508`, writes `0x800` to `0x82550C`, sets `0x825500 = 1` (initialized flag). No loop, no multiplier. | verified (decompiled) |
| 3 | So the client array is exactly **one** `client_t` of stride 0xAA08. The count does not come from a cvar: the string `maxclients` does not occur anywhere in the image (searched; `sv_running` exists at 0x575c20). Count comes from the immediate in the code. | verified for the constant and the missing cvar string; "the 1 is also hard-wired elsewhere" unknown |
| 4 | Stride 0xAA08 is used as a divisor elsewhere (`(client - 0x825508) / 0xAA08` in the snapshot builder) to turn a client pointer into an index into a parallel array at `PTR_DAT_00822430` (stride `DAT_00822434`). Capacity of that parallel array is not known. | verified (use); capacity unknown |
| 5 | `SV_BuildClientSnapshot` is `FUN_0044b0c0` (0x44b0c0); its error string at 0x579e21 is referenced at 0x44b121. It copies 0x2170 dwords from the per-client slot into the client at +0x2444, checks a value at +0x24EC is in 0..0x7FF, then calls `FUN_0044b050`, which collects indices of other active entries from the array at `PTR_DAT_00822424` (count `0x82242C`, stride `0x822428`) skipping the current one. | verified (decompiled); meaning of `0x82242C` unknown |
| 6 | Its caller `FUN_0044b180` (callers 0x448aff, 0x44a951) does: build snapshot, `FUN_0044af00`, increment a counter at client+0xAA04, then `FUN_0040cf00`. | verified |
| 7 | `FUN_0040cf00` (0x40cf00) zeroes a message struct (read count fields) and calls **`CL_ParseServerMessage` = `FUN_0040f620`** directly. String at 0x572685 is referenced at 0x40f6e2. Same function is also called from 0x402d1f, 0x408467, 0x4084b7 (message from a local buffer reader `FUN_00408360`, apparently demo/save playback). | verified (call graph); playback role of the other callers likely, not checked |
| 8 | So a snapshot reaches the local client by a plain in-process call chain: server build -> `0x40cf00` -> `0x40f620`. The "message" is a `msg_t`-like struct (data ptr at +4, cursize +0xC, readcount +0x10) read in place. No network layer between them. | verified |
| 9 | `CL_ParseServerMessage` handles only two command ids besides end-of-message (id 4): id 2 -> `FUN_0040f0f0` and id 3 -> `FUN_0040f3f0`; anything else is an error `Illegible server message` (0x572684). Name table `svc_*` at 0x59a708 (`svc_EOF`, `svc_snapshot`, `svc_configstring`, `svc_gamestate` strings at 0x572764..). | verified (decompiled); which id is snapshot vs gamestate not mapped |
| 10 | No `NET_*`, `netchan`, `clc_*`, `loopback` strings in the image; `localhost` exists (0x572644, 0x617C84, role unchecked). There is no client->server `clc_` parser string, so usercmd flow from a second client has no visible entry point. | verified (searched strings); absence of the code itself unknown |
| 11 | `SV_SpawnServer`-like init is `FUN_00449340` (0x449340, prints `Server Initialization`/`Server: %s`); it calls `SV_Startup` only if a cvar flag is zero. | verified |
| 12 | A function reusable to attach a second client (a `SV_DirectConnect` / `ClientConnect` equivalent): none identified. The only client-slot code found is the single-slot init above. Not searched: gamecode (`ClientConnect`/`ClientBegin`) in the same exe. | unknown / not found |

## Verdict

**Not feasible as a switch, unclear as a patch project (leaning no).** The server is built for exactly one client:
`SV_Startup` allocates a single hard-coded `0xAA08`-byte slot (no `sv_maxclients`, the cvar string does not exist),
snapshots are handed to the local client by a direct function call, and there is no connect/usercmd/netchan code
visible, so a second slot would need real new code, not a setting. To add one we would have to (a) change the allocation
and everything indexed by `(client-0x825508)/0xAA08` and the parallel arrays at `0x822424/0x822430` (capacity unknown),
(b) write a connect path (spawn a player entity, usercmd input, per-client snapshot send) that the SP build dropped, and
(c) make the second client's snapshot go somewhere other than the in-process `0x40cf00` call, i.e. our own transport
plus a second client parse. That is reverse-engineering and code injection of engine internals measured in weeks,
with high crash risk, versus the current GSC-driven puppet. It would give enemies awareness of player 2 only if the
game's AI iterates the same entity list, which is also unverified. Next experiment, if it is still wanted: find where entity/player-slot count (`0x82242C`) is written and whether the AI's enemy search iterates it.

## Addendum: entity model and enemy awareness (second pass, same dump)

| # | Claim | Status |
|---|-------|--------|
| 13 | `FUN_004d7a30` (0x4d7a30) is the entity allocator (G_Spawn-like): hard cap 0x7FE entities (`0x7fe` check, error string 0x582948), gentity stride `0x1FC` at base `PTR_DAT_01c60504`, and it publishes `0x822424` (gentities base), `0x822428` = 0x1FC (stride), `0x82242C` (entity count), `0x822430` (gclients base, = `01c60500`), `0x822434` = 0x86B8 (gclient stride). So `0x82242C` is the entity count, not a client count, and the snapshot loop `FUN_0044b050` walks all linked entities. | verified (decompiled) |
| 14 | The game side is also single-client: the save/load pointer converter `FUN_004e0c60` case 2 converts a gclient pointer to an index `(p - 0x1c60500) / 0x86B8 + 1` and raises an error if the result is `> 1`. `FUN_004da470` writes straight to `g_clients[0]` (`0x1c60500 + 0x85c0`). | verified (decompiled); that no other limit exists unknown |
| 15 | `makeFakeAI` (script method table entry at 0x56f438, handler `FUN_004f82a0` @ 0x4f82a0) only checks the entity is a `script_model` (type 5, flag 0x2000), clears two bits in the flags byte at +0xA2, writes a bounding box (+0xA8..: mins about (-15,-15,0), maxs about (15,15,72) as floats) and sets flag 0x8000 at +0xC0, then relinks (`FUN_0044b2d0`). It does **not** create a sentient or choose a team. | verified (decompiled) |
| 16 | The sentient/actor AI exists in the exe (`No sentient for player.`, `Sentient allocation failed`, `Actor allocation failed`, `too many actors in bsp file`, `dospawn can only be called on actor spawners`, `unknown team '%s', should be axis, allies, or neutral`, script methods `issentient`, `isai`, `setgoalpos`, `setgoalentity`, `teleport`, `animscripted`, `favoriteenemy`, `ignoreme`, `threatbias`). | verified (strings); behavior not tested |

### Consequence for enemy awareness
Enemies pick targets among sentients. The player has one (`No sentient for player.`); a `script_model` with `makeFakeAI` gets a hitbox but no sentient, so enemies should not target it on their own (inferred from item 15, not tested in game).
Cheapest way to make enemies aware of player 2 is therefore **a real friendly actor** (spawned from a level's `actor_ally_*` spawner, `dospawn`) used as the puppet body and moved with `setgoalpos`/`teleport`, instead of a `script_model` (status: hypothesis; `teleport` has visibility restrictions per strings 0x591584/0x5915b8, so continuous remote positioning may need goal-based movement). Next experiment: in the test install, spawn one `actor_ally_rus_reg_ppsh` from a spawner, log `isai()`/`issentient()` and watch whether axis actors shoot it.

## Script and dvar entry points

All addresses are virtual addresses in the same SP image (SHA-256 `AB17A89C...2281`, base 0x400000). Everything was derived from our own dump with strings, xrefs and the decompiler/disassembly (`tools/ghidra/DumpFunc.java` writes disassembly with raw bytes plus decompilation; `DecompAt.java`/`DecompRefs.java` as before). Nothing was taken from third-party symbol tables. Calling conventions come from the disassembly: "usercall" means a register carries an argument. Status: **V** = verified from disassembly/decompilation, **I** = inferred from callers/behavior, **U** = unknown.

### 1. Built-in lookup (name -> handler)

| What | Address | Convention (from disassembly) | Status |
|---|---|---|---|
| Function lookup (`Scr_GetFunction`-like) | `0x4F8B60` | cdecl `(const char** pName, int* pDevFlag)`. `*pName` is the name on entry; on a hit the routine overwrites `*pName` with the table's name pointer and stores the entry's dev flag in `*pDevFlag`, returns the handler in EAX; returns 0 if absent. Linear scan, byte-wise compare, **no case folding**. Plain `RET` (caller cleans). | V |
| Method lookup (`Scr_GetMethod`-like, dispatcher) | `0x4F8CB0` | **usercall**: ESI = `const char**` (same in/out meaning), EDI = `int*` dev-flag out; no stack args; returns handler in EAX (0 if absent). It zeroes `*EDI` first, then tries per-class lookups in this order, each as cdecl `(ESI)` returning the handler: `0x540EF0`, `0x53C450`, `0x53AB80`, `0x534120`, `0x512FF0`, `0x530C70`, and last `0x4F8C20` (the entity table); the first non-zero result wins. | V |
| Entity method lookup | `0x4F8C20` | same shape as `0x4F8B60`: cdecl `(const char**, int*)`, table at `0x56EF70`. | V |
| Function call site (compiler) | `0x457A3F` inside `FUN_004579A0` | `E8 1C 11 0A 00` = `CALL 0x4F8B60`. Preceded by `PUSH ECX (&devFlag); PUSH EDX (&namePtr)`; result moved to EDI. 16 bytes at the site: `E8 1C 11 0A 00 8B F8 8D 43 01 8B CD 8B D6 E8 4E`. Only caller of `0x4F8B60`. | V |
| Method call site (compiler) | `0x457C5B` inside `FUN_00457BC0` | `E8 50 10 0A 00` = `CALL 0x4F8CB0`; just before it `LEA EDI,[ESP+0x14]` (devflag out) and `LEA ESI,[ESP+0x1C]` (name pointer cell). Result moved to EDI. 16 bytes: `E8 50 10 0A 00 8B 15 04 D7 82 00 8B F8 8D 43 01`. Only caller of `0x4F8CB0`. | V |
| Entry bytes for a guard | `0x4F8B60`: `8B 44 24 04 8B 10 53 55 56 57 33 ED 33 FF 8B FF`; `0x4F8CB0`: `56 C7 07 00 00 00 00 E8 34 82 04 00 83 C4 04 85` | V |

Table entry layout (function and method tables): **12 bytes** `{ const char* name; void* handler; int devFlag }` (devFlag = 1 for developer-only such as `print`, `println`, `assert`, `getentbynum`). There is **no terminator entry**; each lookup bakes the table size in as an immediate: function table `0x5A5C08`, 163 entries (`CMP EDI,0x7A4` at `0x4F8BA9`); method tables: entity `0x56EF70` (111 entries, `CMP EDI,0x534` at `0x4F8C69`), vehicle/turret `0x56FA58` (26), hud `0x570138` (14), script brush `0x570270` (14), player `0x570908` (54), sentient/actor `0x570BA0` (76) (entry counts from scanning for valid entries; class names are guesses from the method names, I); the table behind `0x53C450` was not located (U). Entries cannot be appended in place (size is an immediate, the tables are in the image), so a built-in of our own has to be added by wrapping the lookups; patching the two call sites above (one `E8` rel32 each) is the minimum.

The lookups run **at script compile time**, not on every call; the compile step caches the handler in a runtime table (section 4). Caching is V; that a map change always recompiles scripts is I.

### 2. Script stack helpers

Model (V): `0xA01890` = pointer to the top stack slot, `0xA01884` = stack limit (overflow -> `Com_Error(1, ...)`, string `0x57B838`), `0xA0189C` = **number of parameters** of the current call, `0xA01898` = number of return values pushed. A slot is 8 bytes `{ value @+0, type @+4 }`; parameter *N* (0-based) is at `*(0xA01890) - 8*N`. Types (names table at `0x5A0A50`): 0 undefined, 1 object (entity/array/struct reference), 2 string, 3 localized string, 4 vector, 5 float, 6 int, 7 codepos, 8 precodepos, 9 function, 10 stack, 11 animation.

| Helper | Address | Convention | Status |
|---|---|---|---|
| number of params | global `0xA0189C` (read directly, no accessor) | n/a | V |
| type of param N | inline: `*(int*)(*(0xA01890) - 8*N + 4)`, valid when `N < [0xA0189C]` | n/a | V |
| GetInt(N) | `0x46E4A0` | usercall: EAX = N; returns int in EAX; Scr_Error if `N >= count` (`parameter %d does not exist`, string `0x57B804`) or type != 6 | V |
| GetFloat(N) | `0x46E6D0` | usercall: EAX = N; returns the value in **ST0** (x87); accepts type 5 and type 6 (converted) | V |
| GetString(N) | `0x46E760` | usercall: EAX = N; returns a **string handle** in EAX; characters are at `*(char**)0x8AED00 + handle*8 + 4` (as every handler does) | V |
| GetVector(N) | `0x46EA90` | usercall: EAX = N, ECX = `float[3]*` destination (copies 12 bytes); type must be 4 | V |
| GetObjectType(N) | `0x46EDD0` | usercall: EAX = N; requires type 1, returns class id `(*(0x8BEE88 + 16*value)) & 0x1F` (`isalive` compares with `0x15`) | V (use); class numbering U |
| GetEntity(N) | no single accessor found; entity struct stride `0x1FC` at `0x1B15F00` | | U |
| AddInt(v) / AddBool(b) | `0x46EEC0` | cdecl `(int)`; pushes type 6; bool = AddInt(0/1) (`isalive`, `isdefined`) | V |
| AddFloat(f) | `0x46EF10` | cdecl `(float)`; pushes type 5 | V |
| AddString(s) | `0x46F0C0` | usercall: **ESI = `const char*`** (copied into the script string table via `0x462CF0`); type 2 | V |
| AddLocalizedString | `0x46F130` | usercall ESI = `const char*`; type 3 | V |
| AddVector(v) | `0x46F200` | usercall: **ESI = `const float[3]*`**; allocates a 16-byte node (`0x45EC70`), type 4 | V |
| AddUndefined() | `0x46EFB0` | no args; type 0 | V |
| AddEntity / AddObject(id) | `0x46F000` | usercall: **EAX = object id**; type 1 and a refcount bump (`word` at `0x8BEE84 + 16*id`); an entity's id is `FUN_00467450(*(uint16*)(ent+8))` as done in `getentbynum` and `spawn` | V |
| pop/free inputs | `0x468F60` | every Add* calls it first; it **pops and releases all remaining input params** in a loop. A handler must read all its params **before the first Add** | V |
| Scr_Error | `0x468ED0` | no arguments; the message pointer is taken from global `0x9BEF18` (callers set it with `if (0x9BEF18 == 0) 0x9BEF18 = msg`, message built by `0x43F990` = sprintf into a rotating buffer, cdecl `(fmt, ...)` returning char*). In script context it `longjmp`s via the CRT routine `0x554AB4` with jmp_buf `0xA01020 + [0xA01860]*0x40`; otherwise it calls `Com_Error(1, ...)` | V |

### 3. Dvar functions and console print

Dvar struct (V from `0x431260`, `0x42FE80`, `0x431D00`): `+0` `const char* name`, `+4` `uint16 flags`, `+6` `uint8 type`, `+8` value (for type 7 it is the `char*` itself), `+0x20` next in hash chain. Types seen in `0x42FE80`: 0 bool, 4 vec4, 5 int-like, 6 enum, **7 string**, 8 color (1..3 float forms). Flag `0x4000` marks script-created dvars (`setcvar` refuses others: "Invalid Dvar set - Internal Dvar", string `0x587AE0`).

| Function | Address | Convention | Status |
|---|---|---|---|
| Dvar find by name | `0x431260` | cdecl `(const char* name)` -> `dvar*` or 0 (**called from the bridge in the game 2026-11-13 with an SEH guard and a name check: returns the live dvar struct; used for the menu dvars**). Takes the critical section `0x825B90` (function pointers at `0x56A1B8` enter / `0x56A1B4` leave), hashes the name (`0x42F930`, EAX = name), walks bucket table `0x7805D0`, compares with `0x43F6D0` | V |
| Dvar value to string | `0x42FE80` | **ECX = dvar**, stack = the raw value word (`[dvar+8]`); returns `char*` in a rotating buffer (what `getcvar` uses) | V |
| Set dvar string | `0x432C10` | usercall: **ESI = dvar\***, stack `(const char* value, int source)`, caller cleans (`setcvar` passes source 2); takes the lock, truncates to 0x3FF chars, converts per type, calls `0x430F50` | V |
| Register dvar (new) | `0x431D00` | cdecl 6 args as `setcvar` calls it: `(name, 7 /*string*/, 0x4000 /*flags*/, value, 0, 0)`; max 0x500 dvars (count at `0x780584`, error string `0x576358`), slot array `0x7809D0 + 36*i` | V (call shape), I (last two args) |
| Re-register existing dvar | `0x431BF0` | cdecl 7 args `(dvar*, name, type, flags, value, 0, 0)` | V (call shape), I (semantics) |
| Com_Printf | `0x42C130` | cdecl varargs `(const char* fmt, ...)`: `_vsnprintf` into a 0x1000 stack buffer (`0x553CFA`), then `0x42C000(0, buf)`; `print`/`println` call it with `"%s"` (`0x56CA4C`) | V |
| Com_Error | `0x42C7F0` | cdecl varargs `(int code, const char* fmt, ...)`; code 1 = drop error | V |

### 4. How a built-in handler is invoked by the VM

The VM main loop is `FUN_004690C0` (0x4690C0..0x46B5B8). Compiled scripts cache handler pointers in a runtime table `0x82D728` (count at `0x82D700`; overflow error `SCR_FUNC_TABLE_SIZE exceeded`, string `0x57ACFC`, raised at `0x456ED6`). Built-in call opcodes do:

* **function**: set `[0xA0189C]` = param count and `[0xA01890]` = top, then `CALL dword ptr [EDX*4 + 0x82D728]` (`0x46A04F`): cdecl `void handler(void)`.
* **method**: `PUSH EAX` with EAX = the 32-bit object ref (`uint16 entnum @[EBP+0x28]`, `uint16 class @[EBP+0x2A]`), then `CALL dword ptr [EDX*4 + 0x82D728]` (`0x46A10F`), then `ADD ESP,4`: cdecl `void handler(uint selfRef)`; `(selfRef >> 16) == 0` means a plain entity at `0x1B15F00 + (selfRef & 0xFFFF)*0x1FC` (the pattern every method handler starts with, e.g. `makeFakeAI`, `getorigin`; other classes raise `not an entity`).
* Afterwards the VM pops leftover params and takes `[0xA01898]` pushed values as results.
* devFlag-1 built-ins are rejected outside `/# ... #/` blocks (`return value of developer command can not be accessed...`, string `0x57ACA8`, checks at `0x457ACE` / `0x457CF2`).

A C handler for the bridge therefore has the shape `void __cdecl fn(void)` (function) or `void __cdecl fn(unsigned selfRef)` (method). Helpers that take EAX/ESI/ECX need small naked/asm shims in MSVC. Read all params first, then Add results.

### 5. Runtime hooking notes

* The image has one section `.text` with characteristics `0x60000060` (code|data|execute|read, **not writable** in the header), VSize 0x18C4000, yet the VM writes tables and globals inside that same range, so the loader/CEG changed the protections; code patching needs `VirtualProtect` on the call sites. Whether Steam/CEG re-verifies code pages is **U** (no checksum routine was looked for).
* Guard every patch with the expected original bytes from section 1. Both call sites are the only callers of their lookup, so replacing the rel32 of the `E8` is enough and leaves the original lookups untouched.
* The method lookup is usercall (ESI/EDI): the wrapper must be a naked trampoline that preserves registers and falls through to `0x4F8CB0`; the function lookup is plain cdecl.
* Lookups happen during script compile, so the patch must be in place **before** the scripts that use the new built-ins are compiled; scripts compiled before injection keep their cached handlers (I). Code that runs before our DLL loads (CEG decryption, CRT init) is outside this analysis (U).
* Not verified: that the script system is single-threaded (the dvar functions take a critical section, the script stack globals do not: I).

### Still unknown
* The table behind method lookup `0x53C450` and its size.
* Class-id numbering and a generic GetEntity accessor.
* The last two args of `0x431D00` and the `source` argument of `0x432C10`.
* Whether anything re-verifies code pages at runtime.
* All of this is static analysis; nothing was executed in the game.

## Full decompilation of the SP exe (2026-11-14)

Whole image decompiled once with Ghidra headless (`tools/ghidra/SplitBlock.java` + `DecompAll.java`; `TryAt.java` is a debugging helper). Result: 6301 functions, 6144 decompiled, 93 % of the code bytes covered; every address in this document is a function in the result. The output (`_re/`, git-excluded, local only) is decompiled game code and is NOT stored in any repository; `_re/README.txt` says how to query and rebuild it. Caveat: the dump was taken from a game running with our proxy, so the call sites 0x457A3F / 0x457C5B hold our patched rel32.

### Findings from reading the full decompilation (2026-11-14) - addresses are in the same SP image, status V = read in the decompiled code

1. **All script built-ins of the SP exe** (names and handler addresses extracted from the tables; kept locally in `_re/out4/script_builtins.txt`): 163 functions, entity 111, vehicle 26, hud 14, brush 14, player 54, sentient (AI actor) 76 methods. Useful ones we had not used: player `isthrowinggrenade`, `ismeleeing`, `isfiring`, `isonground`, `getnormalizedmovement`, `playerads`, `buttonpressed`, `setclientdvar`, `setorigin`, `setvelocity`, `getweaponslotammo`; entity `stopuseanimtree`, `animscripted`, `stopanimscripted`, `dumpanims`, `getstance`, `getammocount`, `magicgrenade`, `magicgrenademanual`, `setcontents`; functions `newhudelem` (on-screen text without the menu system), `magicbullet`, `getanimlength`, `animhasnotetrack`, `getmovedelta`, `physicstrace`, `radiusdamage`, `earthquake`. (V, from the tables)
2. **A real actor can do what the script_model puppet cannot**: sentient methods `enterprone` / `exitprone` / `setproneanimnodes`, `melee`, `throwgrenade` / `checkgrenadethrow` / `pickupgrenade`, `shoot`, `teleport`, `setgoalpos` / `setgoalentity`, `animcustom`, `startscriptedanim`, `setaimanims`, `aimatpos`. This is the way to real prone, melee and throw animations (ENGINE.md: "A real friendly actor"). (V, names only; behaviour not tested)
3. **Anim trees** (`useanimtree` handler 0x4F7600, `stopuseanimtree` 0x4F76E0, worker for all `setanim*` 0x4F6C60, per-entity tree instance pointer at entity+500, instance allocated `nodeCount*5+9` bytes, helper 0x4F7540). Error strings: "cannot change the animtree of class '%s'" (the entity's tree differs from the tree of the script that calls: use `stopuseanimtree()` first), "Cannot dynamically use an anim tree", "blended nonsynchronized animation" (blending two anims of different sync groups errors). The `setanim*` worker prints "<method>: tree <t> anim <a> root <r> weight ..." for the entity selected by `dumpanims()` - an on-screen/console debug of what is really played. Nothing in the worker checks that the xanim data is loaded, so the generic_human T-pose is NOT a missing-check error; next test: `stopuseanimtree()` then `useanimtree`, and `dumpanims()` on the puppet. (V)
4. **Mouse path (for raw input)**: init `0x44F040` (cvar `in_mouse`, "Mouse control not active."), activate `0x44ED00` (centre = middle of the WINDOW rect, SetCursorPos to it, ShowCursor loop), per-frame poll `0x44F0A0` (GetCursorPos, delta against the centre, SetCursorPos back to the centre, then calls `0x4096D0` when it moved), deactivate `0x44EE90`, focus gate `0x44EE20`. The Win32 API calls go through function pointers at 0x56A270 (GetCursorPos), 0x56A274 (SetCursorPos), 0x56A278 (GetForegroundWindow), 0x56A26C (PostMessage), 0x56A288 / 0x56A28C (window rect), window handle at 0x82D09C, flags 0x825BD0 / 0x825BD1, centre at 0x825BC4 / 0x825BBC. A raw-input patch would replace the poll at 0x44F0A0 or wrap the pointers. The centre-of-window-rect logic is also why a stretched or resized window offsets the mouse a little. (V; the delta hand-over to 0x4096D0 is by registers, not fully mapped)
5. **Console/dvar/client**: `0x4096D0` is the mouse hand-over (name unconfirmed), `0x40E130` is client init (all cl_* dvars), `0x451DC0` is the window procedure (vid_xpos, vid_ypos, r_fullscreen, quit). (V)

6. **Anim tree size limit (verified by the console log, 2026-11-14):** `useanimtree` (0x4F7600) refuses a tree whose size field (tree object +4) is above 0x20 with the script error "Cannot dynamically use an anim tree of size greater than 32"; after that `setflaggedanimknob*` on the entity gives "entity ... does not have an animation tree". `generic_human` (2624 names; the tree with prone, crawl, throw, melee and crouch-run) is far over it, `fakeshooters` (42 names, 15 top-level) and `drones` (48 names, 20 top-level) are under it. Big trees can only reach an entity statically, i.e. through the actor/AI spawn path. Usable trees on a script_model: fakeshooters (stand/crouch aim and shoot, reload, run, jump_across_100, jump_down_56) and drones (crouchrun_loop_forward_1..3, walk_run, death anims). Player methods `isthrowinggrenade`, `ismeleeing`, `isonground` work in SP (stance 3 transitions seen in `coop5.log`); throw and melee events with them are not yet seen in a log.

## Animation trees: what the limit really counts (2026-11-14, verified in the game)
- `animtrees/<name>.atr` is a plain text list of xanim names (nested `{ }` groups are sync groups). A mod can ship its OWN tree file (ours: `coop_puppet.atr`, 18 top-level nodes) built from any xanim clips that exist in the stock archives (`xanim/<name>`), e.g. `prone_crawl`, `prone_aim_idle`, `stand_rechamber`, `crouch_rechamber`, `jump_across_100`, `jump_down_56`.
- The error "Cannot dynamically use an anim tree of size greater than 32" (useanimtree on a script_model) is decided by how many DISTINCT `%anim` names the script file mentions for that tree: with the same fakeShooters.atr, a script naming 26 clips worked, one naming 28 failed. The tree file itself (42 names, 15 top-level) was not the problem. Remove unused `%names` (debug blocks!) instead of looking for a smaller tree.
- When the error happens the entity has no tree: it stands in the T-pose and every setflaggedanimknobrestart on it raises "does not have an animation tree".
- Real third-person clips that exist for AI in the game files: jump (`jump_across_100`, `jump_down_56`), prone (`prone_aim_idle`, `prone_crawl`, `prone_aim2crawl`, `prone_crawl2aim`, `crouch2prone_gunsupport`, `pb_prone_*` for multiplayer), rechamber, grenade throw (`grenader_throw`, `crouch_grenade_throw`), melee (`aim2melee_right`), crouch run (`crouchrun_loop_forward_1..4`).
