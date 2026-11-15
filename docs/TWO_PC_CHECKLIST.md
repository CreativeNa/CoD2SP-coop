# Two-PC checklist / Чек-лист на двух ПК

Mark each line. If a line fails, stop, collect the logs of both PCs (`CoD2Coop.exe logs`) and send them.
Отмечайте строки по порядку. Если строка не выполняется, остановитесь, соберите логи обоих ПК (`CoD2Coop.exe logs`) и пришлите.

## Before / До игры

- [ ] Both PCs: same release version (`CoD2Coop.exe check` prints it) / одна версия на обоих
- [ ] Both: `CoD2Coop.exe check` says "Mod: installed (current)" and "Autoload: installed (current)"
- [ ] Both PCs in Radmin VPN (or one LAN) and can ping each other / оба в Radmin VPN (или одной сети), пинг идёт
- [ ] Firewall: `CoD2SP_s.exe` allowed on private **and** public networks / брандмауэр разрешает для обеих сетей
- [ ] Game closed everywhere before the install / игра закрыта при установке

## Connection / Связь

- [ ] Host: CO-OP → HOST. Guest: CO-OP → JOIN (address `auto`) / хост HOST, гость JOIN
- [ ] Status line on both: "linked to …" / в строке статуса «linked to …»
- [ ] Host starts a **new** mission; the guest opens the same one by itself / хост начинает новую миссию, гость открывает ту же сам

## In the mission / В миссии

- [ ] "Waiting for the other player" at the start, then both start together / ожидание на старте, потом старт у обоих
- [ ] The partner is visible and moves, crouches, lies, jumps / напарник виден и двигается, приседает, ложится, прыгает
- [ ] Shots and reloads of the partner are visible / выстрелы и перезарядка напарника видны
- [ ] Grenades of the partner fly and explode (smoke: no damage) / гранаты напарника летят и взрываются
- [ ] Kills of one player also remove the enemy on the other PC / убийство у одного убирает врага у другого
- [ ] F8 brings you to the partner / F8 переносит к напарнику
- [ ] One player dies → both continue from the shared checkpoint / смерть одного → оба с общей точки
- [ ] End of the mission: the guest follows the host into the next one / конец миссии: гость идёт за хостом
- [ ] `libya` / `88ridge`: the guest is seated in a second tank; the partner's tank is visible and drives / гость во втором танке, танк напарника виден

## After / После

- [ ] `CoD2Coop.exe logs` on both PCs and send the two zips with the mission name and what looked wrong
