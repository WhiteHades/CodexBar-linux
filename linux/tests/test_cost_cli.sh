#!/bin/sh

set -eu

binary=$1
work=$(mktemp -d "$PWD/codexbar-cost-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT
codex=$work/codex
claude=$work/claude/projects/sample
mkdir -p "$codex" "$claude"
timestamp=$(date '+%Y-%m-%dT%H:%M:%S%:z')

cat >"$codex/session.jsonl" <<EOF
{"timestamp":"$timestamp","type":"session_meta","payload":{"id":"session-1","cwd":"$work/project"}}
{"timestamp":"$timestamp","type":"turn_context","payload":{"model":"gpt-5"}}
{"timestamp":"$timestamp","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":1000,"cached_input_tokens":200,"output_tokens":100},"total_token_usage":{"input_tokens":1000,"cached_input_tokens":200,"output_tokens":100}}}}
{"timestamp":"$timestamp","type":"event_msg","payload":{"type":"token_count","info":{"last_token_usage":{"input_tokens":500,"cached_input_tokens":100,"output_tokens":50},"total_token_usage":{"input_tokens":1500,"cached_input_tokens":300,"output_tokens":150}}}}
EOF

cat >"$claude/session.jsonl" <<EOF
{"timestamp":"$timestamp","type":"assistant","requestId":"request-1","message":{"id":"message-1","model":"claude-sonnet-4-6","usage":{"input_tokens":50,"cache_read_input_tokens":10,"cache_creation_input_tokens":5,"output_tokens":2}}}
{"timestamp":"$timestamp","type":"assistant","requestId":"request-1","message":{"id":"message-1","model":"claude-sonnet-4-6","usage":{"input_tokens":100,"cache_read_input_tokens":20,"cache_creation_input_tokens":10,"output_tokens":5}}}
EOF

output=$(CODEXBAR_COST_CODEX_ROOT="$codex" CODEXBAR_COST_CLAUDE_ROOT="$work/claude" \
  "$binary" cost --provider codex --format json)
case "$output" in
  '[{"provider":"codex"'*'"sessionTokens":1650'*'"sessionCostUSD":0.0030375'*'"totalTokens":1650'*'"skippedForkFiles":0'*) ;;
  *)
    printf 'unexpected Codex cost output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_COST_CODEX_ROOT="$codex" "$binary" cost --provider codex --group-by project)
case "$output" in
  *'Projects (Last 30 days):'*'project: $0.0030, 1.6K tokens'*"$work/project"*) ;;
  *)
    printf 'unexpected project output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_COST_CLAUDE_ROOT="$work/claude" "$binary" cost --provider claude --json)
case "$output" in
  '[{"provider":"claude"'*'"sessionTokens":135'*'"sessionCostUSD":0.0004185'*'"totalTokens":135'*) ;;
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
