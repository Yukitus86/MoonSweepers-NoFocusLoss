# Changelog

All notable changes to this project are documented here.
The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/), and this project uses
[Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- GitHub Actions: every push is built automatically, and pushing a `v*` tag publishes a release.
- Issue templates for bug reports and feature requests.
- Linux / Steam Deck (Proton) instructions (untested).

### Changed
- Rewrote the README: badges, compatibility table, troubleshooting section.

## [1.0.0] - 2026-11-04

### Added
- First release.
- Hides window focus loss (`WM_ACTIVATE`, `WM_ACTIVATEAPP`, `WM_KILLFOCUS`) from Unity, so vehicles keep driving while
  you're alt-tabbed.
- Proxy `version.dll`: no mod loader required.
- USER32 import hooks keep the mouse free, block background input and stop the game from stealing focus back.
- `NoFocusLoss.ini` with `Enabled`, `KeepHeldKeys`, `BlockBackgroundInput`, `ReleaseCursor` and `Log`.

[Unreleased]: https://github.com/Yukitus86/MoonSweepers-NoFocusLoss/compare/v1.0.0...HEAD
[1.0.0]: https://github.com/Yukitus86/MoonSweepers-NoFocusLoss/releases/tag/v1.0.0
