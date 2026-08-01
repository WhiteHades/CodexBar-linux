#!/bin/sh

set -eu

binary=$1
mkdir -p .tmp
work=$(mktemp -d "$PWD/.tmp/codexbar-cost-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT
codex=$work/codex
claude=$work/claude/projects/sample
mkdir -p "$codex" "$claude"
timestamp=$(date '+%Y-%m-%dT%H:%M:%S%:z')
previous_timestamp=$(date -d '40 days ago' '+%Y-%m-%dT%H:%M:%S%:z')

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
{"timestamp":"$timestamp","type":"turn_context","payload":{"model":"gpt-5"}}
{"timestamp":"$timestamp","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":100,"cached_input_tokens":20,"output_tokens":10}}}}
{"timestamp":"$timestamp","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":50,"cached_input_tokens":10,"output_tokens":5},"total_token_usage":{"input_tokens":150,"cached_input_tokens":30,"output_tokens":15}}}}
EOF

cat >"$codex/fork.jsonl" <<EOF
{"timestamp":"$timestamp","type":"session_meta","payload":{"id":"session-fork","cwd":"$work/project","forked_from_id":"session-1","parent_thread_id":"session-1","source":"cli"}}
{"timestamp":"$timestamp","type":"turn_context","payload":{"model":"gpt-5"}}
{"timestamp":"$timestamp","type":"event_msg","payload":{"type":"token_count","info":{"total_token_usage":{"input_tokens":1000,"cached_input_tokens":200,"output_tokens":100}}}}
{"timestamp":"$timestamp","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":50,"cached_input_tokens":10,"output_tokens":5},"total_token_usage":{"input_tokens":1050,"cached_input_tokens":210,"output_tokens":105}}}}
EOF

cat >"$claude/session.jsonl" <<EOF
{"timestamp":"$timestamp","type":"assistant","requestId":"request-1","message":{"id":"message-1","model":"claude-sonnet-4-6","usage":{"input_tokens":50,"cache_read_input_tokens":10,"cache_creation_input_tokens":5,"output_tokens":2}}}
{"timestamp":"$timestamp","type":"assistant","requestId":"request-1","message":{"id":"message-1","model":"claude-sonnet-4-6","usage":{"input_tokens":100,"cache_read_input_tokens":20,"cache_creation_input_tokens":10,"output_tokens":5}}}
EOF

output=$(CODEXBAR_COST_CODEX_ROOT="$codex" CODEXBAR_COST_CLAUDE_ROOT="$work/claude" \
  "$binary" cost --provider codex --format json)
case "$output" in
  '[{"provider":"codex"'*'"sessionTokens":1870'*'"sessionCostUSD":0.0034425'*'"models":[{"id":"gpt-5"'*'"rawAliases":[" OpenAI\/GPT-5 ","gpt-5"]'*'"reasoningTokens":60'*'"totalTokens":1870'*'"sessionReferences":3'*'"pricedTokens":1870'*'"kind":"new"'*'"id":"gpt-5-mini"'*'"previousTotalTokens":100'*'"kind":"ended"'*'"activeModelCount":1'*) ;;
  *)
    printf 'unexpected Codex cost output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_COST_CODEX_ROOT="$codex" "$binary" cost --provider codex --group-by model)
case "$output" in
  *'Models (Last 30 days):'*'gpt-5: $0.0034 known, 1.9K tokens, 3 session refs'*) ;;
  *) printf 'unexpected model output: %s\n' "$output" >&2; exit 1 ;;
esac

output=$(CODEXBAR_COST_CODEX_ROOT="$codex" "$binary" cost --provider codex --group-by project)
case "$output" in
  *'Projects (Last 30 days):'*'project: $0.0034, 1.9K tokens'*"$work/project"*) ;;
  *)
    printf 'unexpected project output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_COST_CLAUDE_ROOT="$work/claude" "$binary" cost --provider claude --json)
case "$output" in
  '[{"provider":"claude"'*'"sessionTokens":135'*'"sessionCostUSD":0.0004185'*'"models":[{"id":"claude-sonnet-4-6"'*'"totalTokens":135'*'"sessionReferences":1'*) ;;
  *)
    printf 'unexpected Claude cost output: %s\n' "$output" >&2
    exit 1
    ;;
esac

if "$binary" cost --provider openrouter >"$work/output" 2>"$work/error"; then
    printf 'unsupported provider unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: cost is only supported for Claude and Codex.' ]

if "$binary" cost --days 0 >"$work/output" 2>"$work/error"; then
    printf 'invalid day count unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: --days must be from 1 through 365.' ]
