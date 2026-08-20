#!/bin/sh

set -eu

binary=$1
fixtures=$2
python=$3
work=$(mktemp -d "$PWD/codexbar-sessions-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT
export CODEXBAR_CONFIG="$work/config.json"
proc=$work/proc
home=$work/home
codex_home=$work/codex
project=$work/project
now=$(date +%s)
partition=$(date '+%Y/%m/%d')
mkdir -p "$proc/101" "$proc/202" "$proc/303" "$proc/404" "$proc/505" "$proc/606" "$project" \
  "$codex_home/sessions/$partition"
printf 'codex\0exec\0' >"$proc/101/cmdline"
printf 'claude\0' >"$proc/202/cmdline"
printf 'codex\0app-server\0' >"$proc/303/cmdline"
printf 'pi\0' >"$proc/404/cmdline"
printf 'bun\0/tools/oh-my-pi/omp\0' >"$proc/505/cmdline"
printf 'omp\0--help\0' >"$proc/606/cmdline"
ln -s "$project" "$proc/101/cwd"
ln -s "$project" "$proc/202/cwd"
ln -s "$project" "$proc/404/cwd"
ln -s "$project" "$proc/505/cwd"

rollout=$codex_home/sessions/$partition/rollout-fixture.jsonl
cat >"$rollout" <<EOF
{"type":"session_meta","payload":{"id":"codex-session","cwd":"$project","originator":"codex_exec","source":{"subagent":{"thread_spawn":{"agent_path":"/root/code_review"}}}}}
EOF

"$python" - "$codex_home/state_5.sqlite" <<'PY'
import sqlite3
import sys

connection = sqlite3.connect(sys.argv[1])
connection.execute("create table threads (id text primary key, title text, agent_path text)")
connection.execute("insert into threads values ('codex-session', 'Fix local session labels', '/root/code_review')")
connection.commit()
connection.close()
PY

escaped=$(printf '%s' "$project" | sed 's/[^[:alnum:]]/-/g')
claude_dir=$home/.claude/projects/$escaped
mkdir -p "$claude_dir"
printf '{}\n' >"$claude_dir/claude-session.jsonl"

pi_dir=$home/.pi/agent/sessions/project
omp_dir=$home/.omp/agent/sessions/project
mkdir -p "$pi_dir" "$omp_dir"
sed "s|@PROJECT@|$project|g" "$fixtures/pi-session.jsonl" >"$pi_dir/pi.jsonl"
sed "s|@PROJECT@|$project|g" "$fixtures/omp-session.jsonl" >"$omp_dir/omp.jsonl"

output=$(HOME="$home" CODEX_HOME="$codex_home" CODEXBAR_SESSION_PROC_ROOT="$proc" \
  CODEXBAR_SESSION_NOW="$now" "$binary" sessions --json-v2)
case "$output" in
  *'"id":"codex-session"'*'"provider":"codex"'*'"source":"cli"'*'"state":"active"'*'"pid":101'*'"cwd":"'*'"projectName":"project"'*'"sessionName":"Fix local session labels'*'"startedAt":null'*'"lastActivityAt":"'*'"transcriptPath":"'*'"host":"'*) ;;
  *)
    printf 'unexpected Codex session JSON: %s\n' "$output" >&2
    exit 1
    ;;
esac
case "$output" in
  *'"id":"pi-session"'*'"provider":"pi"'*'"dialect":"pi"'*'"pid":404'*'"sessionName":"Plain pi fixture"'*) ;;
  *) printf 'unexpected pi session JSON: %s\n' "$output" >&2; exit 1 ;;
esac
case "$output" in
  *'"id":"omp-session"'*'"provider":"pi"'*'"dialect":"omp"'*'"pid":505'*'"sessionName":"OMP fixture"'*) ;;
  *) printf 'unexpected OMP session JSON: %s\n' "$output" >&2; exit 1 ;;
esac
case "$output" in
  *'"id":"claude-session"'*'"provider":"claude"'*'"pid":202'*) ;;
  *)
    printf 'unexpected Claude session JSON: %s\n' "$output" >&2
    exit 1
    ;;
esac
case "$output" in
  *'"pid":303'*|*'"pid":606'*)
    printf 'app server was reported as an agent session\n' >&2
    exit 1
    ;;
esac

output=$(HOME="$home" CODEX_HOME="$codex_home" CODEXBAR_SESSION_PROC_ROOT="$proc" \
  CODEXBAR_SESSION_NOW="$now" "$binary" sessions)
case "$output" in
  'STATE   PROVIDER  DIALECT'*) ;;
  *)
    printf 'unexpected session table: %s\n' "$output" >&2
    exit 1
    ;;
esac
case "$output" in
  *'Fix local session labels'*) ;;
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
case "$output" in
  *'omp'*'OMP fixture'*'pi'*'Plain pi fixture'*) ;;
  *) printf 'Pi-family labels are missing from table: %s\n' "$output" >&2; exit 1 ;;
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
