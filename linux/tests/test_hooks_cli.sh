#!/bin/sh
set -eu

binary=$1
work=$(mktemp -d "$PWD/codexbar-hooks-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM
config=$work/config.json

output=$(CODEXBAR_CONFIG="$config" "$binary" hooks list)
[ "$output" = "Hooks: disabled
No rules configured." ]

output=$(CODEXBAR_CONFIG="$config" "$binary" hooks enable --json)
case "$output" in
  *'"enabled":true'*'"events":[]'*) ;;
  *) printf 'unexpected hooks enable output: %s\n' "$output" >&2; exit 1 ;;
esac
[ "$(stat -c %a "$config")" = 600 ]

cat >"$config" <<'EOF'
{
  "version": 1,
  "marker": "preserved",
  "providers": [],
  "hooks": {
    "enabled": true,
    "events": [
      {
        "id": "cat-rule",
        "event": "quota_low",
        "provider": "codex",
        "threshold": 1,
        "executable": "/bin/cat",
        "arguments": [],
        "timeoutSeconds": 2
      }
    ]
  }
}
EOF

if CODEXBAR_CONFIG="$config" "$binary" hooks test quota_low --provider openai-api --json >/dev/null 2>&1; then
    printf 'provider-scoped hook unexpectedly matched another provider\n' >&2
    exit 1
fi

output=$(CODEXBAR_CONFIG="$config" "$binary" hooks test quota_low --provider codex --json)
case "$output" in
  *'"ruleID":"cat-rule"'*'"success":true'*'\"event\":\"quota_low\"'*'\"account\":\"test@example.com\"'*'\"usagePercent\":1.0'*'\"window\":\"session\"'*'\"used\":1.0'*'\"limit\":1.0'*'\"resetAt\"'*) ;;
  *) printf 'unexpected hook test output: %s\n' "$output" >&2; exit 1 ;;
esac

CODEXBAR_CONFIG="$config" "$binary" hooks disable >/dev/null
grep -q '"marker"' "$config"
if CODEXBAR_CONFIG="$config" "$binary" hooks test quota_low --provider codex >/dev/null 2>&1; then
    printf 'disabled hooks unexpectedly ran\n' >&2
    exit 1
fi

cat >"$config" <<'EOF'
{
  "version": 1,
  "providers": [],
  "hooks": {
    "enabled": true,
    "events": [
      {
        "id": "env-rule",
        "event": "refresh_failed",
        "executable": "/usr/bin/env"
      }
    ]
  }
}
EOF
output=$(OPENAI_API_KEY=private-marker CODEXBAR_CONFIG="$config" \
  "$binary" hooks test refresh_failed --provider codex --json)
case "$output" in
  *private-marker*) printf 'hook inherited provider secret\n' >&2; exit 1 ;;
  *'CODEXBAR_EVENT=refresh_failed'*'CODEXBAR_PROVIDER=codex'*'CODEXBAR_ACCOUNT=test@example.com'*) ;;
  *) printf 'hook event environment missing: %s\n' "$output" >&2; exit 1 ;;
esac

cat >"$config" <<'EOF'
{
  "version": 1,
  "providers": [],
  "hooks": {
    "enabled": true,
    "events": [
      {
        "id": "failure-rule",
        "event": "provider_unavailable",
        "executable": "/bin/false"
      }
    ]
  }
}
EOF
if CODEXBAR_CONFIG="$config" "$binary" hooks test provider_unavailable --provider codex --json >"$work/out"; then
    printf 'failing hook unexpectedly succeeded\n' >&2
    exit 1
fi
output=$(sed -n '1p' "$work/out")
case "$output" in
  *'"success":false'*'"error":"exit 1"'*) ;;
  *) printf 'unexpected failing hook output: %s\n' "$output" >&2; exit 1 ;;
esac
