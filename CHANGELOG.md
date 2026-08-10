# Changelog

## 0.4.2.2 - 2026-08-10

### Added

- Debian, RPM, AppImage, and AUR packaging with deterministic source archives and checksums.
- Shared desktop icon and AppStream metadata for package managers and application launchers.
- KDE-compatible status notifier registration and watcher restart coverage.

### Changed

- Parse z.ai credit limits as rolling usage windows.
- Simplify installation and tray documentation and credit the upstream CodexBar project.

## 0.4.1 — 2026-08-01

### Added

- Native C23 Linux runtime with 66 upstream provider integrations and strict source routing.
- ncurses terminal UI, Waybar output, StatusNotifierItem tray integration, XDG autostart, and desktop launchers.
- Text and JSON usage, cards, cost, history, sessions, guard, hooks, diagnostics, cache, account, and dashboard commands.
- Adaptive refresh coordination, status polling, last-good publication, account-scoped transitions, notifications,
  hooks, predictive pace, historical utilization, and local session discovery.
- Managed Codex and token-account operations with atomic private configuration persistence.
- Native Codex and Claude cost scanning, including fork/subagent lineage accounting and model/project breakdowns.
- OpenCode Go local usage, authoritative web reconciliation, workspace discovery fallback, and Zen balance support.

### Changed

- Replaced the inherited cross-platform repository with a focused Linux-only Meson project.
- Replaced application-bundle, website, and non-Linux release tooling with native local build, install, package, and
  verification flows.

### Removed

- All non-Linux sources and tests, widget projects, platform icons, appcast/signing files, generated site assets,
  migration notes, and stale development artifacts.
