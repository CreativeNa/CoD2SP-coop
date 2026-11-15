# Script built-ins added by the bridge

The bridge registers these GSC functions through the proxy's stub table (`bridge/coopapi.h`; 48 stub slots, 39 used). They are called from
the co-op script body in `tools/make_mod.ps1`. "state" means the last report of the partner; the queue functions return one item per call.

| Function | Returns / does |
|---|---|
| `coop_version()` | the bridge version string |
| `coop_ping(n)` | `n+1`, a smoke test; `7002` is also the "unlink the puppet" marker in the log |
| `coop_publish_state(origin, yaw, stance, evt, weapon, parent, health)` | sends the local player's state |
| `coop_peer_state()` / `coop_peer_yaw()` / `coop_peer_seq()` | partner's position, yaw, packet counter |
| `coop_peer_stance()` / `coop_peer_event()` / `coop_peer_weapon()` / `coop_peer_parent()` / `coop_peer_health()` | stance, event word, weapon index, vehicle entity number, health % |
| `coop_peer_solo()` | 1 when the "partner" is this very game (solo mirror test) |
| `coop_send_kill(origin)` / `coop_kill_count()` / `coop_pop_kill()` | enemy kills to mirror |
| `coop_pending_level()` | the level the script must change to (`changelevel`) |
| `coop_player_link(...)` / `coop_player_parent()` | the vehicle the local player is linked to |
| `coop_pop_teleport()` | a pending F8 teleport target |
| `coop_pop_peer_death()` / `coop_death_started()` | the partner died / the local death screen started |
| `coop_is_guest()` | 1 on the guest PC (not in the solo test) |
| `coop_send_flag(name)` / `coop_pop_flag()` / `coop_flag_applied(name)` | mission flag mirror |
| `coop_send_grenade(weapon, offset, velocity)` / `coop_pop_grenade()` / `coop_grenade_org()` / `coop_grenade_vel()` | thrown grenades |
| `coop_send_tank(origin, hullAngles, turretAngles, shots)` | the local tank |
| `coop_tank_pos()` / `coop_tank_ang()` / `coop_tank_turret()` / `coop_tank_fire()` / `coop_tank_age()` | the partner's tank (age in ms) |
| `coop_link_active()` | 1 while a partner is linked |
| `coop_barrier_begin(map, skill, delay)` / `coop_barrier_active()` | start barrier (pause until the partner is ready) |
| `coop_apply_host_skill()` | writes the host's difficulty into `g_gameskill` before the level reads it (guest only) |

Strings are copied out of the script stack by the bridge's own helpers (`E_GetStringCopy`, `E_GetVector`, `E_AddInt`, `E_AddVector` …,
register-convention engine functions called through small assembly thunks); see [01_ENGINE_AND_CODE.md](01_ENGINE_AND_CODE.md) for the addresses.
