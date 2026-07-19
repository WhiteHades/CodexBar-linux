#!/bin/sh

set -eu

binary=$1
base=$PWD/.tmp/claude-cli
mkdir -p "$base"
work=$(mktemp -d "$base/run.XXXXXX")
trap 'rm -rf "$work"; rmdir "$base" 2>/dev/null || true' EXIT
export HOME="$work/home"
export XDG_CONFIG_HOME="$work/config"
export CLAUDE_CONFIG_DIR="$work/claude"
mkdir -p "$HOME" "$XDG_CONFIG_HOME" "$CLAUDE_CONFIG_DIR"

if output=$("$binary" usage --provider claude --source oauth --format json); then
    printf 'claude usage succeeded without credentials\n' >&2
    exit 1
fi
printf '%s\n' "$output" | grep -q '"provider":"claude"'
printf '%s\n' "$output" | grep -q '"source":"oauth"'
printf '%s\n' "$output" | grep -q 'Run `claude login`'

cat >"$CLAUDE_CONFIG_DIR/.credentials.json" <<'EOF'
{"claudeAiOauth":{"accessToken":"expired-token","expiresAt":1,"rateLimitTier":"claude_pro"}}
EOF
if output=$("$binary" usage --provider claude --source oauth --format json); then
    printf 'claude usage succeeded with an expired token\n' >&2
    exit 1
fi
printf '%s\n' "$output" | grep -q 'OAuth token expired'
if printf '%s\n' "$output" | grep -q 'expired-token'; then
    printf 'claude usage leaked access token\n' >&2
    exit 1
fi
