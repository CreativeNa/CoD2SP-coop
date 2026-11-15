# Release integrity / Проверка сборки

## English

**Verify the download.** In PowerShell: `Get-FileHash .\CoD2Coop-0.7.4.zip -Algorithm SHA256` and compare with the table in
[ANTIVIRUS.md](ANTIVIRUS.md) (zip, `CoD2Coop.exe`, and the two DLLs that the installer writes into the game folder). `CoD2Coop.exe.sha256`
inside the zip holds the hash of the exe.

**What is in the zip:** `CoD2Coop.exe` (the installer/launcher; the bridge and the proxy are embedded in it), `README.txt`, `log_collector.py`
(optional log receiver), `CoD2Coop.exe.sha256`. **Not in the zip:** any file of the game; the mod archive `main\zz_coop.iwd` is built on your PC from
your own game files by the PowerShell scripts embedded in the exe (`tools/make_mod.ps1`, `tools/patch_menu.ps1`; the same text is in this repository).

**Building from source** gives the same program but not the same bytes: the bridge embeds its build date and time, the installer embeds the result.
So a local build is verified by its behaviour and by `CoD2Coop.exe check`, not by the hash. See [BUILD.md](BUILD.md).

**After installation** `CoD2Coop.exe check` prints the version of the installed mod (`coop_mod_version.txt`), the state of the autoload and
whether the game is running. The first line of `coop5.log` shows the bridge version (`cod2net5 0.7.4 …`).

## Русский

**Проверка загрузки.** В PowerShell: `Get-FileHash .\CoD2Coop-0.7.4.zip -Algorithm SHA256` и сравнить с таблицей в
[ANTIVIRUS.md](ANTIVIRUS.md) (архив, `CoD2Coop.exe` и две DLL, которые установщик пишет в папку игры). В архиве `CoD2Coop.exe.sha256`
содержит хеш exe.

**Что в архиве:** `CoD2Coop.exe` (установщик/лаунчер; мост и прокси встроены), `README.txt`, `log_collector.py` (необязательный приёмник логов),
`CoD2Coop.exe.sha256`. **Чего нет:** никаких файлов игры; архив мода `main\zz_coop.iwd` собирается на вашем компьютере из ваших файлов игры
PowerShell-скриптами, встроенными в exe (`tools/make_mod.ps1`, `tools/patch_menu.ps1`; тот же текст лежит в этом репозитории).

**Сборка из исходников** даёт ту же программу, но не те же байты: мост встраивает дату и время сборки, установщик встраивает результат.
Поэтому локальную сборку проверяют по поведению и по `CoD2Coop.exe check`, а не по хешу. См. [BUILD.md](BUILD.md).

**После установки** `CoD2Coop.exe check` показывает версию установленного мода (`coop_mod_version.txt`), состояние автозагрузки и
запущена ли игра. Первая строка `coop5.log` показывает версию моста (`cod2net5 0.7.4 …`).
