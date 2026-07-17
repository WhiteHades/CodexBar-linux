# linux c23 migration

## where this is going

codexbar is becoming a native linux application written in c23 for arch linux, hyprland, and waybar. the final version
owns the provider engine, cli, config, saved data, waybar output, and terminal interface. it does not need swift at build
time or runtime.

the waybar behavior is simple.

1. hover shows a detailed terminal style tooltip.
2. left click launches or focuses the ncurses interface in a floating terminal.
3. right click refreshes the data.

## what is in the old codebase

the repository started with 1,903 tracked files.

1. `Sources/CodexBarCore` has 448 swift files for providers, parsing, networking, config, processes, and cost data.
2. `Sources/CodexBar` has 366 swift files for the appkit and swiftui application.
3. `Sources/CodexBarCLI` has 27 swift files for the existing cli.
4. `Tests/CodexBarTests` has 603 swift files and more than 6,000 test declarations.
5. `TestsLinux` has 24 swift files for the portable behavior that already works on linux.
6. there are 60 stable provider ids and 59 provider source directories.

the old linux cli is useful as a behavior reference while the migration is happening. it is not part of the final
version. the macos interface cannot be translated line by line because appkit, swiftui, widgetkit, keychain, webkit,
sparkle, service management, and apple notifications all need different linux behavior.

## the new shape

the new code has four clear parts.

1. the cli owns usage, cards, cost, sessions, serve, config, cache, and diagnose.
2. the core owns provider fallback, usage models, http, json, sqlite, xml, processes, ptys, time, config, cache, history,
   and secret service credentials.
3. the desktop part owns waybar json, the hover tooltip, the ncurses interface, notifications, and startup.
4. the tests own language neutral fixtures, comparisons with the old implementation, sanitizer runs, and linux
   integration checks.

the main dependencies are ncursesw, glib, gio, json c, libcurl, sqlite, libsecret, meson, and ninja. they are normal c
libraries and they are already available on the target arch system.

## behavior that cannot change by accident

1. provider ids, provider order, source modes, and fallback order stay compatible.
2. `~/.config/codexbar/config.json` stays compatible and writes atomically with mode `0600`.
3. managed accounts, history, dashboard data, and cost caches keep their readable formats.
4. cli commands, flags, json fields, output streams, and exit codes stay compatible where linux supports them.
5. account ownership checks keep one account or provider from showing another account or provider identity.
6. endpoint validation keeps credentials on the expected origin.
7. process groups and ptys always clean up correctly on cancel, timeout, and exit.
8. provider parser behavior is checked against the existing fixtures and tests.

## stage 0: freeze the behavior

1. keep the swift cli as a temporary reference.
2. move useful inline test cases into language neutral fixtures.
3. normalize timestamps, uuids, locale, terminal width, and temporary paths in comparison tests.

this stage is complete when the same deterministic case can run against both implementations.

## stage 1: make the linux terminal application real

1. build the new binary with strict c23 warnings.
2. parse the current cli json into small c models.
3. print waybar json with usage state, percentage, and a multiline tooltip.
4. launch or focus the ncurses interface in an omarchy floating terminal.
5. show providers, quota bars, resets, credits, and errors.
6. test parsing and waybar output with deterministic data.

this stage is complete when the build, tests, sanitizer checks, offscreen terminal capture, key handling, and waybar json
all pass.

## stage 2: own config and the first provider in c

1. implement xdg config lookup, normalization, validation, and atomic writes.
2. implement the provider registry and fallback interfaces.
3. port openrouter with libcurl and json c.
4. compare valid, unauthorized, malformed, timeout, and missing key behavior.

this stage is complete when openrouter does not call swift and its output matches the old behavior.

## stage 3: port direct api providers

1. port the providers that only need http and json.
2. share retry, timeout, redirect, tls, endpoint, date, and crypto code.
3. preserve each provider fixture before enabling it by default.

this stage is complete when every provider in this group passes its behavior and security tests.

## stage 4: port local and process providers

1. port sqlite, xml, jsonl, local file, process, and pty sources.
2. port cost scans, sessions, and persistent cli helpers.
3. test process cleanup, bounded output, cancellation, and timeouts.

this stage is complete when every behavior currently covered by `TestsLinux` has a c test and sanitizer coverage.

## stage 5: port login and accounts

1. use secret service for credentials.
2. port oauth, device flows, refresh limits, account ownership, and managed codex accounts.
3. add chromium and firefox profile readers where linux can support them safely.
4. only use webkitgtk for a provider when there is no stable api, cli, or local source.

this stage is complete when login and account switching need no swift code and identity never leaks across providers.

## stage 6: finish the desktop behavior

1. finish provider settings, ordering, display modes, status checks, notifications, history, and charts.
2. add the waybar wiring, desktop metadata, and systemd user startup.
3. verify themes, terminal sizes, scaling, and multiple monitors without interrupting the active desktop session.
4. keep catppuccin mocha as the default theme and include dark and system terminal themes.

this stage is complete when the c terminal interface and cli cover the full linux feature list.

## stage 7: remove the old code

1. run the complete comparison suite one final time.
2. remove swift sources, swift package files, macos packaging, widgetkit, apple signing, and apple release artifacts.
3. replace ci and releases with small c23 linux builds and packages.

this stage is complete when a clean checkout builds, tests, installs, starts from waybar, and uses configured providers
without a swift toolchain or swift binary.

## what must work before the old code is removed

1. waybar needs icon and text modes, highest usage selection, state classes, the tooltip, and signal refresh.
2. the terminal interface needs provider switching, all quota windows, resets, credits, status, errors, refresh,
   settings, and quit.
3. settings need provider enablement, order, source, credentials, accounts, refresh, display, notifications, and privacy.
4. the cli needs usage, cards, cost, sessions, serve, config, cache, diagnose, compatible json, and compatible exit codes.
5. all 60 provider ids need working linux behavior or a clear unavailable state when the provider itself has no linux
   source.
6. account support needs token accounts, managed codex accounts, switching, ownership checks, and credential refresh.
7. desktop support needs waybar, notifications, startup, terminal themes, scaling, and multiple monitors.
8. saved data needs config, history, caches, snapshots, migrations, and restrictive permissions.
9. quality needs fixture comparisons, integration tests, address sanitizer, undefined behavior sanitizer, no saved secrets,
   and offscreen visual checks.

## removal rule

swift code is not removed just because similar c code exists. each old subsystem stays until the c replacement passes
its fixture tests, comparison tests, and linux integration checks. after that it is removed instead of kept as a second
implementation.
