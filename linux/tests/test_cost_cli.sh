#!/bin/sh

set -eu

binary=$1
fixtures=$2
python=$3
mkdir -p .tmp
work=$(mktemp -d "$PWD/.tmp/codexbar-cost-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT
codex=$work/codex
claude=$work/claude/projects/sample
mkdir -p "$codex" "$claude"
cursor=$work/cursor-cache
antigravity=$work/antigravity-cache/sessions
mkdir -p "$cursor" "$antigravity"
fixed_timestamp='2026-08-03T12:00:00Z'
timestamp=$fixed_timestamp
previous_timestamp='2026-06-24T12:00:00Z'
pi=$work/pi
omp=$work/omp
mkdir -p "$pi/nested" "$omp/nested"
cp "$fixtures/pi-cost.jsonl" "$pi/nested/session.jsonl"
ln -s "$pi/nested/session.jsonl" "$omp/nested/linked.jsonl"
cp "$fixtures/omp-cost.jsonl" "$omp/nested/session.jsonl"

cat >"$codex/session.jsonl" <<EOF
{"timestamp":"$timestamp","type":"session_meta","payload":{"id":"session-1","cwd":"$work/project"}}
{"timestamp":"$timestamp","type":"turn_context","payload":{"model":"gpt-5"}}
{"timestamp":"$timestamp","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":1000,"cached_input_tokens":200,"output_tokens":100,"reasoning_output_tokens":40},"total_token_usage":{"input_tokens":1000,"cached_input_tokens":200,"output_tokens":100,"reasoning_output_tokens":40}}}}
{"timestamp":"$timestamp","type":"turn_context","payload":{"model":" OpenAI/GPT-5 "}}
{"timestamp":"$timestamp","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":500,"cached_input_tokens":100,"output_tokens":50,"reasoning_output_tokens":20},"total_token_usage":{"input_tokens":1500,"cached_input_tokens":300,"output_tokens":150,"reasoning_output_tokens":60}}}}
EOF

cat >"$codex/previous.jsonl" <<EOF
{"timestamp":"$previous_timestamp","type":"session_meta","payload":{"id":"session-previous","cwd":"$work/old-project"}}
{"timestamp":"$previous_timestamp","type":"turn_context","payload":{"model":"gpt-5-mini"}}
{"timestamp":"$previous_timestamp","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":80,"cached_input_tokens":10,"output_tokens":20},"total_token_usage":{"input_tokens":80,"cached_input_tokens":10,"output_tokens":20}}}}
EOF

cat >"$codex/subagent.jsonl" <<EOF
{"timestamp":"$timestamp","type":"session_meta","payload":{"id":"session-subagent","cwd":"$work/project","forked_from_id":"session-1","source":{"subagent":{"thread_spawn":{"parent_thread_id":"session-1"}}}}}
{"timestamp":"$timestamp","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":1500,"cached_input_tokens":300,"output_tokens":150}}}}
{"timestamp":"$timestamp","type":"turn_context","payload":{"model":"gpt-5"}}
{"timestamp":"$timestamp","type":"inter_agent_communication_metadata","payload":{"trigger_turn":true}}
{"timestamp":"$timestamp","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":50,"cached_input_tokens":10,"output_tokens":5},"total_token_usage":{"input_tokens":1550,"cached_input_tokens":310,"output_tokens":155}}}}
EOF

cat >"$codex/fork.jsonl" <<EOF
{"timestamp":"$timestamp","type":"session_meta","payload":{"id":"session-fork","cwd":"$work/project","forked_from_id":"session-1","parent_thread_id":"session-1","source":"cli"}}
{"timestamp":"$timestamp","type":"turn_context","payload":{"model":"gpt-5"}}
{"timestamp":"$timestamp","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":1000,"cached_input_tokens":200,"output_tokens":100}}}}
{"timestamp":"$timestamp","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":50,"cached_input_tokens":10,"output_tokens":5},"total_token_usage":{"input_tokens":1050,"cached_input_tokens":210,"output_tokens":105}}}}
EOF

