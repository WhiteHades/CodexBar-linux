#!/bin/sh

set -eu

binary=$1
binary_dir=$(cd "$(dirname "$binary")" && pwd)
binary=$binary_dir/$(basename "$binary")
work=$(mktemp -d "$PWD/codexbar-tui-launch.XXXXXX")
trap 'rm -rf "$work"' EXIT

stub_dir=$work/bin
launch_log=$work/launch.log
expected=$work/expected.log
mkdir -p "$stub_dir"

cat > "$stub_dir/xdg-terminal-exec" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$CODEXBAR_TEST_LAUNCH_LOG"
EOF
chmod +x "$stub_dir/xdg-terminal-exec"

timeout 2 env PATH="$stub_dir" CODEXBAR_TEST_LAUNCH_LOG="$launch_log" \
  "$binary" tui </dev/null >"$work/stdout.log" 2>"$work/stderr.log" || true

for attempt in $(seq 1 50); do
  test -s "$launch_log" && break
  sleep 0.02
done

cat > "$expected" <<EOF
--app-id=com.steipete.codexbar
--title=CodexBar
-e
$binary
tui
EOF

if ! cmp -s "$expected" "$launch_log"; then
  printf 'tui did not launch the default terminal with the expected arguments\n' >&2
  printf 'stderr:\n' >&2
  cat "$work/stderr.log" >&2
  printf 'launch arguments:\n' >&2
  cat "$launch_log" >&2 2>/dev/null || true
  exit 1
fi

fallback_dir=$work/fallback
fallback_log=$work/fallback.log
mkdir -p "$fallback_dir"
cat > "$fallback_dir/ghostty" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$CODEXBAR_TEST_LAUNCH_LOG"
EOF
chmod +x "$fallback_dir/ghostty"

timeout 2 env PATH="$fallback_dir" CODEXBAR_TEST_LAUNCH_LOG="$fallback_log" \
  "$binary" tui </dev/null >"$work/fallback-stdout.log" 2>"$work/fallback-stderr.log"

for attempt in $(seq 1 50); do
  test -s "$fallback_log" && break
  sleep 0.02
done

cat > "$expected" <<EOF
--class=com.steipete.codexbar
--title=CodexBar
-e
$binary
tui
EOF

if ! cmp -s "$expected" "$fallback_log"; then
  printf 'tui did not launch the fallback terminal with the expected arguments\n' >&2
  cat "$fallback_log" >&2 2>/dev/null || true
  exit 1
fi

empty_dir=$work/empty
mkdir -p "$empty_dir"
if timeout 2 env PATH="$empty_dir" "$binary" tui \
  </dev/null >"$work/missing-stdout.log" 2>"$work/missing-stderr.log";
then
  printf 'tui succeeded without a terminal emulator\n' >&2
  exit 1
fi

if ! grep -q 'no supported terminal emulator was found' "$work/missing-stderr.log"; then
  printf 'tui did not report the missing terminal emulator\n' >&2
  cat "$work/missing-stderr.log" >&2
  exit 1
fi
