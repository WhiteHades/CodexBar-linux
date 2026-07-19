#!/bin/sh

set -eu

binary=$1
base=$PWD/.tmp/cache-cli
mkdir -p "$base"
work=$(mktemp -d "$base/run.XXXXXX")
trap 'rm -rf "$work"; rmdir "$base" 2>/dev/null || true' EXIT
export HOME="$work/home"
export XDG_CACHE_HOME="$work/cache"
export XDG_DATA_HOME="$work/data"
mkdir -p "$XDG_CACHE_HOME/CodexBar/cost-usage/nested" "$XDG_DATA_HOME/CodexBar"
printf 'cost\n' >"$XDG_CACHE_HOME/CodexBar/cost-usage/nested/index"
printf '{}\n' >"$XDG_DATA_HOME/CodexBar/claude-cookie.json"
printf '{}\n' >"$XDG_DATA_HOME/CodexBar/codex-cookie.json"
printf '{}\n' >"$XDG_DATA_HOME/CodexBar/unknown-cookie.json"
printf 'keep\n' >"$XDG_DATA_HOME/CodexBar/settings.json"

output=$("$binary" cache clear --cookies --provider CLAUDE)
[ "$output" = 'cookies: cleared (claude)' ]
[ ! -e "$XDG_DATA_HOME/CodexBar/claude-cookie.json" ]
[ -f "$XDG_DATA_HOME/CodexBar/codex-cookie.json" ]

output=$("$binary" cache clear --cookies --json)
[ "$output" = '[{"cache":"cookies","cleared":2}]' ]
[ ! -e "$XDG_DATA_HOME/CodexBar/codex-cookie.json" ]
[ ! -e "$XDG_DATA_HOME/CodexBar/unknown-cookie.json" ]
[ -f "$XDG_DATA_HOME/CodexBar/settings.json" ]

output=$("$binary" cache clear --cost)
[ "$output" = 'cost: cleared (all providers)' ]
[ ! -e "$XDG_CACHE_HOME/CodexBar/cost-usage" ]

output=$("$binary" cache clear --all --format json --pretty)
printf '%s\n' "$output" | grep -Eq '"cache" *: *"cookies"'
printf '%s\n' "$output" | grep -Eq '"cache" *: *"cost"'

if "$binary" cache clear >"$work/output" 2>"$work/error"; then
    printf 'empty cache selection unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: Specify --cookies, --cost, or --all.' ]

if "$binary" cache clear --cost --provider claude >"$work/output" 2>"$work/error"; then
    printf 'cost provider scope unexpectedly succeeded\n' >&2
    exit 1
fi
grep -q '^Error: --provider only scopes cookie caches\.' "$work/error"

if "$binary" cache clear --cookies --provider missing >"$work/output" 2>"$work/error"; then
    printf 'unknown provider unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: Unknown provider: missing' ]
