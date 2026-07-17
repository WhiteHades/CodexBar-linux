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
```

`waybar` prints one line of json for the waybar module and its hover tooltip.

`tui` opens the interactive terminal interface.

the current migration stage reads json from the existing `codexbar` cli. set `CODEXBAR_BACKEND` when that binary is
in a different location. this temporary backend goes away as each provider moves into the c engine.

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
5. use `r` or `Ctrl R` to refresh.
6. use `?` to show the key help.
7. use `q`, `Esc`, or `ZZ` to close.
