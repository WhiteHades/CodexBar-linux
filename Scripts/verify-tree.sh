#!/bin/sh

set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo"

for forbidden in '*.swift' '*.xcodeproj/*' '*.xcworkspace/*' 'Package.swift' 'Package.resolved' \
    '*.icns' 'appcast.xml' '.mac-release.env'; do
    if git ls-files -- "$forbidden" | grep -q .; then
        printf 'forbidden tracked artifact matching %s:\n' "$forbidden" >&2
        git ls-files -- "$forbidden" >&2
        exit 1
    fi
done

if git ls-files | grep -E '(^|/)(build|build-[^/]*|\.build|\.tmp)/' >/dev/null; then
    printf '%s\n' 'generated build output is tracked' >&2
    exit 1
fi

if git grep -n -I -E 'CodexBar\.app|swift build|swift test|SwiftPM|Xcode|Sparkle|notari[sz]' -- \
    .github Makefile meson.build linux >/dev/null; then
    printf '%s\n' 'obsolete non-Linux build or release references remain' >&2
    git grep -n -I -E 'CodexBar\.app|swift build|swift test|SwiftPM|Xcode|Sparkle|notari[sz]' -- \
        .github Makefile meson.build linux >&2
    exit 1
fi

printf '%s\n' 'repository tree is C23-only and free of tracked build artifacts'
