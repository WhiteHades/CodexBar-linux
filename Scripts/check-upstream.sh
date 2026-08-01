#!/bin/sh

set -eu

repo=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
cd "$repo"
remote=${CODEXBAR_UPSTREAM_REMOTE:-upstream}
url=${CODEXBAR_UPSTREAM_URL:-https://github.com/steipete/CodexBar.git}

if ! git remote get-url "$remote" >/dev/null 2>&1; then
    git remote add "$remote" "$url"
fi
git fetch --quiet "$remote" main

audited=$(tr -d '[:space:]' <UPSTREAM_REVISION)
current=$(git rev-parse "$remote/main")
printf 'audited: %s\ncurrent: %s\n' "$audited" "$current"
if test "$audited" = "$current"; then
    printf '%s\n' 'upstream main is fully audited'
    exit 0
fi

printf '%s\n' 'new upstream commits require review:'
git log --oneline --no-merges "$audited..$current"
exit 1
