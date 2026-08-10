# Packaging

CodexBar Linux has independent local packaging paths. None of them publish, tag, commit, or push. Distro packages and
the AppImage stage under `/usr`; the generic tar archive keeps its existing `/usr/local` layout.

## Common requirements

All package builds require the normal development dependencies from `docs/DEVELOPMENT.md`. Run the focused metadata
and script checks before building artifacts:

```sh
make check-packaging
make check
```

`make check-packaging` requires `appstreamcli`, `desktop-file-validate`, and `xmllint`. Package scripts write ignored
artifacts to `dist/` and temporary files to `.tmp/`. The two installed executables are always kept together:

```text
bin/codexbar-linux
bin/codexbar-process-supervisor
```

Moving only the main executable breaks bounded child-process supervision.

## Deterministic source archive

```sh
make package-source
```

This creates `dist/codexbar-linux-VERSION.tar.gz` and its `.sha256` file. The archive contains tracked and unignored
source files, sorted by name, with numeric root ownership, normalized modes and timestamps, and a gzip header without
host-specific metadata. `SOURCE_DATE_EPOCH` defaults to the current commit timestamp.

The command requires the matching version tag and refuses a dirty tree because a release archive must correspond to
reviewed source. For a local reproducibility check only, `CODEXBAR_ALLOW_DIRTY=1 make package-source` includes
uncommitted and untracked, unignored files from the tagged commit's working tree. Never publish that output.

## Generic tar archive

```sh
make package
```

The existing generic archive remains `codexbar-linux-VERSION-linux-ARCH.tar.gz`. It installs below `/usr/local` and
includes the system XDG autostart entry below `/etc/xdg/autostart`. This format is not a substitute for a distro package
manager.

## Debian package

```sh
make package-deb
```

On Debian or Ubuntu, the script builds locally and derives runtime dependencies with `dpkg-shlibdeps`. On other Linux
distributions it uses Docker and `debian:trixie-slim` by default. Override the container with
`CODEXBAR_DEBIAN_IMAGE`. The output is `codexbar-linux_VERSION-1_ARCH.deb` plus a checksum.

The package installs to `/usr`, treats `/etc/xdg/autostart/codexbar-status.desktop` as a conffile, includes compressed
release notes, and validates package metadata and contents with `dpkg-deb`.

## RPM package

```sh
make package-rpm
```

`packaging/rpm/codexbar-linux.spec` builds the deterministic source archive with Fedora dependencies and RPM automatic
runtime dependency generation. The script uses local `rpmbuild` when available. Otherwise, it falls back to Docker with
`fedora:42`; set `CODEXBAR_FEDORA_IMAGE` to choose another compatible Fedora image. Binary and source RPMs and their
checksums are copied to `dist/`.

Update the spec `Version`, AppStream release version, Meson project version, and changelog together. The RPM script
fails when the spec and Meson versions differ.

## AppImage

Install `linuxdeploy` and its `appimage` output plugin, then run:

```sh
make package-appimage
```

Set `LINUXDEPLOY` if the executable is not in `PATH`. The output is
`codexbar-linux-VERSION-ARCH.AppImage` plus a checksum. The script extracts the result and verifies that both
executables remain adjacent.

The AppImage intentionally omits `/etc/xdg/autostart/codexbar-status.desktop`: a read-only mounted `/etc` entry cannot
register host autostart. Users who want AppImage autostart must configure their desktop to execute the stable AppImage
file. When a desktop launch opens the TUI asynchronously, CodexBar uses the AppImage runtime's absolute `APPIMAGE` path
instead of the transient mount path.

## AUR

The AUR package currently supports `x86_64`. Add `aarch64` only after a native Arch Linux ARM package build passes.

`packaging/aur/PKGBUILD.in` is a tracked publication template, not a valid package recipe. It contains explicit version
and checksum tokens and never uses `SKIP`. First publish the deterministic source archive and checksum as assets on the
matching `vVERSION` GitHub release. Then prepare, inspect, and test the AUR files:

```sh
make package-aur AUR_VERSION=VERSION
cd dist/aur/VERSION
makepkg --verifysource
makepkg --cleanbuild
```

The helper downloads both release assets, verifies the checksum, replaces all tokens, rejects unresolved or `SKIP`
checksums, and generates `.SRCINFO`. It does not clone or push an AUR repository. Copy the reviewed `PKGBUILD` and
`.SRCINFO` into the AUR package repository and publish them manually only after the package build succeeds.
