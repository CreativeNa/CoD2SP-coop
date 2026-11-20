# Known issues / Известные проблемы (0.7.8)

Nothing here stops the game. / Ничто из этого не мешает игре.

## Visuals / Внешний вид

- The partner's **tank** (on `libya` and `88ridge`) is a plain model: its turret does not turn, the tracks keep moving while it stands, the shot effect is a stand-in. /
  Танк напарника — обычная модель: башня не вращается, гусеницы крутятся на месте, эффект выстрела условный.
- The partner's **smoke grenade** may spin on the spot after landing. / Дымовая граната напарника может крутиться на месте после падения.
- Without a weapon, crouching and crawling use the poses of a soldier with a rifle (the arms look inexact); no unarmed crawl exists in the game. /
  Без оружия в приседе и ползком используются позы с винтовкой (руки выглядят неточно).
- While crouching **with** a weapon the puppet moves without a step animation (a held aiming pose). / В приседе с оружием кукла движется без анимации шага.
- Falling and landing animations are simple; the jump animation is a stand-in. / Падение и приземление простые, прыжок — временная анимация.

## Gameplay / Игровой процесс

- Damage of the partner's grenades to enemies is only weakly tested. / Урон гранат напарника врагам проверен слабо.
- Only tanks are driven by a player; trucks, planes and boats are not (the partner rides along). / Игрок ведёт только танки.
- Real mounted-gun firing (turrets) of the partner is not shown, only the shot stand-in. / Стрельба напарника с установленных пулемётов показывается условно.
- Scripted scenes can still drift between the two PCs; the shared checkpoints and the start barrier remove most of the difference. /
  Сценарные сцены могут расходиться; общие контрольные точки и барьер старта убирают большую часть различий.
- Enemies see and attack the player they are scripted against; there is no shared AI. / Общего ИИ нет.

## Performance / Производительность

- A slow CPU needs 9 s and more for a mission load (measured on a 2.5 GHz laptop); the faster PC waits at the start barrier. /
  Медленный процессор грузит миссию 9 с и больше; быстрый ПК ждёт на барьере старта.

## Install / Установка

- The installer is 32-bit; in environments that forbid 32-bit programs to write into `Program Files` it reports windows error 5. /
  Установщик 32-битный; там, где запрещена запись в `Program Files`, он сообщает ошибку 5 (запустите от администратора).
- The files are not code-signed; antivirus false positives are possible (see [ANTIVIRUS.md](ANTIVIRUS.md)). / Файлы не подписаны.
