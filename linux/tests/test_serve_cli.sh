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
