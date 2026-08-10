<div align="center">

# codexbar linux

see your ai usage from the terminal and desktop tray

[![release](https://img.shields.io/github/v/release/WhiteHades/CodexBar-linux?style=flat-square)](https://github.com/WhiteHades/CodexBar-linux/releases)
[![linux](https://img.shields.io/badge/platform-linux-1793d1?style=flat-square)](https://github.com/WhiteHades/CodexBar-linux)
[![c23](https://img.shields.io/badge/code-c23-00599c?style=flat-square)](https://github.com/WhiteHades/CodexBar-linux)
[![license](https://img.shields.io/github/license/WhiteHades/CodexBar-linux?style=flat-square)](LICENSE)

native • private • open source • 67 providers

</div>

codexbar linux shows how much ai usage you have left. it can live in your kde plasma tray, waybar tray, or another linux desktop tray. it also has a terminal app, simple commands, json output, history, alerts, cost reports, and a local dashboard api.

codexbar linux is a native linux fork of [steipete/codexbar](https://github.com/steipete/CodexBar), which defines the upstream provider behavior this port follows.

## contents

1. [features](#features)
2. [install](#install)
3. [tray](#tray)
4. [setup](#setup)
5. [use](#use)
6. [providers](#providers)
7. [privacy](#privacy)
8. [help](#help)
9. [develop](#develop)
10. [license](#license)

## features

1. one small native linux app
2. kde plasma and status notifier tray support
3. waybar support through its normal tray module
4. terminal ui with keyboard controls
5. text and json commands for scripts
6. usage history, alerts, hooks, costs, and sessions
7. multiple accounts with separate data
8. bounded network requests and private config files
9. 67 native providers with no compatibility subprocess
10. x86_64 and aarch64 release support

## install

### release packages

release pages provide a debian package, rpm, appimage, generic archive, and deterministic source archive for the
published version. verify the adjacent `.sha256` file before installation.

install a downloaded debian package:

```sh
sudo apt install ./codexbar-linux_VERSION-1_ARCH.deb
```

install a downloaded rpm:

```sh
sudo dnf install ./codexbar-linux-VERSION-1.DISTRO.ARCH.rpm
```

run an appimage without installing system files:

```sh
chmod +x codexbar-linux-VERSION-ARCH.AppImage
./codexbar-linux-VERSION-ARCH.AppImage
```

the appimage does not configure desktop autostart. configure your desktop to run the stable appimage path if you want
the status item at login.

install from the aur on arch linux or manjaro:

```sh
yay -S codexbar-linux
```

### build from source

#### ubuntu and debian

```sh
sudo apt update
sudo apt install build-essential meson ninja-build pkg-config libglib2.0-dev libcurl4-openssl-dev libjson-c-dev libsqlite3-dev libncurses-dev
```

#### fedora

```sh
sudo dnf install gcc meson ninja-build pkgconf-pkg-config glib2-devel libcurl-devel json-c-devel sqlite-devel ncurses-devel
```

#### arch linux and manjaro

```sh
sudo pacman -S --needed base-devel meson ninja pkgconf glib2 curl json-c sqlite ncurses
```

#### opensuse

```sh
sudo zypper install gcc meson ninja pkg-config glib2-devel libcurl-devel libjson-c-devel sqlite3-devel ncurses-devel
```

#### build and install

```sh
git clone https://github.com/WhiteHades/CodexBar-linux.git
cd CodexBar-linux
make check
sudo make install
```

the app is now installed as `codexbar-linux`. it starts in your desktop tray on your next login.

run it now without logging out:

```sh
codexbar-linux status-item
```

the generic ready-made archive remains available for systems without one of the package formats above.

## tray

codexbar linux uses the standard linux status notifier protocol. this is the same tray system used by many chat, sync, and media apps.

### kde plasma

the standard plasma system tray finds codexbar automatically. install the app, then log out and back in. you can also run `codexbar-linux status-item` to show it now.

if it is hidden, open the plasma system tray settings and set `codexbar` to show when relevant or always show.

### waybar

if your waybar already has a tray module, codexbar appears automatically.

if it does not, add `tray` to `modules-right` in your waybar config and add this section:

```json
"tray": {
  "icon-size": 18,
  "spacing": 8
}
```

reload waybar after the change.

### gnome, xfce, cinnamon, and mate

codexbar works with panels that support status notifier items. ubuntu usually includes this support. on gnome, install or enable the appindicator extension if your tray is not visible.

codexbar never rewrites your panel layout. it joins the tray that your desktop already owns.

## setup

see every provider:

```sh
codexbar-linux config providers
```

enable a provider:

```sh
codexbar-linux config enable --provider openrouter
```

add an api key without placing it in shell history:

```sh
printf '%s\n' "$OPENROUTER_API_KEY" | codexbar-linux config set-api-key --provider openrouter --stdin
```

check your setup:

```sh
codexbar-linux config validate
codexbar-linux diagnose --provider openrouter --format json
```

the config file lives at `${XDG_CONFIG_HOME:-~/.config}/codexbar/config.json` and uses private file permissions.

read the full [configuration guide](docs/CONFIGURATION.md) for accounts, sources, themes, and environment options.

## use

open the terminal app:

```sh
codexbar-linux tui
```

show usage:

```sh
codexbar-linux usage --provider codex
codexbar-linux usage --provider all --format json --pretty
```

use the other tools:

```sh
codexbar-linux cards --provider all --brief
codexbar-linux cost --provider both --format json
codexbar-linux history --provider codex --format json
codexbar-linux sessions --json
codexbar-linux guard --provider codex --window weekly --min-remaining 20
codexbar-linux waybar
```

run `codexbar-linux --help` to see every command.

## providers

codexbar linux includes 67 native providers. this includes codex, claude, copilot, cursor, fireworks, gemini, openrouter, deepseek, deepgram, groq, kimi, kilo, z.ai, vertex ai, bedrock, xai, and many more.

read the complete [provider list](docs/PROVIDERS.md).

## privacy

your provider data stays separated by provider and account. config writes are atomic and private. network responses have size limits. redirects and custom endpoints are checked. secrets are redacted from diagnostics.

read the [security guide](docs/SECURITY.md) for the full threat model.

## help

if the tray icon does not appear:

1. run `codexbar-linux status-item` in a terminal
2. check that your panel has a system tray or tray module
3. check whether your desktop has hidden the `codexbar` item
4. run `codexbar-linux diagnose --provider all --format json`

if a provider does not work, run `codexbar-linux config validate` first. do not post api keys, cookies, or account tokens in an issue.

## develop

```sh
make build
make test
make check
make sanitize
make release
make package
make package-source
make package-deb
make package-rpm
make package-appimage
make check-packaging
```

read the [development guide](docs/DEVELOPMENT.md), [packaging guide](docs/PACKAGING.md), and
[release guide](docs/RELEASING.md).

## license

mit. see [license](LICENSE).
