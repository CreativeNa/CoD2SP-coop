# Two-PC test / Тест на двух ПК

## English

**Network.** Both PCs must reach each other directly: Radmin VPN (recommended; each PC gets an address like `26.x.x.x`) or one local network
(`192.168.x.x`). The guest normally finds the host by itself (UDP beacons); if it does not, type the host's address in the CO-OP screen.

**Ports** (open them for `CoD2SP_s.exe` in the Windows firewall, private **and** public networks):

| Port | Protocol | Use |
|---|---|---|
| 27201 | UDP | partner state, kills, grenades, tank, mission follow, ready/barrier |
| 27202 | UDP | the host announces itself (discovery) |
| 27203 | TCP | shared checkpoint files (only the linked partner is accepted) |

**Order of a test**

1. Both PCs: the same mod version (a different version shows "VERSION MISMATCH" in the CO-OP screen and in the log).
2. Host → HOST, guest → JOIN. The status line shows "linked to …" once packets flow.
3. Host starts a mission. The guest loads the same mission by itself. Both see "Waiting for the other player", then the mission starts.
4. Walk, crouch, jump, shoot, reload, throw a grenade; check that the partner does the same on the other screen.
5. Let one player die: both should return to the shared checkpoint.
6. Finish the mission: the guest follows the host into the next one.

**What to send back:** the zip from `CoD2Coop.exe logs` of **both** PCs, the mission name, the difficulty and a few lines on what looked wrong.

## Русский

**Сеть.** Компьютеры должны видеть друг друга напрямую: Radmin VPN (рекомендуется, у каждого адрес вида `26.x.x.x`) или одна локальная
сеть (`192.168.x.x`). Обычно гость сам находит хоста (UDP-сигналы); если нет, введите адрес хоста в экране CO-OP.

**Порты** (откройте для `CoD2SP_s.exe` в брандмауэре Windows, для частных **и** общедоступных сетей):

| Порт | Протокол | Для чего |
|---|---|---|
| 27201 | UDP | состояние напарника, убийства, гранаты, танк, переход за миссией, готовность |
| 27202 | UDP | хост объявляет себя (поиск) |
| 27203 | TCP | файлы общих контрольных точек (принимаются только от связанного напарника) |

**Порядок теста**

1. На обоих ПК одна версия мода (при разных версиях в экране CO-OP и в логе «VERSION MISMATCH»).
2. Хост → HOST, гость → JOIN. Когда пакеты идут, в строке статуса «linked to …».
3. Хост запускает миссию. Гость сам грузит ту же. У обоих «Ждём второго игрока», потом миссия стартует.
4. Походите, присядьте, прыгните, постреляйте, перезарядитесь, бросьте гранату; проверьте, что напарник делает то же на другом экране.
5. Дайте одному игроку погибнуть: оба вернутся на общую контрольную точку.
6. Пройдите миссию до конца: гость пойдёт за хостом в следующую.

**Что прислать:** zip из `CoD2Coop.exe logs` **с обоих** ПК, название миссии, сложность и несколько строк о том, что выглядело неправильно.
