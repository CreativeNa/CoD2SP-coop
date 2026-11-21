🇬🇧 **English** — below.  🇷🇺 **[Русская версия](#ru)** — ниже.

# Call of Duty 2 Co-op

Unofficial two-player co-op for the **single-player campaign of Call of Duty 2**. Early beta.
Not affiliated with the game's publisher or developers. Free.

**How to play:** [docs/HOW_TO_PLAY.md](docs/HOW_TO_PLAY.md) — a one-screen guide.

## Requirements

- Call of Duty 2, **single player, version 1.3**: the Steam build (`CoD2SP_s.exe`, SHA-256 `AB17A89C…2281`) or the retail 1.3 build
  (SHA-256 `64E7ADCA…5A48`). Other builds are refused by the installer.
- Windows 10/11 (PowerShell 5.1 that comes with Windows)
- Radmin VPN (recommended) or one local network
- **A legitimate copy of the game is required.** The package contains no game files and bypasses no protection: the installer builds the
  mod archive **on your PC from your own game files**. Pirated copies are not supported.
  The mod itself is distributed under the licence in [LICENSE](LICENSE) — free, no warranty.

## Installation

1. Unpack `CoD2Coop-<version>.zip`, close the game
2. Run `CoD2Coop.exe` → **1) Install the mod** (about half a minute: the mod is built from your game files)
3. Start the game through Steam: the main menu now has a **CO-OP** button

The installer writes into the game folder (`main\zz_coop.iwd`, `main\zz_coop_lang.iwd`, `d3d9.dll`, `cod2net5.dll`, `coop_mod_version.txt`);
**Uninstall** in the same menu removes everything it wrote. Your saves and the stock game files are not touched.

## Co-op in two lines

1. **Host** — main menu → **CO-OP → HOST**, then starts a **new** mission (not a loaded save).
2. **Guest** — main menu → **CO-OP → JOIN** (address `auto` finds the host by itself). The guest's game opens the same mission automatically.

At the start of every mission both PCs wait for each other (a small "Waiting for the other player" panel), so the mission begins at the same moment.

## What is shared

Position, stance, jumping, shooting and reloading (with flash), grenades and smoke, melee; kills of enemies; mission progress flags (optional);
death of either player (both go back to the shared checkpoint); the player tank on the missions where you drive one (`libya`, `88ridge`);
**F8** teleports you to your partner; **F7** (second player) takes the host's current checkpoint.

## Antivirus

The mod uses a small `d3d9.dll` proxy that loads the bridge into the game, so some antivirus products may flag it. This is a **false positive**
(the source is in this repository). Do not disable your antivirus: exclusions and file hashes are in [docs/ANTIVIRUS.md](docs/ANTIVIRUS.md).

## Found a bug

Run `CoD2Coop.exe logs` (or the "Collect logs" item of its menu): it puts a `CoD2Coop-logs-*.zip` next to the program (addresses are masked).
Send it with a few lines about what you did and what you saw. For connection problems logs from **both PCs** are needed.
Use the issue form of this repository.

## Documentation

| Document | Contents |
|---|---|
| [HOW_TO_PLAY.md](docs/HOW_TO_PLAY.md) | how to install and play |
| [TWO_PC_TEST.md](docs/TWO_PC_TEST.md) | network, ports, test order, what to send back |
| [TWO_PC_CHECKLIST.md](docs/TWO_PC_CHECKLIST.md) | step-by-step checklist for a test session |
| [ANTIVIRUS.md](docs/ANTIVIRUS.md) | false positives, exclusions, hashes |
| [SECURITY.md](docs/SECURITY.md) | who to play with, ports, firewall, reporting a problem |
| [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | common errors |
| [BUGS.md](docs/BUGS.md) | known issues |
| [releases/0.7.8.md](docs/releases/0.7.8.md) | notes for the current build |
| [RELEASE_INTEGRITY.md](docs/RELEASE_INTEGRITY.md) | how to verify the build and hashes |
| [AI_TRIAGE.md](docs/AI_TRIAGE.md) | what is read in a report and how it is triaged |
| [BUILD.md](docs/BUILD.md) | build from source |
| [RE/](docs/RE/README.md) | engine research and the bridge design |

## Status

Beta. Working: link and automatic mission follow, partner puppet with weapons, shots, grenades, death and shared checkpoints,
start barrier, tanks on `libya` and `88ridge`. Known visual defects are listed in `BUGS.md`. Only tanks are driven by a player;
trucks, planes and boats are not.

---

<a id="ru"></a>

# Call of Duty 2 Co-op — Русская версия

Неофициальный кооператив на двоих для **одиночной кампании Call of Duty 2**. Ранняя бета.
Не связан с издателем и разработчиками игры. Бесплатно.

**Как играть:** [docs/HOW_TO_PLAY.md](docs/HOW_TO_PLAY.md) — короткая инструкция на один экран.

## Что нужно

- Call of Duty 2, **одиночная игра, версия 1.3**: сборка Steam (`CoD2SP_s.exe`, SHA-256 `AB17A89C…2281`) или коробочная 1.3
  (SHA-256 `64E7ADCA…5A48`). Другие сборки установщик не принимает.
- Windows 10/11 (PowerShell 5.1 из состава Windows)
- Radmin VPN (рекомендуется) или одна локальная сеть
- **Лицензионная копия игры — обязательно.** Архив не содержит файлов игры и не обходит защиту: установщик собирает архив мода
  **на вашем компьютере из ваших файлов игры**. С пиратскими копиями мод не поддерживается.
  Сам мод распространяется по лицензии из файла [LICENSE](LICENSE) — бесплатно, без гарантий.

## Установка

1. Распаковать `CoD2Coop-<версия>.zip`, закрыть игру
2. Запустить `CoD2Coop.exe` → **1) Install the mod** (около полминуты: мод собирается из ваших файлов игры)
3. Запустить игру через Steam: в главном меню появилась кнопка **CO-OP**

