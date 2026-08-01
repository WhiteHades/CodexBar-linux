# Development

## Dependencies

- Meson 1.6 or newer and Ninja
- GCC or Clang with C23 support
- GLib/GIO 2.78 or newer
- libcurl 8, json-c 0.17, SQLite 3, ncursesw 6.4, and pkg-config

## Inner loop

```sh
make build
make test
make check
```

`make check` uses `.build/check`, compiles with warnings as errors, runs all GLib and shell integration tests, checks
the diff, and rejects non-native or generated tracked artifacts.

For memory and undefined-behavior validation:

```sh
make sanitize
```

For a release-optimized build and install staging:

```sh
make release
make install DESTDIR="$PWD/.tmp/install-root"
```

## Provider changes

1. Compare the current upstream provider behavior at the commit in `UPSTREAM_REVISION`.
2. Add or update a focused transport/parser fixture in `linux/tests`.
3. Implement the smallest native change using the shared HTTP, process, model, and registry helpers.
4. Test explicit source modes, cancellation, response bounds, invalid credentials, malformed payloads, account scope,
   and renderer serialization as applicable.
5. Run `make check` and `make sanitize`.

Do not test with live credentials unless explicitly required. Deterministic transport functions and local fixtures are
the default.

## UI changes

TUI changes require the relevant automated test plus a fresh terminal run. Status-item changes require a D-Bus session
integration test. Verify the newly built binary, not an older installed copy.

## Upstream review

```sh
./Scripts/check-upstream.sh
git diff "$(cat UPSTREAM_REVISION)..upstream/main" -- Sources/CodexBarCore Sources/CodexBar
```

Port behavior and fixtures into native modules. Update `UPSTREAM_REVISION` only after the new upstream head has been
audited and all applicable native checks pass.
