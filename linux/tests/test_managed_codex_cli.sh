#!/bin/sh
set -eu

binary=$1
mkdir -p "$PWD/.tmp"
work=$(mktemp -d "$PWD/.tmp/codexbar-managed-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT
store="$work/managed.json"
homes="$work/homes"
auth="$work/auth.json"

header=$(printf '%s' '{"alg":"none"}' | base64 -w0 | tr '+/' '-_' | tr -d '=')
payload=$(printf '%s' '{"email":"cli@example.com","https://api.openai.com/auth":{"chatgpt_account_id":"workspace-cli"}}' |
  base64 -w0 | tr '+/' '-_' | tr -d '=')
printf '{"tokens":{"access_token":"access","refresh_token":"refresh","id_token":"%s.%s."}}\n' \
  "$header" "$payload" >"$auth"

export CODEXBAR_MANAGED_CODEX_STORE="$store"
export CODEXBAR_MANAGED_CODEX_ROOT="$homes"
output=$($binary codex-accounts import "$auth")
case "$output" in
  'Imported managed Codex account cli@example.com ('*')'.) ;;
  *) printf 'unexpected import output: %s\n' "$output" >&2; exit 1 ;;
esac

output=$($binary codex-accounts list --json)
case "$output" in
  *'"email":"cli@example.com"'*'"providerAccountID":"workspace-cli"'*) ;;
  *) printf 'unexpected account list: %s\n' "$output" >&2; exit 1 ;;
esac

home=$($binary codex-accounts exec cli@example.com -- sh -c 'printf %s "$CODEX_HOME"')
case "$home" in
  "$homes"/*) ;;
  *) printf 'managed execution used wrong CODEX_HOME: %s\n' "$home" >&2; exit 1 ;;
esac
[ -f "$home/auth.json" ]
[ "$(stat -c %a "$home/auth.json")" = 600 ]

fake="$work/fake-codex"
printf '%s\n' '#!/bin/sh' 'cp "$CODEXBAR_TEST_AUTH" "$CODEX_HOME/auth.json"' >"$fake"
chmod +x "$fake"
export CODEX_CLI_PATH="$fake"
export CODEXBAR_TEST_AUTH="$auth"
output=$($binary codex-accounts login --timeout 5)
case "$output" in
  'Authenticated managed Codex account cli@example.com ('*')'.) ;;
  *) printf 'unexpected login output: %s\n' "$output" >&2; exit 1 ;;
esac
new_home=$($binary codex-accounts exec cli@example.com -- sh -c 'printf %s "$CODEX_HOME"')
[ "$new_home" != "$home" ]
[ ! -e "$home" ]
home=$new_home

printf '%s\n' \
  '#!/bin/sh' \
  'while IFS= read -r line; do' \
  '  case "$line" in' \
  '    *'"'"'"id":1'"'"'*) printf '\''{"id":1,"result":{}}\n'\'' ;;' \
  '    *'"'"'"id":2'"'"'*) printf '\''{"id":2,"result":{"rateLimits":{"planType":"pro","primary":{"usedPercent":10}}}}\n'\'' ;;' \
  '    *'"'"'"id":3'"'"'*) printf '\''{"id":3,"result":{"account":{"type":"chatgpt","email":"ambient@example.com","planType":"pro"}}}\n'\'' ;;' \
  '  esac' \
  'done' >"$fake"
chmod +x "$fake"
export CODEXBAR_CONFIG="$work/config.json"
output=$($binary usage --format json)
[ "$(printf '%s' "$output" | grep -o '"provider":"codex"' | wc -l)" -eq 2 ]
case "$output" in
  *'"accountEmail":"cli@example.com"'*'"accountID":"workspace-cli"'*'"loginMethod":"Managed Codex account"'*) ;;
  *) printf 'managed account missing from usage output: %s\n' "$output" >&2; exit 1 ;;
esac

output=$($binary codex-accounts remove cli@example.com)
[ "$output" = 'Removed managed Codex account.' ]
[ ! -e "$home" ]
[ "$($binary codex-accounts list --json)" = '[]' ]
