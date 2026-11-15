# How a report is triaged / Как разбирается отчёт

## English

A report is the zip from `CoD2Coop.exe logs`: `coop5.log` (the bridge and proxy log of the session), `main\console.log` (the game's own log),
the versions, `coop_*` settings, and crash/hang reports if there are any. IP addresses are masked.

**Where to look first** (search the text):

| Search for | Means |
|---|---|
| `=== cod2net5` | the bridge version and game exe of the session (first line) |
| `script runtime error` / `script compile error` in `console.log` | an error in the mod's script or a stock script; the next 3 lines name the file and line |
| `VERSION MISMATCH` | the two PCs run different mod versions |
| `discovery:` / `peer learned` / `tx ready` | how the link was found |
| `follow:` | the guest following the host (mission, briefing skip), the shared-checkpoint loads after a death |
| `barrier:` | start barrier: `begin`, `ready`, `GO (…)`, `the engine dropped the pause` |
| `checkpoint sent` / `checkpoint received` / `refused` | shared checkpoint transfer (TCP 27203) |
| `difficulty:` | the guest taking the host's difficulty |
| `COOPT` | the player tank logic (`seated after N retries`, `tanks ready`) |
| `dvar watch: sv_running=… mapname=…` | the timeline of loads and mission changes |
| `Load time: N msec` | the engine's own load time (CPU-bound on slow PCs) |

**Typical decisions**

1. Version mismatch or missing install → ask to install the same release on both PCs.
2. Link lines missing → network/firewall (ports in [TWO_PC_TEST.md](TWO_PC_TEST.md)).
3. A `script runtime error` from our code → a bug: needs the mission, the line and both logs.
4. Everything in the log looks normal → ask for a screenshot and the time of the problem and compare with the other PC's log.

A report is only useful with the **mission name, difficulty, role (host/guest) and time**; please include them.

## Русский

Отчёт — это zip из `CoD2Coop.exe logs`: `coop5.log` (лог моста и прокси за сеанс), `main\console.log` (лог самой игры), версии, настройки `coop_*`
и отчёты о падениях/зависаниях, если есть. IP-адреса скрыты.

**Что смотреть в первую очередь** (поиск по тексту): таблица выше (строки `=== cod2net5`, `script runtime error`, `VERSION MISMATCH`, `discovery:`,
`follow:`, `barrier:`, `checkpoint …`, `difficulty:`, `COOPT`, `dvar watch`, `Load time`).

**Типичные решения**

1. Несовпадение версий или нет установки → поставить один релиз на оба ПК.
2. Нет строк о связи → сеть/брандмауэр (порты в [TWO_PC_TEST.md](TWO_PC_TEST.md)).
3. `script runtime error` из нашего кода → баг: нужны миссия, строка и оба лога.
4. В логе всё нормально → попросить скриншот и время проблемы и сравнить с логом второго ПК.

Отчёт полезен только вместе с **названием миссии, сложностью, ролью (хост/гость) и временем**: пожалуйста, укажите их.
