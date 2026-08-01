# Releasing

Releases are Linux C23 artifacts for `x86_64` and `aarch64`.

## Prepare

1. Fetch and audit upstream with `./Scripts/check-upstream.sh`.
2. Update `UPSTREAM_REVISION`, the Meson project version, and `CHANGELOG.md` together.
3. Run `make check` with GCC and Clang locally.
4. Run `make sanitize` with leak detection enabled.
5. Run `make release` and stage an install with a non-system `DESTDIR`.
6. Run `make package` in native or emulated x86-64 and aarch64 Linux environments; inspect each tar listing and
   validate its SHA-256 file.
7. Run the packaged binary’s `--version`, fixture-backed `usage`, and TUI/status-item smoke tests.

## Tag

Use a signed or annotated `vMAJOR.MINOR.PATCH` tag that exactly matches `meson.build`. Push the verified commit and tag,
then create the GitHub release with the four locally verified x86-64/aarch64 archives and checksum files. GitHub
Actions are not required for building, testing, packaging, or publishing a release.

## Artifact contract

Each archive is named `codexbar-linux-VERSION-linux-ARCH.tar.gz` and contains the Meson install tree:

- `usr/local/bin/codexbar-linux`
- `usr/local/bin/codexbar-process-supervisor`
- `usr/local/share/applications/com.steipete.codexbar.desktop`
- `etc/xdg/autostart/codexbar-status.desktop`

The packaged binaries are stripped and dynamically linked to the documented system libraries. Archive ownership is
normalized to numeric root metadata so artifacts do not expose the build account, and packaged directories use mode
`0755`. Checksum files contain archive basenames, so each archive and its checksum can be downloaded and verified
together from any directory. Release jobs use Ubuntu 24.04 runners to keep the runtime baseline explicit.

## Post-release

Verify uploaded asset names, checksums, executable architecture, reported version, and release notes. Start the next
changelog section only when new user-facing work begins.