cat >"$codex/priority.jsonl" <<EOF
{"timestamp":"$fixed_timestamp","type":"session_meta","payload":{"id":"priority-session","cwd":"/tmp/local-parity-project"}}
{"timestamp":"$fixed_timestamp","type":"turn_context","payload":{"model":"gpt-5.6-sol"}}
{"timestamp":"$fixed_timestamp","type":"event_msg","payload":{"type":"task_started","turn_id":"priority-turn"}}
{"timestamp":"$fixed_timestamp","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":100000,"cached_input_tokens":20000,"output_tokens":20000},"total_token_usage":{"input_tokens":100000,"cached_input_tokens":20000,"output_tokens":20000}}}}
EOF

trace=$work/logs_2.sqlite
"$python" - "$trace" "$fixtures/priority-trace.txt" <<'PY'
import pathlib
import sqlite3
import sys

database, fixture = sys.argv[1:]
connection = sqlite3.connect(database)
connection.execute("create table logs (id integer primary key autoincrement, ts integer not null, feedback_log_body text)")
connection.execute("create index idx_logs_ts on logs(ts desc, id desc)")
connection.execute("insert into logs (ts, feedback_log_body) values (?, ?)", (1785758400, pathlib.Path(fixture).read_text()))
connection.commit()
connection.close()
PY

cat >"$claude/session.jsonl" <<EOF
{"timestamp":"$timestamp","type":"assistant","requestId":"request-1","message":{"id":"message-1","model":"claude-sonnet-4-6","usage":{"input_tokens":50,"cache_read_input_tokens":10,"cache_creation_input_tokens":5,"output_tokens":2}}}
{"timestamp":"$timestamp","type":"assistant","requestId":"request-1","message":{"id":"message-1","model":"claude-sonnet-4-6","usage":{"input_tokens":100,"cache_read_input_tokens":20,"cache_creation_input_tokens":10,"output_tokens":5}}}
EOF

cat >"$cursor/usage.csv" <<EOF
Date,Model,Input with Cache Write,Input without Cache Write,Cache Read,Output,Total Tokens,Cost
$timestamp,cursor-model,130,100,20,50,200,\$0.42
EOF

cat >"$antigravity/session.jsonl" <<EOF
{"type":"session_meta","sessionId":"ag-session","modelId":"gemini-2.5-pro"}
{"type":"usage","timestamp":1785758400000,"input":100,"cacheRead":20,"cacheWrite":5,"output":30,"responseId":"response-1"}
{"type":"usage","timestamp":1785758400000,"input":100,"cacheRead":20,"cacheWrite":5,"output":30,"responseId":"response-1"}
EOF

antigravity_db=$work/antigravity-db
mkdir -p "$antigravity_db"
"$python" - "$antigravity_db/db-session.db" <<'PY'
import sqlite3
import sys

def varint(value):
    out = bytearray()
    while value > 127:
        out.append((value & 127) | 128)
        value >>= 7
    out.append(value)
    return bytes(out)

def scalar(field, value):
    return varint(field << 3) + varint(value)

def message(field, value):
    return varint((field << 3) | 2) + varint(len(value)) + value

usage = scalar(2, 40) + scalar(5, 10) + scalar(9, 5) + message(11, b'db-response')
stamp = scalar(1, 1785758400)
generation = message(4, stamp)
chat = message(4, usage) + message(9, generation) + message(19, b'gemini-2.5-flash')
payload = message(1, chat)
db = sqlite3.connect(sys.argv[1])
db.execute('create table gen_metadata (idx integer, data blob)')
db.execute('insert into gen_metadata values (?, ?)', (7, payload))
db.commit()
db.close()
PY

output=$(CODEXBAR_COST_NOW="$fixed_timestamp" CODEXBAR_COST_CODEX_ROOT="$codex" \
  CODEXBAR_COST_CODEX_TRACE_DB="$trace" CODEXBAR_COST_PI_ROOT="$pi" CODEXBAR_COST_OMP_ROOT="$omp" \
  CODEXBAR_COST_CLAUDE_ROOT="$work/claude" \
  "$binary" cost --provider codex --format json)
