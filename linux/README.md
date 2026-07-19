# codexbar linux c23

this directory is the native linux version of codexbar. it uses c23, ncursesw, glib, gio, and json c. it does not use
any graphical application toolkit. everything the user opens is a real terminal interface.

## build it

```bash
meson setup .build/c23
meson compile -C .build/c23
meson test -C .build/c23 --print-errorlogs
```

the binary is at `.build/c23/linux/codexbar-linux`.

## use it

```bash
codexbar-linux waybar
codexbar-linux tui
codexbar-linux usage --provider openrouter --format json --pretty
codexbar-linux config validate
codexbar-linux config providers
codexbar-linux --version
```

`waybar` prints one line of json for the waybar module and its hover tooltip.

`tui` opens the interactive terminal interface. when a panel or desktop launcher starts it without a terminal,
codexbar opens the system terminal automatically.

invoking `codexbar-linux` without a command defaults to `usage`. provider selection accepts stable IDs, CLI names, and
aliases; `--provider both` selects Codex and Claude, while explicit `--provider all` selects the full registry.

the native engine currently owns Codex, Codebuff API-key usage, JetBrains local usage, Kimi API-key usage, Kimi K2,
OpenRouter, ClawRouter, LLM Proxy, DeepSeek, Moonshot, ElevenLabs, Crof, Venice, and ZenMux. configure
providers in `~/.config/codexbar/config.json`. api keys can come from each provider's standard environment variable or
the provider `apiKey` field.

the native config commands normalize all 60 stable providers while preserving provider-specific fields that are not
native yet. writes are atomic, reject concurrent changes, and keep the config file at mode `0600`.

```bash
codexbar-linux config dump --pretty
codexbar-linux config enable --provider openrouter
codexbar-linux config disable --provider codex
printf '%s\n' "$OPENROUTER_API_KEY" | codexbar-linux config set-api-key --provider openrouter --stdin
```

`CODEXBAR_BACKEND` remains available for deterministic fixtures and migration comparisons. normal use does not need a
swift cli.

## themes

the default theme is catppuccin mocha. set `CODEXBAR_THEME` before opening the tui when you want another theme.

```bash
CODEXBAR_THEME=mocha codexbar-linux tui
CODEXBAR_THEME=dark codexbar-linux tui
CODEXBAR_THEME=system codexbar-linux tui
```

`mocha` uses the catppuccin mocha base, text, subtext, blue, green, and red roles.

`dark` uses a standard black terminal background with neutral text and restrained accents.

`system` keeps the terminal foreground and background and only adds semantic colors for borders, usage, and errors.

## tui keys

1. use `h`, `Left`, `[`, or `Shift Tab` for the previous provider.
2. use `l`, `Right`, `]`, or `Tab` for the next provider.
3. use `g` and `G` for the first and last provider.
4. use `1` through `9` to select a provider by its position.
5. use `Enter` to open provider actions for the usage dashboard, status page, config, About, and Quit.
6. use `j`, `k`, and `Enter` to choose and run an action; use `Esc` to return without losing your position.
7. use `r` or `Ctrl R` to refresh.
8. use `?` to show the key help.
9. use `q`, `Esc`, or `ZZ` to close from the usage view.
