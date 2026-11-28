# How to play / Как играть

🇬🇧 English first, 🇷🇺 русская версия ниже.

## English

**Once, on each PC**

1. Close the game. Unpack the release zip and run `CoD2Coop.exe`.
2. Choose **1) Install the mod**. It builds the mod from your own game files (about half a minute; PowerShell must be available) and
   installs a small `d3d9.dll` proxy + the bridge `cod2net5.dll`. If a different `d3d9.dll` (for example ReShade) is already in the game folder,
   the installer does **not** touch it and tells you.
3. Check the game firewall prompt the first time: allow `CoD2SP_s.exe` on **both** private and public networks (Radmin VPN counts as public).

**Every time**

1. Start Radmin VPN on both PCs (or use one local network).
2. Start the game through Steam. The main menu has a **CO-OP** button.
3. **Host:** CO-OP → **HOST**. Then start a **new** mission (Single Player → any mission). Do not load a save as the first thing.
4. **Guest:** CO-OP → leave `auto` in the address field → **JOIN**. The guest does not need to pick a mission: the guest's game follows the host
   (when the host is in the briefing the guest loads the mission that follows it).
5. At the start of each mission both PCs wait for each other (a panel "Waiting for the other player", up to 20 s), then the mission starts for both.

**In the mission**

- You see your partner as a soldier of your campaign: running, crouching, lying, jumping, shooting, reloading, throwing grenades.
- **F8** teleports you to your partner. **F7** (second player): loads the host's latest checkpoint, so you catch up with the host's world.
- If one player dies, both continue from the last shared checkpoint (**Mission: MANUAL** in the CO-OP screen turns this off).
- On `libya` and `88ridge` each player drives a tank of his own; the guest is seated in a second tank automatically.

**Settings in the CO-OP screen** (stored on your PC): window mode (normal/borderless), mouse (normal/RAW INPUT), follow the host on/off,
the language of the texts follows the game language (English, French, German, Italian, Spanish, Russian).

**Difficulty:** the guest takes the host's difficulty with the mission.

## Русский

**Один раз на каждом ПК**

1. Закройте игру. Распакуйте архив релиза и запустите `CoD2Coop.exe`.
2. Выберите **1) Install the mod**. Мод собирается из ваших файлов игры (около полминуты; нужен PowerShell) и ставит небольшой прокси
   `d3d9.dll` и мост `cod2net5.dll`. Если в папке игры уже есть другой `d3d9.dll` (например, ReShade), установщик его **не трогает** и сообщает об этом.
3. Когда Windows спросит про брандмауэр: разрешите `CoD2SP_s.exe` и для частных, и для общедоступных сетей (Radmin VPN считается общедоступной).

**Каждый раз**

1. Запустите Radmin VPN на обоих компьютерах (или используйте одну локальную сеть).
2. Запустите игру через Steam. В главном меню есть кнопка **CO-OP**.
3. **Хост:** CO-OP → **HOST**. Затем начните **новую** миссию (Single Player → любая). Первым делом не загружайте сохранение.
4. **Гость:** CO-OP → в поле адреса оставьте `auto` → **JOIN**. Гостю не нужно выбирать миссию: его игра идёт за хостом
   (если хост в брифинге, гость сразу загружает миссию, которая идёт после него).
5. В начале каждой миссии оба ПК ждут друг друга (панель «Ждём второго игрока», до 20 с), потом миссия стартует у обоих.

**В миссии**

- Напарник виден как солдат вашей кампании: бегает, приседает, ложится, прыгает, стреляет, перезаряжается, бросает гранаты.
- **F8** переносит вас к напарнику. **F7** (второй игрок): загружает последнюю контрольную точку хоста, чтобы догнать его мир.
- Если погиб один игрок, оба продолжают с последней общей контрольной точки (кнопка **Mission: MANUAL** в экране CO-OP отключает это).
- На `libya` и `88ridge` каждый ведёт свой танк; гостя сажает во второй танк автоматически.

**Настройки в экране CO-OP** (хранятся на вашем ПК): режим окна (обычный/без рамки), мышь (обычная/RAW INPUT), следование за хостом,
язык текстов берётся из языка игры (английский, французский, немецкий, итальянский, испанский, русский).

**Сложность:** гость берёт сложность хоста вместе с миссией.