case "$output" in
  '[{"provider":"codex"'*'"sessionTokens":121805'*'"sessionCostUSD":2.02354'*'"id":"gpt-5.6-sol"'*'"priorityTokens":120000'*'"priorityCostUSD":2.02'*'"id":"gpt-5"'*'"totalTokens":1760'*'"sessionReferences":3'*'"id":"gpt-5.4"'*'"totalTokens":45'*'"id":"gpt-5-mini"'*'"previousTotalTokens":100'*'"activeModelCount":3'*) ;;
  *)
    printf 'unexpected Codex cost output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_COST_NOW="$fixed_timestamp" CODEXBAR_COST_CODEX_ROOT="$codex" \
  CODEXBAR_COST_CODEX_TRACE_DB="$trace" CODEXBAR_COST_PI_ROOT="$pi" CODEXBAR_COST_OMP_ROOT="$omp" \
  "$binary" cost --provider codex --group-by model)
case "$output" in
  *'Models (Last 30 days):'*'gpt-5.6-sol: $2.0200 known, 120.0K tokens, 1 session refs'*'gpt-5: $0.0032 known, 1.8K tokens, 3 session refs'*'gpt-5.4: $0.0003 known, 45 tokens, 1 session refs'*) ;;
  *) printf 'unexpected model output: %s\n' "$output" >&2; exit 1 ;;
esac

output=$(CODEXBAR_COST_NOW="$fixed_timestamp" CODEXBAR_COST_CODEX_ROOT="$codex" \
  CODEXBAR_COST_CODEX_TRACE_DB="$trace" CODEXBAR_COST_PI_ROOT="$pi" CODEXBAR_COST_OMP_ROOT="$omp" \
  "$binary" cost --provider codex --group-by project)
case "$output" in
  *'Projects (Last 30 days):'*'local-parity-project: $2.0200, 120.0K tokens'*'project: $0.0032, 1.8K tokens'*"$work/project"*) ;;
  *)
    printf 'unexpected project output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_COST_NOW="$fixed_timestamp" CODEXBAR_COST_CLAUDE_ROOT="$work/claude" \
  CODEXBAR_COST_PI_ROOT="$pi" CODEXBAR_COST_OMP_ROOT="$omp" "$binary" cost --provider claude --json)
case "$output" in
  '[{"provider":"claude"'*'"sessionTokens":150'*'"sessionCostUSD":0.00050685'*'"models":[{"id":"claude-sonnet-4-6"'*'"totalTokens":150'*'"sessionReferences":2'*) ;;
  *)
    printf 'unexpected Claude cost output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_COST_NOW="$fixed_timestamp" CODEXBAR_COST_CURSOR_ROOT="$cursor" \
  "$binary" cost --provider cursor --json)
case "$output" in
  '[{"provider":"cursor"'*'"sessionTokens":200'*'"sessionCostUSD":0.42'*'"id":"cursor-model"'*'"totalTokens":200'*) ;;
  *) printf 'unexpected Cursor cost output: %s\n' "$output" >&2; exit 1 ;;
esac

output=$(CODEXBAR_COST_NOW="$fixed_timestamp" CODEXBAR_COST_ANTIGRAVITY_ROOT="$antigravity" \
  CODEXBAR_COST_ANTIGRAVITY_DB_ROOT="$antigravity_db" \
  "$binary" cost --provider antigravity --json)
case "$output" in
  '[{"provider":"antigravity"'*'"sessionTokens":210'*'"sessionCostUSD":null'*'"id":"gemini-2.5-pro"'*'"totalTokens":155'*'"unpricedTokens":155'*'"id":"gemini-2.5-flash"'*'"totalTokens":55'*'"unpricedTokens":55'*) ;;
  *) printf 'unexpected Antigravity cost output: %s\n' "$output" >&2; exit 1 ;;
esac

if "$binary" cost --provider openrouter >"$work/output" 2>"$work/error"; then
    printf 'unsupported provider unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: cost is only supported for Claude, Codex, Cursor, and Antigravity.' ]

if "$binary" cost --days 0 >"$work/output" 2>"$work/error"; then
    printf 'invalid day count unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: --days must be from 1 through 365.' ]
