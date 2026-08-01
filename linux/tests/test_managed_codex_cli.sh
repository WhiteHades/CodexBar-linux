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
export CODEXBAR_CONFIG="$work/config.json"
printf '%s\n' '{"version":1,"providers":[{"id":"codex","enabled":true,"source":"cli"}]}' >"$CODEXBAR_CONFIG"
output=$($binary codex-accounts import "$auth")
case "$output" in
  'Imported managed Codex account cli@example.com ('*')'.) ;;
  *) printf 'unexpected import output: %s\n' "$output" >&2; exit 1 ;;
esac

$binary codex-accounts select managed cli@example.com >/dev/null
output=$($binary config dump --show-secrets)
case "$output" in
  *'"codexActiveSource":{"kind":"managedAccount","accountID":"'*) ;;
  *) printf 'managed selection was not persisted: %s\n' "$output" >&2; exit 1 ;;
esac
profile="$work/profile-home"
mkdir -p "$profile"
$binary codex-accounts select profile "$profile" >/dev/null
output=$($binary config dump --show-secrets)
case "$output" in
  *'"codexActiveSource":{"kind":"liveSystem","homePath":"'*'"}'*) ;;
  *) printf 'profile selection was not downgrade-readable: %s\n' "$output" >&2; exit 1 ;;
esac
case "$output" in
  *'"codexProfileHomePaths":["'*'"]'*) ;;
  *) printf 'profile selection was not added to configured homes: %s\n' "$output" >&2; exit 1 ;;
esac
$binary codex-accounts select live >/dev/null
output=$($binary config dump --show-secrets)
case "$output" in
  *'"codexActiveSource":{"kind":"liveSystem"}'*) ;;
  *) printf 'live selection was not persisted: %s\n' "$output" >&2; exit 1 ;;
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

output=$($binary codex-accounts reauth cli@example.com --timeout 5)
case "$output" in
  'Reauthenticated managed Codex account cli@example.com ('*')'.) ;;
  *) printf 'unexpected reauth output: %s\n' "$output" >&2; exit 1 ;;
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
$binary codex-accounts select managed cli@example.com >/dev/null
output=$($binary usage --format json)
[ "$(printf '%s' "$output" | grep -o '"provider":"codex"' | wc -l)" -eq 2 ]
case "$output" in
  '[{"provider":"codex","account":"cli@example.com"'*'"accountID":"workspace-cli"'*'"loginMethod":"Managed Codex account"'*) ;;
  *) printf 'managed account missing from usage output: %s\n' "$output" >&2; exit 1 ;;
esac

cp "$store" "$work/managed.backup.json"
printf '%s\n' '{"version":3,"accounts":[' >"$store"
output=$($binary usage --format json || true)
case "$output" in
  '[{"provider":"codex","source":"cli","error":{"message":"Managed Codex store is unreadable; selected home remains fail-closed at '*'managed-store-unreadable:'*) ;;
  *) printf 'unreadable managed store did not fail closed: %s\n' "$output" >&2; exit 1 ;;
esac
mv "$work/managed.backup.json" "$store"

output=$($binary codex-accounts remove cli@example.com)
[ "$output" = 'Removed managed Codex account.' ]
[ ! -e "$home" ]
[ "$($binary codex-accounts list --json)" = '[]' ]
output=$($binary config dump --show-secrets)
case "$output" in
  *'"codexActiveSource":{"kind":"liveSystem"}'*) ;;
  *) printf 'removing the selected account did not restore live selection: %s\n' "$output" >&2; exit 1 ;;
esac
