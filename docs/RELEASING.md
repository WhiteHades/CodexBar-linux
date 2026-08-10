# Releasing

Releases are Linux C23 artifacts for `x86_64` and `aarch64`. Packaging and publication remain local maintainer actions;
no CI workflow is required or provided.

## Prepare

1. Fetch and audit upstream with `./Scripts/check-upstream.sh`.
2. Update `UPSTREAM_REVISION`, `CHANGELOG.md`, the Meson project version, the RPM spec version and changelog, and the
   AppStream release version and date together.
3. Run `make check-packaging`, then run `make check` with GCC and Clang locally.
4. Run `make sanitize` with leak detection enabled.
5. Run `make release` and `make install DESTDIR="$PWD/.tmp/install-root"`. Confirm the `/usr/local` generic layout.
6. Create the matching annotated tag on the clean release commit, then run `make package-source`. Run it twice with the
   same `SOURCE_DATE_EPOCH` and compare the
   archive SHA-256 values.
7. Run `make package` in native or emulated x86-64 and aarch64 environments. This preserves the generic archive.
8. Run `make package-deb`, `make package-rpm`, and `make package-appimage` on each supported architecture where that
   format is published. Container fallbacks still require a working Docker daemon and network access to distro mirrors.
9. Inspect package metadata and file lists. Confirm distro artifacts use `/usr`, the supervisor is adjacent to the main
   executable, and only the AppImage omits `/etc/xdg/autostart`.
10. Run each packaged binary's `--version`, fixture-backed `usage`, TUI launch, and status-item smoke tests. Visually
    inspect the installed desktop icon and a fresh tray instance.

Do not use `CODEXBAR_ALLOW_DIRTY=1` for release artifacts. Record the build distribution, image digest when a container
is used, architecture, compiler, libc baseline, and every artifact checksum.

If `vVERSION` already identifies an older commit, source packaging fails until all version metadata advances. Never
replace or move the existing release tag to bypass this check.

## Tag and upload

Use a signed or annotated `vMAJOR.MINOR.PATCH` tag that exactly matches all package metadata. Push the verified commit
and tag, then create the GitHub release. Upload the deterministic source archive and checksum first, followed by the
verified generic archives, Debian packages, RPMs, AppImages, and checksum files that the release supports.

Do not claim an architecture or format that was not built and tested. Package scripts create local files only; they do
not tag, upload, publish, commit, or push.

## Artifact contracts

The generic archive is named `codexbar-linux-VERSION-linux-ARCH.tar.gz` and retains this layout:

- `usr/local/bin/codexbar-linux`
- `usr/local/bin/codexbar-process-supervisor`
- `usr/local/share/applications/com.steipete.codexbar.desktop`
- `usr/local/share/icons/hicolor/scalable/apps/com.steipete.codexbar.svg`
- `usr/local/share/metainfo/com.steipete.codexbar.metainfo.xml`
- `etc/xdg/autostart/codexbar-status.desktop`

Debian, RPM, and Arch packages install the equivalent files below `/usr` and keep the host autostart entry below
`/etc`. The AppImage contains the `/usr` application tree but no host autostart entry. All packaged binaries are
stripped and dynamically linked to the documented runtime libraries. Archive ownership is numeric root metadata, and
checksum files contain artifact basenames.

## AUR publication

After the source archive and checksum are downloadable from the GitHub release, run:

```sh
make package-aur AUR_VERSION=MAJOR.MINOR.PATCH
```

The command must fail before those release-only assets exist. Inspect `dist/aur/VERSION/PKGBUILD` and `.SRCINFO`, run
`makepkg --verifysource` and `makepkg --cleanbuild`, review the package contents, and only then copy both files into the
AUR repository. The helper does not publish them.

## Post-release

Verify uploaded names, checksums, executable architecture, reported version, package-manager metadata, AppStream data,
and release notes from a clean machine. Start the next changelog section only when new user-facing work begins.
