#!/bin/sh

set -eu

binary=$1
work=$(mktemp -d "$PWD/codexbar-sessions-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT
export CODEXBAR_CONFIG="$work/config.json"
proc=$work/proc
home=$work/home
codex_home=$work/codex
project=$work/project
now=$(date +%s)
partition=$(date '+%Y/%m/%d')
mkdir -p "$proc/101" "$proc/202" "$proc/303" "$project" \
  "$codex_home/sessions/$partition"
printf 'codex\0exec\0' >"$proc/101/cmdline"
printf 'claude\0' >"$proc/202/cmdline"
printf 'codex\0app-server\0' >"$proc/303/cmdline"
ln -s "$project" "$proc/101/cwd"
ln -s "$project" "$proc/202/cwd"

rollout=$codex_home/sessions/$partition/rollout-fixture.jsonl
cat >"$rollout" <<EOF
{"type":"session_meta","payload":{"id":"codex-session","cwd":"$project","originator":"codex_exec"}}
EOF

escaped=$(printf '%s' "$project" | sed 's/[^[:alnum:]]/-/g')
claude_dir=$home/.claude/projects/$escaped
mkdir -p "$claude_dir"
printf '{}\n' >"$claude_dir/claude-session.jsonl"

output=$(HOME="$home" CODEX_HOME="$codex_home" CODEXBAR_SESSION_PROC_ROOT="$proc" \
  CODEXBAR_SESSION_NOW="$now" "$binary" sessions --json)
case "$output" in
  *'"id":"codex-session"'*'"provider":"codex"'*'"source":"cli"'*'"state":"active"'*'"pid":101'*'"cwd":"'*'"projectName":"project"'*'"startedAt":null'*'"lastActivityAt":"'*'"transcriptPath":"'*'"host":"'*) ;;
  *)
    printf 'unexpected Codex session JSON: %s\n' "$output" >&2
    exit 1
    ;;
esac
case "$output" in
  *'"id":"claude-session"'*'"provider":"claude"'*'"pid":202'*) ;;
  *)
    printf 'unexpected Claude session JSON: %s\n' "$output" >&2
    exit 1
    ;;
esac
case "$output" in
  *'"pid":303'*)
    printf 'app server was reported as an agent session\n' >&2
    exit 1
    ;;
esac

output=$(HOME="$home" CODEX_HOME="$codex_home" CODEXBAR_SESSION_PROC_ROOT="$proc" \
  CODEXBAR_SESSION_NOW="$now" "$binary" sessions)
case "$output" in
  'STATE   PROVIDER'*) ;;
  *)
    printf 'unexpected session table: %s\n' "$output" >&2
    exit 1
    ;;
esac
case "$output" in
  *codex-session*) ;;
  *)
    printf 'Codex session is missing from table: %s\n' "$output" >&2
    exit 1
    ;;
esac
case "$output" in
  *claude-session*) ;;
  *)
    printf 'Claude session is missing from table: %s\n' "$output" >&2
    exit 1
    ;;
esac

set +e
HOME="$home" CODEX_HOME="$codex_home" CODEXBAR_SESSION_PROC_ROOT="$proc" CODEXBAR_SESSION_NOW="$now" \
  "$binary" sessions focus codex-session >"$work/output" 2>"$work/error"
status=$?
set -e
[ "$status" -eq 2 ]
[ "$(cat "$work/error")" = 'Session focus is not available on Linux.' ]

if HOME="$home" CODEX_HOME="$codex_home" CODEXBAR_SESSION_PROC_ROOT="$proc" CODEXBAR_SESSION_NOW="$now" \
  "$binary" sessions focus missing >"$work/output" 2>"$work/error"; then
    printf 'unknown session unexpectedly focused\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Unknown session: missing' ]

fake_bin=$work/bin
marker=$work/remote-command-ran
mkdir -p "$fake_bin"
cat >"$fake_bin/tailscale" <<EOF
#!/bin/sh
printf '%s\n' '{"BackendState":"Running","Self":{"DNSName":"local.tail.ts.net"},"Peer":[{"DNSName":"discovered.tail.ts.net","OS":"linux","Online":true}]}'
printf 'tailscale\n' >>"$marker"
EOF
cat >"$fake_bin/ssh" <<EOF
#!/bin/sh
host=\$5
printf '[{"id":"remote-%s","provider":"codex","source":"cli","state":"idle","pid":null,"cwd":null,"projectName":null,"sessionName":"Remote fixture","startedAt":null,"lastActivityAt":null,"transcriptPath":null,"host":"ignored"}]\n' "\$host"
printf 'ssh:%s\n' "\$host" >>"$marker"
exit 0
EOF
chmod +x "$fake_bin/tailscale" "$fake_bin/ssh"

cat >"$CODEXBAR_CONFIG" <<EOF
{"agentSessionsEnabled":false,"agentSessionsManualHosts":"manual","providers":[]}
EOF
PATH="$fake_bin:$PATH" HOME="$home" CODEX_HOME="$codex_home" CODEXBAR_SESSION_PROC_ROOT="$proc" \
  CODEXBAR_SESSION_NOW="$now" "$binary" sessions --json >"$work/output"
[ ! -e "$marker" ]

cat >"$CODEXBAR_CONFIG" <<EOF
{"agentSessionsEnabled":true,"agentSessionsManualHosts":" manual, MANUAL ","providers":[]}
EOF
output=$(PATH="$fake_bin:$PATH" HOME="$home" CODEX_HOME="$codex_home" CODEXBAR_SESSION_PROC_ROOT="$proc" \
  CODEXBAR_SESSION_NOW="$now" "$binary" sessions --json)
case "$output" in
  *'"id":"remote-discovered"'*'"sessionName":"Remote fixture"'*'"host":"discovered"'*) ;;
  *)
    printf 'discovered remote session is missing: %s\n' "$output" >&2
    exit 1
    ;;
esac
case "$output" in
  *'"id":"remote-manual"'*'"host":"manual"'*) ;;
  *)
    printf 'manual remote session is missing: %s\n' "$output" >&2
    exit 1
    ;;
esac
[ "$(grep -c '^ssh:manual$' "$marker")" -eq 1 ]
[ "$(grep -c '^ssh:discovered$' "$marker")" -eq 1 ]
