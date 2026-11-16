# Antivirus / Антивирус

## English

The mod works by placing a small **`d3d9.dll` proxy** next to the game. The game loads it at start (like ReShade does), and it loads the
bridge `cod2net5.dll` into the game process. Code that gets loaded into another program is exactly what antivirus products look for, so a
**false positive** is possible (typical names: generic "Trojan"/"Wacatac"/"heuristic"). The source of every file is in this repository
(`proxy/d3d9proxy.c`, `bridge/cod2net5.c`, `launcher/cod2coop.c`); the files are **not signed**.

What the files do: the proxy forwards `Direct3DCreate9` to the real `d3d9.dll` and loads the bridge only in `CoD2SP_s.exe`; the bridge talks UDP/TCP
to your partner (ports in [TWO_PC_TEST.md](TWO_PC_TEST.md)), writes `coop5.log` and reads/writes the game's memory to add the partner.
It does not touch other programs, the registry (read only) or your saves except the shared checkpoint files it receives from your partner.

**What to do:** do not disable your antivirus. If a file is flagged, restore it from quarantine and add an exclusion for the **game folder**
(for Windows Defender: Windows Security → Virus & threat protection → Manage settings → Exclusions → Add a folder). The same exclusion also
makes missions load a little faster.

**Hashes of the files in release 0.7.7** (SHA-256):

| File | SHA-256 |
|---|---|
| `CoD2Coop-0.7.7.zip` | `9ef7cf32a25110b7a257b9fe9f7bd4493cb051f8b2046bb0cab53a9eed157c3b` |
| `CoD2Coop.exe` | `79f07f38e5b7d61f10302659f136a427b3bd8ac42faec0f65fb492dac95184d9` |
| `d3d9.dll` (written into the game folder) | `864c9803da56d019946c753e689eae81599fd79bd0fb3b648ef5b04d83e2e887` |
| `cod2net5.dll` (written into the game folder) | `88fee3705bee78751f2e8bcad104b38ccd7004f8dbc6e31a7ec42b282b0d5263` |

Check: `Get-FileHash <file> -Algorithm SHA256` in PowerShell. To remove everything: `CoD2Coop.exe` → **Uninstall**.

## Русский

Мод работает через небольшой **прокси `d3d9.dll`** рядом с игрой. Игра загружает его при запуске (как ReShade), а он загружает мост `cod2net5.dll`
в процесс игры. Код, который загружается в чужую программу, — именно то, что ищут антивирусы, поэтому возможно **ложное срабатывание**
(типичные названия: «Trojan», «Wacatac», «эвристика»). Исходный код каждого файла лежит в этом репозитории
(`proxy/d3d9proxy.c`, `bridge/cod2net5.c`, `launcher/cod2coop.c`); файлы **не подписаны**.

Что делают файлы: прокси передаёт `Direct3DCreate9` настоящему `d3d9.dll` и загружает мост только в `CoD2SP_s.exe`; мост по UDP/TCP общается
с напарником (порты в [TWO_PC_TEST.md](TWO_PC_TEST.md)), пишет `coop5.log` и читает/пишет память игры, чтобы добавить напарника.
Он не трогает другие программы, реестр (только чтение) и ваши сохранения, кроме файлов общих контрольных точек, полученных от напарника.

**Что делать:** не отключайте антивирус. Если файл помечен, восстановите его из карантина и добавьте исключение для **папки игры**
(Windows Defender: Безопасность Windows → Защита от вирусов и угроз → Управление настройками → Исключения → Добавить папку). Это же исключение
немного ускоряет загрузку миссий.

**Хеши файлов релиза 0.7.7** (SHA-256) — в таблице выше. Проверка: `Get-FileHash <файл> -Algorithm SHA256` в PowerShell.
Чтобы убрать всё: `CoD2Coop.exe` → **Uninstall**.
