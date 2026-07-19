#!/bin/sh

set -eu

binary=$1
backend=$2
application_entry=$3
autostart_entry=$4
binary_dir=$(cd "$(dirname "$binary")" && pwd)
binary=$binary_dir/$(basename "$binary")
backend_dir=$(cd "$(dirname "$backend")" && pwd)
backend=$backend_dir/$(basename "$backend")
work=$(mktemp -d "$PWD/codexbar-status-item.XXXXXX")
trap 'rm -rf "$work"' EXIT

grep -q '^Exec=codexbar-linux tui$' "$application_entry"
grep -q '^Terminal=false$' "$application_entry"
grep -q '^Exec=codexbar-linux status-item$' "$autostart_entry"
grep -q '^NoDisplay=true$' "$autostart_entry"

stub_dir=$work/bin
launch_log=$work/launch.log
mkdir -p "$stub_dir"
cat > "$stub_dir/xdg-terminal-exec" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" > "$CODEXBAR_TEST_LAUNCH_LOG"
EOF
chmod +x "$stub_dir/xdg-terminal-exec"

dbus-run-session -- sh -eu -c '
  binary=$1
  backend=$2
  stub_dir=$3
  launch_log=$4
  output=$5
  CODEXBAR_BACKEND="$backend" CODEXBAR_TEST_LAUNCH_LOG="$launch_log" PATH="$stub_dir:$PATH" \
    "$binary" status-item >"$output" 2>&1 &
  pid=$!
  trap "kill $pid 2>/dev/null || true; wait $pid 2>/dev/null || true" EXIT
  service=org.freedesktop.StatusNotifierItem-$pid-1

  found=false
  for attempt in $(seq 1 100); do
    if gdbus introspect --session --dest "$service" --object-path /StatusNotifierItem >/dev/null 2>&1; then
      found=true
      break
    fi
    sleep 0.02
  done
  test "$found" = true

  tooltip=
  for attempt in $(seq 1 100); do
    tooltip=$(gdbus call --session --dest "$service" --object-path /StatusNotifierItem \
      --method org.freedesktop.DBus.Properties.Get org.freedesktop.StatusNotifierItem ToolTip)
    case "$tooltip" in
      *codex*91*) break ;;
    esac
    sleep 0.02
  done
  case "$tooltip" in
    *codex*91*) ;;
    *) printf "status item tooltip did not contain usage: %s\n" "$tooltip" >&2; exit 1 ;;
  esac

  icon=$(gdbus call --session --dest "$service" --object-path /StatusNotifierItem \
    --method org.freedesktop.DBus.Properties.Get org.kde.StatusNotifierItem IconPixmap)
  case "$icon" in
    *32*32*) ;;
    *) printf "status item did not expose a 32px icon\n" >&2; exit 1 ;;
  esac

  gdbus call --session --dest "$service" --object-path /StatusNotifierItem \
    --method org.freedesktop.StatusNotifierItem.Activate 0 0 >/dev/null
  for attempt in $(seq 1 100); do
    test -s "$launch_log" && break
    sleep 0.02
  done
  test -s "$launch_log"
  kill "$pid"
  wait "$pid"
' sh "$binary" "$backend" "$stub_dir" "$launch_log" "$work/status.log"

cat > "$work/expected.log" <<EOF
--app-id=com.steipete.codexbar
--title=CodexBar
-e
$binary
tui
EOF
cmp -s "$work/expected.log" "$launch_log"
