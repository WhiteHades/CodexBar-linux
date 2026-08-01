# Repository guidelines

CodexBar Linux is a C23-only Linux application. Keep it native and do not add non-Linux application or release assets.

## Layout

- `linux/src`: application, providers, CLI, TUI, status notifier, runtime, history, and integrations.
- `linux/tests`: GLib and shell integration tests.
- `linux/data`: XDG desktop and autostart entries.
- `fixtures/native`: deterministic native fixtures.
- `Scripts`: build, packaging, tree-verification, and upstream-audit helpers.
- `docs`: maintained Linux documentation only.

## Work rules

1. Read the files being changed and their callers first.
2. Keep changes scoped and use existing C/GLib patterns.
3. C must compile as C23 with warnings as errors under GCC and Clang.
4. Do not expose credentials, cookie headers, account tokens, or absolute home paths.
5. Provider data must remain account- and provider-scoped.
6. Network endpoints must use the central HTTP policy and bounded responses.
7. Child processes must be bounded, cancellable, and fully reaped.
8. Add focused tests for behavior changes; never weaken tests to make a change pass.
9. Run `make check` after code or build changes and `make sanitize` before release.
10. For TUI/status-item changes, run the relevant integration test and visually inspect the fresh binary.

## Commands

```sh
make build
make test
make check
make sanitize
make release
make package
make install DESTDIR="$PWD/.tmp/install-root"
```

`make check` builds, runs all tests, validates formatting invariants, and rejects non-native or generated artifacts.

## Upstream parity

The source project is `steipete/CodexBar`. `UPSTREAM_REVISION` records the audited upstream commit. Use
`./Scripts/check-upstream.sh` to fetch `upstream/main` and report changes. Port behavior into the existing native model;
do not copy upstream implementation files into this repository.

## Commits

Use short lowercase conventional messages such as `feat: add provider usage` or `fix: preserve account scope`.
Commit and push each verified phase separately. Stage only files belonging to that phase.
