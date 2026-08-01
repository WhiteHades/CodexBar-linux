# CodexBar Linux

CodexBar Linux is a native C23 usage monitor for AI coding services. It provides a terminal UI, scriptable JSON/text
CLI, Waybar output, a StatusNotifierItem tray process, usage history, cost estimates, account management, hooks,
notifications, session discovery, and a local dashboard API.

This repository is Linux-only. It tracks CodexBar upstream provider behavior and data contracts while implementing
native equivalents with GLib/GIO, libcurl, json-c, SQLite, and ncursesw.

## Build

Requirements: Meson 1.6+, Ninja, a C23 compiler, pkg-config, GLib/GIO 2.78+, libcurl 8+, json-c 0.17+, SQLite 3,
and ncursesw 6.4+.

On Debian or Ubuntu:

```sh
sudo apt-get install build-essential meson ninja-build pkg-config \
  libglib2.0-dev libcurl4-openssl-dev libjson-c-dev libsqlite3-dev libncurses-dev
make check
```

The binary is `.build/debug/linux/codexbar-linux`.

## Use

```sh
codexbar-linux usage --provider codex
codexbar-linux usage --provider claude --all-accounts --format json --pretty
codexbar-linux cards --provider all --brief
codexbar-linux cost --provider both --format json
codexbar-linux history --provider codex --format json
codexbar-linux sessions --json
codexbar-linux guard --provider codex --window weekly --min-remaining 20
codexbar-linux waybar
codexbar-linux tui
codexbar-linux status-item
```

Run `codexbar-linux --help` for the complete command list. The TUI supports provider switching, refresh, actions,
and keyboard help. The status item opens the TUI and is installed with an XDG autostart entry.

## Configure

Configuration defaults to `${XDG_CONFIG_HOME:-~/.config}/codexbar/config.json`, is written atomically with mode `0600`,
and can be managed without exposing secrets:

```sh
codexbar-linux config providers
codexbar-linux config enable --provider openrouter
printf '%s\n' "$OPENROUTER_API_KEY" | \
  codexbar-linux config set-api-key --provider openrouter --stdin
codexbar-linux config validate
```

See [configuration](docs/CONFIGURATION.md), [providers](docs/PROVIDERS.md), and
[development](docs/DEVELOPMENT.md).

## Install and package

```sh
make release
sudo meson install -C .build/release
make package
```

Packaging creates a versioned architecture tarball and SHA-256 file under `dist/`. Release procedure and artifact
validation are documented in [releasing](docs/RELEASING.md).

## Security

Provider responses are size-bounded, redirects are restricted, configuration is private, and dashboard bearer tokens
are required off loopback. See [security](docs/SECURITY.md) for the threat model and reporting guidance.

## License

MIT. See [LICENSE](LICENSE).
