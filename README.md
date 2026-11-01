# Princess Peach Showtime Local Co-op

Princess Peach Showtime Local Co-op adds a fully playable second Peach actor to **Princess Peach: Showtime! version 1.0.0**, enabling two players to play together cooperatively on a single shared screen.

This is a local co-op modification built using an ExLaunch `subsdk9` module; it is not an online or networked multiplayer mod.

This is an early alpha release. Save backups are recommended. Known limitations and gameplay notes are documented in [KNOWN_ISSUES.md](KNOWN_ISSUES.md).

## Support status

| Target | Status | Notes |
|---|---|---|
| Princess Peach: Showtime! | **USA 1.0.0 (v0) only** | Title ID `01007A3009184000`. Other game versions are unsupported. |
| Ryujinx | Primary target | The alpha release, automated installer, and gameplay are primarily tested here. |
| Atmosphere / physical Switch | Experimental | The package includes the generated `main.npdm` with verified kernel capabilities. |

## What the mod adds

- **Two playable Peach characters** on the same screen, each controlled by an independent controller.
- **Shared dynamic camera** that centers on both players, smoothly zooming and widening its field of view with distance.
- **Independent health and damage** pipelines, ensuring separate health gauges and life slot tracking for both players.
- **Distance-based recovery leash** that safely teleports Player 2 back to Player 1 if they become separated.
- **Customizable Player 2 appearance** featuring a distinctive emissive cyan tint, fully configurable via `settings.ini`.
- **Full native engine interaction**: both players use the game's native locomotion, physics, jump mechanics, and animations.

## Requirements

- A legally dumped copy of Princess Peach: Showtime! **version 1.0.0 (USA v0, Title ID 01007A3009184000)**.
- A current installation of Ryujinx (or Atmosphere on hardware).
- Two game controllers configured as Player 1 and Player 2 before booting.
- The `PrincessPeachShowtimeLocalCoop-v0.1.0-alpha.1.zip` release package.

## Install on Ryujinx

1. Download `PrincessPeachShowtimeLocalCoop-v0.1.0-alpha.1.zip` and its `.sha256` sidecar from the [GitHub Releases](https://github.com/Gudui/PrincessPeachShowtimeLocalCoop/releases) page.
2. Verify the package checksum:

   ```powershell
   Get-FileHash .\PrincessPeachShowtimeLocalCoop-v0.1.0-alpha.1.zip -Algorithm SHA256
   ```

3. Extract the ZIP archive.
4. Open PowerShell in the extracted folder and run:

   ```powershell
   Unblock-File .\install.ps1
   .\install.ps1
   ```

   The installer verifies every packaged payload file before installing it into your Ryujinx mod directory (`%APPDATA%\Ryujinx\mods\contents\01007a3009184000`). Existing files at the destination are safely backed up before replacement.

5. Configure both controllers in Ryujinx, launch Princess Peach: Showtime!, and enjoy local co-op!

For manual installation, copy the `payload/contents` folder directly into your Ryujinx `mods` directory, preserving the title ID directory structure.

## Configuration

The mod settings file is located at:

```text
contents/01007A3009184000/romfs/PShowCoop/settings.ini
```

Edit this file with your emulator closed. Settings are automatically reloaded during scene transitions.

- `p2.tint.enabled`: Enable (1) or disable (0) the Player 2 color tint.
- `p2.tint.r`, `p2.tint.g`, `p2.tint.b`: Emissive color channel multipliers (default cyan: 0.0 / 1.0 / 1.0).
- `p2.tint.intensity`: Emissive brightness intensity.
- `p2.leash.distance`: Maximum distance before Player 2 is automatically caught up to Player 1.
- `warp.target`: Optional map name to warp directly into upon boot (e.g. `Fencer_A`).

## Troubleshooting

### Player 2 does not appear

- Verify your game version is 1.0.0 (USA v0, Title ID `01007A3009184000`).
- Ensure both controllers are configured as separate input devices in Ryujinx before launching.
- Check that the mod is enabled in the Ryujinx mod manager.
- Temporarily disable other gameplay or executable mods to eliminate conflicts.

### The game crashes on loading a stage

- Re-run `install.ps1` to ensure `subsdk9`, `main.npdm`, and `settings.ini` all originate from the same release package.
- If you customized `warp.target` in `settings.ini`, verify the map name is valid.
- Check emulator logs for `[PShowCoop]` messages for diagnostic details.

### Controllers control the wrong character

- In Ryujinx Input Settings, make sure Player 1 is assigned to controller 1 and Player 2 is assigned to controller 2 before booting the game.

## How it works

The mod is implemented as an ExLaunch `subsdk9` native module targeting Nintendo Switch Title ID `01007A3009184000`.

Rather than using synthetic shims or external hacks, it hooks cleanly into Unreal Engine 4 and the game's native actor lifecycle:
- Spawns a second native `APlayerCharacter` using the engine's standard actor spawning pipeline.
- Hooks the viewport camera manager to compute dynamic bounding boxes around both players.
- Connects Player 2 to independent controller input buffers and delegates.
- Preserves the game's built-in damage receivers, iframes, and UI presentation structures.

## Building from source

To build the mod from source, clone with submodules and run the build pipeline:

```powershell
git clone --recurse-submodules https://github.com/Gudui/PrincessPeachShowtimeLocalCoop.git
Set-Location PrincessPeachShowtimeLocalCoop
.\scripts\build.ps1 -UseDocker
.\scripts\package.ps1
.\scripts\verify.ps1
```

For local compilation without Docker, install devkitPro (`devkitA64`) and run:

```powershell
.\scripts\build.ps1 -DevkitProRoot C:\devkitPro
```

## Contributing and credits

Please read [CONTRIBUTING.md](CONTRIBUTING.md) before opening an issue or pull request. Do not submit copyrighted game files, dumps, keys, or personal paths.

Credits, third-party licenses, and dependency information are in [CREDITS.md](CREDITS.md) and [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).

Princess Peach: Showtime! and all related assets are copyright Nintendo. This is an unofficial, non-commercial fan project and is neither affiliated with nor endorsed by Nintendo.
