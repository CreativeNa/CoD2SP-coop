# Security / Безопасность

## English

**Play only with people you trust.** The mod connects two PCs and the other player's PC sends data that your game acts on: positions, mission flags, kill events, and the host's checkpoint files (saved games). The game itself opens and parses a checkpoint it receives. The mod checks the name, the size (12 MB at most), the CRC, the mission name inside the file and the place a checkpoint is written to, and where the data comes from, but it cannot check what is inside a saved game. A host you do not trust could, in theory, send a damaged file that still passes those checks. The previous `coopshared.svg` is kept as `coopshared.prev.svg`, and `coop_accept_saves off` disables receiving saved games completely.

**What the mod does not do.** It downloads nothing and has no update function. It runs no command that the other PC sends. It writes no program or DLL from the network (the only DLL it loads is its own `cod2net5.dll` from the game folder). Names that arrive from the partner (mission, flags, keys) are cut to letters, digits and `_` (`-` and spaces where a name needs them) before use. A world id (`<key>#<n>`, the stage 1 death event) is accepted only when it matches `SecWorldId`: at most 40 characters of letters, digits, `_` and `-`, then `#` and up to five digits — so it cannot carry a line feed, a path or a long string into the log or the script. An id that names no actor on this PC only queues a pending kill (60 s at most) for an actor that may spawn later; it changes nothing else.

**Ports** (UDP 27201 and 27202, TCP 27203; see [TWO_PC_TEST.md](TWO_PC_TEST.md)):

- Do **not** forward these ports on your router and do not play over the open internet. Use a LAN, or a VPN such as Radmin VPN with a password.
- The host takes the first PC that contacts it as the guest. On an untrusted network a stranger can take that place before your friend does; start HOST only when your friend is ready to join. The guest with `auto` follows the first host it hears on the network; if the network is not yours, type the host's address.
- In the Windows firewall allow `CoD2SP_s.exe` only for the network type you play on (Radmin VPN is shown as a *public* network by Windows). Do not open the ports for every address and every program.

**Logs.** `coop5.log` and `coop_net.csv` hold no user name and no file paths; IP addresses in the log are masked (`26.1.x.x`). `tools/log_collector.py` has no password and listens on this PC only by default: to collect from another PC give `--bind <your VPN address>`, on a network you trust and only while you collect logs.

**Reporting a problem.** Please do not post details of a security problem in a public issue. Use the *Security* tab of this repository (*Report a vulnerability*) if it is shown; otherwise open an issue that only says you have a security report, without details.

## Русский

**Играйте только с теми, кому доверяете.** Мод соединяет два ПК, и ПК другого игрока присылает данные, по которым действует ваша игра: положение, флаги миссии, события убийств и файлы чекпоинтов хоста (сохранения). Присланный чекпоинт игра открывает и разбирает сама. Мод проверяет имя, размер (не больше 12 МБ), CRC, имя миссии внутри файла, место записи и источник чекпоинта, но проверить, что внутри сохранения, не может. Хост, которому вы не доверяете, теоретически может прислать испорченный файл, который пройдёт эти проверки. Предыдущий `coopshared.svg` сохраняется как `coopshared.prev.svg`, а `coop_accept_saves off` полностью отключает приём сохранений.

**Чего мод не делает.** Он ничего не скачивает и не обновляется сам. Не выполняет команды, присланные другим ПК. Не записывает программы и DLL из сети (единственная DLL, которую он загружает, это его собственная `cod2net5.dll` из папки игры). Имена, пришедшие от партнёра (миссия, флаги, ключи), перед использованием урезаются до букв, цифр и `_` (и `-`, пробела там, где имя его допускает). World id (`<key>#<n>`, событие смерти этапа 1) принимается только если проходит `SecWorldId`: не больше 40 символов из букв, цифр, `_` и `-`, затем `#` и до пяти цифр — поэтому он не может пронести в лог или в скрипт перевод строки, путь или длинную строку. Id, которому на этом ПК не соответствует ни один актор, лишь ставит в очередь отложенное убийство (не дольше 60 с) для актора, который может появиться позже; больше он ничего не меняет.

**Порты** (UDP 27201 и 27202, TCP 27203; см. [TWO_PC_TEST.md](TWO_PC_TEST.md)):

- **Не** пробрасывайте эти порты на роутере и не играйте через открытый интернет. Используйте локальную сеть или VPN, например Radmin VPN с паролем.
- Хост принимает за гостя первый ПК, который к нему обратится. В чужой сети посторонний может занять это место раньше вашего друга: запускайте HOST, когда друг готов подключаться. Гость с `auto` идёт к первому хосту, которого услышал в сети; если сеть не ваша, впишите адрес хоста.
- В брандмауэре Windows разрешайте `CoD2SP_s.exe` только для того типа сети, в которой играете (Radmin VPN Windows показывает как *общедоступную*). Не открывайте порты для всех адресов и всех программ.

**Логи.** В `coop5.log` и `coop_net.csv` нет имени пользователя и путей к файлам; IP-адреса в логе замаскированы (`26.1.x.x`). У `tools/log_collector.py` нет пароля, по умолчанию он слушает только этот ПК: чтобы собирать с другого ПК, укажите `--bind <адрес вашего VPN>` в доверенной сети и только пока собираете логи.

**Сообщить о проблеме.** Пожалуйста, не публикуйте подробности проблемы безопасности в открытом issue. Используйте вкладку *Security* этого репозитория (*Report a vulnerability*), если она есть; иначе откройте issue, в котором только сказано, что есть сообщение о безопасности, без подробностей.
