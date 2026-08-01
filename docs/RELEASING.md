# Releasing

Releases are Linux C23 artifacts for `x86_64` and `aarch64`.

## Prepare

1. Fetch and audit upstream with `./Scripts/check-upstream.sh`.
2. Update `UPSTREAM_REVISION`, the Meson project version, and `CHANGELOG.md` together.
3. Run `make check` with GCC and Clang.
4. Run `make sanitize` with leak detection enabled.
5. Run `make release` and stage an install with a non-system `DESTDIR`.
6. Run `make package`; inspect the tar listing and validate its SHA-256 file.
7. Run the packaged binary’s `--version`, fixture-backed `usage`, and TUI/status-item smoke tests.

## Tag

Use a signed or annotated `vMAJOR.MINOR.PATCH` tag that exactly matches `meson.build`. Push the verified commit, then
the tag. Publishing a GitHub release triggers `.github/workflows/release.yml`, which builds both architectures,
re-runs tests, packages the Meson install tree, verifies architecture/version/checksum, and uploads tarballs plus
checksums.

Manual workflow runs build artifacts without publishing them.

## Artifact contract

Each archive is named `codexbar-linux-VERSION-linux-ARCH.tar.gz` and contains the Meson install tree:

- `usr/local/bin/codexbar-linux`
- `usr/local/bin/codexbar-process-supervisor`
- XDG application and autostart desktop entries

The binaries are dynamically linked to the documented system libraries. Release jobs use Ubuntu 24.04 runners to keep
the runtime baseline explicit.

## Post-release

Verify uploaded asset names, checksums, executable architecture, reported version, and release notes. Start the next
changelog section only when new user-facing work begins.
