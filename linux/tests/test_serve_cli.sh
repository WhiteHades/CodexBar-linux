#!/bin/sh

set -eu

binary=$1
backend=$2
work=$(mktemp -d "$PWD/codexbar-serve-cli.XXXXXX")
server_pid=
trap 'if [ -n "$server_pid" ]; then kill "$server_pid" 2>/dev/null || true; wait "$server_pid" 2>/dev/null || true; fi; rm -rf "$work"' EXIT
port=$((40000 + ($$ % 10000)))
count=$work/count
wrapper=$work/backend.sh
mkdir -p "$work/codex" "$work/claude"

cat >"$wrapper" <<'EOF'
#!/bin/sh
value=0
if [ -f "$CODEXBAR_TEST_COUNT" ]; then value=$(cat "$CODEXBAR_TEST_COUNT"); fi
value=$((value + 1))
printf '%s\n' "$value" >"$CODEXBAR_TEST_COUNT"
if [ "$value" -ge 2 ]; then trap '' TERM; sleep 3; fi
exec "$CODEXBAR_TEST_BACKEND"
EOF
chmod +x "$wrapper"

CODEXBAR_BACKEND="$wrapper" CODEXBAR_TEST_BACKEND="$backend" CODEXBAR_TEST_COUNT="$count" \
  CODEXBAR_COST_CODEX_ROOT="$work/codex" CODEXBAR_COST_CLAUDE_ROOT="$work/claude" \
  "$binary" serve --port "$port" --refresh-interval 60 --request-timeout 0.2 \
  >"$work/server.out" 2>"$work/server.err" &
server_pid=$!

ready=false
for _ in $(seq 1 50); do
    if curl -fsS --max-time 1 "http://127.0.0.1:$port/health" >"$work/health" 2>/dev/null; then
        ready=true
        break
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then break; fi
    sleep 0.1
done
if [ "$ready" != true ]; then
    cat "$work/server.err" >&2
    printf 'server did not become ready\n' >&2
    exit 1
fi
case "$(cat "$work/health")" in
  '{"status":"ok","version":"'*'"}') ;;
  *)
    printf 'unexpected health response\n' >&2
    exit 1
    ;;
esac

status=$(curl -sS --max-time 2 -D "$work/dashboard-denied.headers" -o "$work/dashboard-denied" \
  -w '%{http_code}' "http://127.0.0.1:$port/dashboard/v1/snapshot")
[ "$status" -eq 401 ]
[ "$(cat "$work/dashboard-denied")" = '{"error":"unauthorized"}' ]
grep -qi '^WWW-Authenticate: Bearer' "$work/dashboard-denied.headers"
grep -qi '^Cache-Control: no-store' "$work/dashboard-denied.headers"

status=$(curl -sS --max-time 2 -o "$work/dashboard-query" -w '%{http_code}' \
  "http://127.0.0.1:$port/dashboard/v1/snapshot?token=secret")
[ "$status" -eq 401 ]

status=$(curl -sS --max-time 2 -o "$work/dashboard-duplicate" -w '%{http_code}' \
  -H 'Authorization: Bearer one' -H 'Authorization: Bearer two' \
  "http://127.0.0.1:$port/dashboard/v1/snapshot")
[ "$status" -eq 400 ]
[ "$(cat "$work/dashboard-duplicate")" = '{"error":"invalid request"}' ]

output=$(curl -fsS --max-time 2 "http://127.0.0.1:$port/usage?provider=codex")
case "$output" in
  '[{"provider":"codex"'*) ;;
  *)
    printf 'unexpected usage response: %s\n' "$output" >&2
    exit 1
    ;;
esac
[ "$(cat "$count")" -eq 1 ]
curl -fsS --max-time 2 "http://127.0.0.1:$port/usage?provider=codex" >/dev/null
[ "$(cat "$count")" -eq 1 ]

status=$(curl -sS --max-time 2 -o "$work/timeout" -w '%{http_code}' \
  "http://127.0.0.1:$port/usage?provider=claude")
[ "$status" -eq 504 ]
[ "$(cat "$work/timeout")" = '{"error":"request timed out"}' ]

output=$(curl -fsS --max-time 2 "http://127.0.0.1:$port/cost?provider=codex")
case "$output" in
  '[{"provider":"codex"'*'"source":"local"'*) ;;
  *)
    printf 'unexpected cost response: %s\n' "$output" >&2
    exit 1
    ;;
esac

status=$(curl -sS --max-time 2 -o "$work/forbidden" -w '%{http_code}' -H 'Host: example.com' \
  "http://127.0.0.1:$port/health")
[ "$status" -eq 403 ]
[ "$(cat "$work/forbidden")" = '{"error":"forbidden host"}' ]

status=$(curl -sS --max-time 2 -o "$work/method" -w '%{http_code}' -X POST \
  "http://127.0.0.1:$port/health")
[ "$status" -eq 405 ]

status=$(curl -sS --max-time 2 -o "$work/missing" -w '%{http_code}' \
  "http://127.0.0.1:$port/missing")
[ "$status" -eq 404 ]

kill "$server_pid"
wait "$server_pid"
server_pid=

if "$binary" serve --port 0 >"$work/output" 2>"$work/error"; then
    printf 'invalid port unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: --port must be between 1 and 65535.' ]

if "$binary" serve --request-timeout nan >"$work/output" 2>"$work/error"; then
    printf 'non-finite timeout unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: --request-timeout must be zero or greater and no more than 86400.' ]

