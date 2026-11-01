# Contributing

Thank you for your interest in improving Princess Peach Showtime Local Co-op. Bug reports, documentation improvements, and pull requests are welcome.

## Before opening an issue

1. Read [KNOWN_ISSUES.md](KNOWN_ISSUES.md) and search existing issues.
2. Confirm reproduction on Princess Peach: Showtime! USA version 1.0.0 (Title ID `01007A3009184000`).
3. Note your emulator/system version, controller configuration, and reproduction steps.
4. Retest with any unrelated mods disabled.
5. Redact personal paths, usernames, and unrelated log noise.

Never submit copyrighted game binaries, ROM dumps, encryption keys, firmware, or full save archives.

## Pull requests

- Keep each pull request focused on a single issue or feature.
- Clearly describe the observable gameplay behavior before and after your change.
- Guard all pointer chains and ensure solo (single-player) behavior remains unaffected.
- Do not introduce machine-specific absolute paths, developer credentials, or binary dumps.
- Run the build and verification scripts prior to submitting:

  ```powershell
  .\scripts\build.ps1
  .\scripts\package.ps1
  .\scripts\verify.ps1
  ```

## Licensing

By contributing to Princess Peach Showtime Local Co-op, you agree that your contributions are licensed under the repository's [GNU General Public License v2](LICENSE). Only submit code you have authored or have the legal right to license.