Установщик пишет в папку игры (`main\zz_coop.iwd`, `main\zz_coop_lang.iwd`, `d3d9.dll`, `cod2net5.dll`, `coop_mod_version.txt`);
пункт **Uninstall** в том же меню убирает всё, что он записал. Ваши сохранения и файлы игры не трогаются.

## Кооп в двух строках

1. **Хост** — главное меню → **CO-OP → HOST**, затем начинает **новую** миссию (не загрузку сохранения).
2. **Гость** — главное меню → **CO-OP → JOIN** (адрес `auto` сам находит хоста). Игра гостя сама открывает ту же миссию.

В начале каждой миссии оба компьютера ждут друг друга (небольшая панель «Ждём второго игрока»), и миссия начинается у обоих одновременно.

## Что синхронизируется

Позиция, поза, прыжок, стрельба и перезарядка (со вспышкой), гранаты и дымовые, удар прикладом; убийства врагов; флаги заданий (по желанию);
смерть любого игрока (оба возвращаются на общую контрольную точку); танк игрока на миссиях, где вы ведёте танк (`libya`, `88ridge`);
**F8** переносит вас к напарнику; **F7** (второй игрок) загружает текущую контрольную точку хоста.

## Антивирус

Мод использует небольшой прокси `d3d9.dll`, который загружает мост в игру, поэтому часть антивирусов может ругаться. Это **ложное срабатывание**
(исходный код в этом репозитории). Не отключайте антивирус: исключения и хеши файлов — в [docs/ANTIVIRUS.md](docs/ANTIVIRUS.md).

## Нашли баг

Запустите `CoD2Coop.exe logs` (или пункт «Collect logs» в его меню): рядом ляжет `CoD2Coop-logs-*.zip` (адреса скрыты).
Пришлите его с несколькими строками: что делали и что увидели. Для проблем со связью нужны логи **с обоих компьютеров**.
Используйте форму обращения (issue) этого репозитория.

## Документация

| Документ | О чём |
|---|---|
| [HOW_TO_PLAY.md](docs/HOW_TO_PLAY.md) | как установить и играть |
| [TWO_PC_TEST.md](docs/TWO_PC_TEST.md) | сеть, порты, порядок проверок, что прислать |
| [TWO_PC_CHECKLIST.md](docs/TWO_PC_CHECKLIST.md) | чек-лист на вечер теста |
| [ANTIVIRUS.md](docs/ANTIVIRUS.md) | ложные срабатывания, исключения, хеши |
| [SECURITY.md](docs/SECURITY.md) | с кем играть, порты, брандмауэр, как сообщить о проблеме |
| [TROUBLESHOOTING.md](docs/TROUBLESHOOTING.md) | типичные ошибки |
| [BUGS.md](docs/BUGS.md) | известные проблемы |
| [releases/0.7.8.md](docs/releases/0.7.8.md) | заметки к текущей сборке |
| [RELEASE_INTEGRITY.md](docs/RELEASE_INTEGRITY.md) | как проверить сборку и хеши |
| [AI_TRIAGE.md](docs/AI_TRIAGE.md) | что читается в отчёте и как он разбирается |
| [BUILD.md](docs/BUILD.md) | сборка из исходников |
| [RE/](docs/RE/README.md) | исследование движка и устройство моста |

## Состояние

Бета. Работает связь и автоматический переход за хостом, кукла напарника с оружием, стрельба, гранаты, смерть и общие контрольные точки,
барьер старта, танки на `libya` и `88ridge`. Известные визуальные дефекты перечислены в `BUGS.md`. Игрок ведёт только танки;
грузовики, самолёты и лодки не управляются.