if "$binary" serve --host example.com >"$work/output" 2>"$work/error"; then
    printf 'non-IP host unexpectedly accepted\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = "Error: --host must be 'localhost' or an IPv4 address." ]

if "$binary" serve --host 0.0.0.0 >"$work/output" 2>"$work/error"; then
    printf 'non-loopback host without token unexpectedly accepted\n' >&2
    exit 1
fi
grep -q 'dashboard-token.*required for non-loopback' "$work/error"

if "$binary" serve --host 0.0.0.0 --dashboard-token secret >"$work/output" 2>"$work/error"; then
    printf 'non-loopback cleartext token unexpectedly accepted without opt-in\n' >&2
    exit 1
fi
grep -q 'Refusing to serve the dashboard token over cleartext HTTP' "$work/error"

if CODEXBAR_DASHBOARD_TOKEN='  ' "$binary" serve >"$work/output" 2>"$work/error"; then
    printf 'blank environment token unexpectedly accepted\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: CODEXBAR_DASHBOARD_TOKEN must not be empty or whitespace.' ]

"$binary" serve --help >"$work/help"
grep -q -- '--host <localhost|IPv4>' "$work/help"
grep -q -- '--dashboard-token <token>' "$work/help"
grep -q -- '--allow-plain-http' "$work/help"
grep -q 'GET /dashboard/v1/snapshot' "$work/help"
grep -q 'CODEXBAR_DASHBOARD_TOKEN' "$work/help"
grep -q 'cleartext' "$work/help"

CODEXBAR_BACKEND="$backend" CODEXBAR_COST_CODEX_ROOT="$work/codex" CODEXBAR_COST_CLAUDE_ROOT="$work/claude" \
  CODEXBAR_DASHBOARD_TOKEN=environment-secret \
  "$binary" serve --port "$port" --dashboard-token flag-secret --refresh-interval 60 --request-timeout 2 \
  >"$work/server.out" 2>"$work/server.err" &
server_pid=$!

ready=false
for _ in $(seq 1 50); do
    if curl -fsS --max-time 1 "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
        ready=true
        break
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then break; fi
    sleep 0.1
done
[ "$ready" = true ]

status=$(curl -sS --max-time 2 -o "$work/wrong-token" -w '%{http_code}' \
  -H 'Authorization: Bearer flag-secret' "http://127.0.0.1:$port/dashboard/v1/snapshot")
[ "$status" -eq 401 ]

output=$(curl -fsS --max-time 4 -D "$work/dashboard.headers" \
  -H 'Authorization: bearer environment-secret' "http://127.0.0.1:$port/dashboard/v1/snapshot")
case "$output" in
  *'"schemaVersion":1'*'"codexBarVersion":"'*'"providers":['*) ;;
  *) printf 'unexpected dashboard envelope: %s\n' "$output" >&2; exit 1 ;;
esac
case "$output" in
  *'"id":"codex"'*'"accountEmail":"redacted@example.test"'*'"plan":"Pro"'*) ;;
  *) printf 'unexpected dashboard identity: %s\n' "$output" >&2; exit 1 ;;
esac
case "$output" in
  *'"kind":"session"'*'"usedPercent":28'*'"remainingPercent":72'*) ;;
  *) printf 'unexpected dashboard snapshot: %s\n' "$output" >&2; exit 1 ;;
esac
case "$output" in
  *'dev@example.test'*|*'accountOrganization'*|*'accountID'*)
    printf 'dashboard exposed unredacted identity: %s\n' "$output" >&2
    exit 1
    ;;
esac
[ "$(grep -ci '^Cache-Control: no-store' "$work/dashboard.headers")" -eq 1 ]

output=$(curl -fsS --max-time 2 "http://127.0.0.1:$port/usage?provider=codex")
case "$output" in
  '[{"provider":"codex"'*) ;;
  *) printf 'loopback usage unexpectedly required auth: %s\n' "$output" >&2; exit 1 ;;
esac

kill "$server_pid"
wait "$server_pid"
server_pid=

CODEXBAR_BACKEND="$backend" CODEXBAR_COST_CODEX_ROOT="$work/codex" CODEXBAR_COST_CLAUDE_ROOT="$work/claude" \
  "$binary" serve --host 0.0.0.0 --port "$port" --dashboard-token network-secret --allow-plain-http \
  --request-timeout 2 >"$work/server.out" 2>"$work/server.err" &
server_pid=$!

ready=false
for _ in $(seq 1 50); do
    if curl -fsS --max-time 1 "http://127.0.0.1:$port/health" >/dev/null 2>&1; then
        ready=true
        break
    fi
    if ! kill -0 "$server_pid" 2>/dev/null; then break; fi
    sleep 0.1
done
[ "$ready" = true ]
grep -q 'plain HTTP on a non-loopback host' "$work/server.err"

status=$(curl -sS --max-time 2 -o "$work/network-usage-denied" -w '%{http_code}' \
  "http://127.0.0.1:$port/usage?provider=codex")
[ "$status" -eq 401 ]
output=$(curl -fsS --max-time 2 -H 'Authorization: Bearer network-secret' \
  "http://127.0.0.1:$port/usage?provider=codex")
case "$output" in
  '[{"provider":"codex"'*) ;;
  *) printf 'authorized network usage failed: %s\n' "$output" >&2; exit 1 ;;
esac
curl -fsS --max-time 2 -H 'Host: example.com' "http://127.0.0.1:$port/health" >/dev/null

kill "$server_pid"
wait "$server_pid"
server_pid=
