# Known issues

These limitations apply to `v0.1.0-alpha.1`. They reflect observed behavior during early alpha testing.

## Platform and compatibility

- **Supported version:** Princess Peach: Showtime! USA version 1.0.0 (Title ID `01007A3009184000`) only.
- **Physical hardware (Atmosphere):** Support is experimental. A generated `main.npdm` is included with appropriate kernel capabilities and permissions, but broad hardware coverage is ongoing.
- **Controller setup:** Both controllers must be configured before starting the game.

## Gameplay and camera

- **Cinematic cutscenes:** Scripted single-player cinematics and event sequences focus on Player 1.
- **Camera bounding:** In fast-moving vertical rooms or narrow corridors, camera bounds may briefly constrain Player 2 until players realign.
- **Specialized transformation mini-games:** Certain vehicle or chase mini-games originally designed around single-player mechanics may have limited Player 2 interactions.
- **Distance recovery:** The distance leash brings Player 2 back to Player 1 if separation exceeds configured limits in `settings.ini`. In rare uneven terrain, P2 may need to jump to establish ground contact.

## Reporting new issues

Please test with unmodified base game saves and unrelated mods disabled. Provide reproduction steps, target emulator/system version, and relevant log lines when submitting an issue.
