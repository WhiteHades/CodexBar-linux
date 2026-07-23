#!/bin/sh

set -eu

binary=$1
backend=$2
expected_version=$3
jetbrains_fixture=$4
work=$(mktemp -d "$PWD/codexbar-usage-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT
config=$work/config.json

output=$(CODEXBAR_BACKEND="$backend" "$binary")
case "$output" in
  *'codex · dev@example.test · Pro [oauth]'*'session: 28% used'*'claude [cli]'*) ;;
  *)
    printf 'unexpected default usage output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_BACKEND="$backend" "$binary" usage --provider codex --format json)
case "$output" in
  '[{"provider":"codex"'*'"primary":{'*'"usedPercent":28'*'"credits":{'*'"remaining":12.5'*) ;;
  *)
    printf 'unexpected codex JSON output: %s\n' "$output" >&2
    exit 1
    ;;
esac

nonzero_backend=$work/nonzero-backend.sh
cat > "$nonzero_backend" <<'EOF'
#!/bin/sh
"$CODEXBAR_TEST_BACKEND" "$@"
exit 7
EOF
chmod +x "$nonzero_backend"
nonzero_output=$(CODEXBAR_BACKEND="$nonzero_backend" CODEXBAR_TEST_BACKEND="$backend" \
  "$binary" usage --provider codex --format json)
case "$nonzero_output" in
  '[{"provider":"codex"'*) ;;
  *)
    printf 'valid backend JSON was rejected after a nonzero exit: %s\n' "$nonzero_output" >&2
    exit 1
    ;;
esac

invalid_stderr_backend=$work/invalid-stderr-backend.sh
cat > "$invalid_stderr_backend" <<'EOF'
#!/bin/sh
"$CODEXBAR_TEST_BACKEND" "$@"
printf '\377' >&2
EOF
chmod +x "$invalid_stderr_backend"
if CODEXBAR_BACKEND="$invalid_stderr_backend" CODEXBAR_TEST_BACKEND="$backend" \
  "$binary" usage --provider codex --format json >"$work/invalid-stderr-output" 2>"$work/invalid-stderr-error"; then
    printf 'backend with invalid UTF-8 stderr unexpectedly succeeded\n' >&2
    exit 1
fi
case "$(cat "$work/invalid-stderr-output")" in
  *'Backend error output is not valid UTF-8 text'*) ;;
  *)
    printf 'unexpected invalid stderr diagnostic\n' >&2
    exit 1
    ;;
esac

root_output=$(CODEXBAR_BACKEND="$backend" "$binary" --provider codex --json)
[ "$root_output" = "$output" ]

output=$(CODEXBAR_BACKEND="$backend" "$binary" usage --provider both --json)
case "$output" in
  '[{"provider":"codex"'*'},{"provider":"claude"'*) ;;
  *)
    printf 'unexpected combined JSON output: %s\n' "$output" >&2
    exit 1
    ;;
esac

set +e
output=$(CODEXBAR_BACKEND="$backend" "$binary" usage --provider deepseek --source web --json 2>/dev/null)
status=$?
set -e
[ "$status" -eq 1 ]
[ "$output" = '[{"provider":"deepseek","source":"web","error":{"message":"Source '\''web'\'' is not supported for deepseek.","code":1,"kind":"provider"}}]' ]

if CODEXBAR_BACKEND="$backend" "$binary" usage --provider unknown >/dev/null 2>&1; then
    printf 'unknown provider unexpectedly succeeded\n' >&2
    exit 1
fi

output=$(CODEXBAR_BACKEND="$backend" "$binary" usage --provider unknown --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"cli","source":"cli","error":{"message":"Unknown provider: unknown","code":1,"kind":"args"}}]') ;;
  *)
    printf 'unexpected JSON error output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_BACKEND="$backend" "$binary" usage --provider BOTH --json)
case "$output" in
  '[{"provider":"codex"'*'},{"provider":"claude"'*) ;;
  *)
    printf 'uppercase provider keyword was not normalized\n' >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_BACKEND="$backend" "$binary" usage --provider codex --json --format text)
case "$output" in
  'codex · dev@example.test · Pro [oauth]'*) ;;
  *)
    printf 'explicit text format did not override JSON shortcut\n' >&2
    exit 1
    ;;
esac

[ "$("$binary" --version)" = "CodexBar $expected_version" ]

jetbrains_home=$work/jetbrains-home
jetbrains_quota_dir=$jetbrains_home/.config/JetBrains/IntelliJIdea2025.3/options
mkdir -p "$jetbrains_quota_dir"
cp "$jetbrains_fixture" "$jetbrains_quota_dir/AIAssistantQuotaManager2.xml"
output=$(env -u CODEXBAR_BACKEND HOME="$jetbrains_home" XDG_CONFIG_HOME="$jetbrains_home/.config" \
  XDG_DATA_HOME="$jetbrains_home/.local/share" CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider jetbrains --source cli --json)
case "$output" in
  '[{"provider":"jetbrains","source":"local"'*'"usedPercent":25'*'"accountOrganization":"IntelliJ IDEA 2025.3"'*'"loginMethod":"Available"'*) ;;
  *)
    printf 'unexpected native JetBrains output: %s\n' "$output" >&2
    exit 1
    ;;
esac

empty_jetbrains_home=$work/empty-jetbrains-home
mkdir -p "$empty_jetbrains_home"
set +e
output=$(env -u CODEXBAR_BACKEND HOME="$empty_jetbrains_home" XDG_CONFIG_HOME="$empty_jetbrains_home/.config" \
  XDG_DATA_HOME="$empty_jetbrains_home/.local/share" CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider jetbrains --source auto --json 2>/dev/null)
status=$?
set -e
[ "$status" -eq 1 ]
case "$output" in
  '[{"provider":"jetbrains","source":"auto","error":{"message":"No JetBrains IDE with AI Assistant detected.'*'"code":1,"kind":"provider"}}]') ;;
  *)
    printf 'unexpected missing JetBrains IDE output: %s\n' "$output" >&2
    exit 1
    ;;
esac

set +e
output=$(env -u CODEXBAR_BACKEND HOME="$jetbrains_home" CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider jetbrains --source local --json 2>/dev/null)
status=$?
set -e
[ "$status" -eq 1 ]
[ "$output" = '[{"provider":"cli","source":"cli","error":{"message":"Error: --source must be auto|web|cli|oauth|api.","code":1,"kind":"args"}}]' ]

set +e
output=$(env -u CODEXBAR_BACKEND HOME="$jetbrains_home" CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider jetbrains --source api --json 2>/dev/null)
status=$?
set -e
[ "$status" -eq 1 ]
case "$output" in
  '[{"provider":"jetbrains","source":"api","error":{"message":"Source '\''api'\'' is not supported for jetbrains.","code":1,"kind":"provider"}}]') ;;
  *)
    printf 'unexpected unsupported JetBrains source output: %s\n' "$output" >&2
    exit 1
    ;;
esac

cp "${jetbrains_fixture%/*}/invalid-quota.xml" "$jetbrains_quota_dir/AIAssistantQuotaManager2.xml"
set +e
output=$(env -u CODEXBAR_BACKEND HOME="$jetbrains_home" CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider jetbrains --source cli --json 2>/dev/null)
status=$?
set -e
[ "$status" -eq 1 ]
[ "$output" = '[{"provider":"jetbrains","source":"cli","error":{"message":"Could not parse JetBrains AI quota: Invalid JSON format","code":1,"kind":"provider"}}]' ]

output=$(env -u CODEXBAR_BACKEND -u KIMI_CODE_API_KEY CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider kimi --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"kimi","source":"api","error":{"message":"Kimi Code API key is missing.'*'"code":1,"kind":"provider"}}]') ;;
  *)
    printf 'unexpected native Kimi missing-key output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(env -u CODEXBAR_BACKEND CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider kimi --source web --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"kimi","source":"web","error":{"message":"Kimi source '\''web'\'' has no native Linux implementation yet"'*'"code":1,"kind":"provider"}}]') ;;
  *)
    printf 'unexpected native Kimi web-source output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(env -u CODEXBAR_BACKEND CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider kimi-k2 --json 2>/dev/null || true)
[ "$output" = '[{"provider":"cli","source":"cli","error":{"message":"Unknown provider: kimi-k2","code":1,"kind":"args"}}]' ]

output=$(env -u CODEXBAR_BACKEND -u CLAWROUTER_API_KEY CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider claw-router --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"clawrouter","source":"api","error":{"message":"Missing ClawRouter API key."'*'"code":1,"kind":"provider"}}]') ;;
  *)
    printf 'unexpected native ClawRouter missing-key output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(env -u CODEXBAR_BACKEND -u LLM_PROXY_API_KEY -u LLM_PROXY_BASE_URL CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider llm-proxy --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"llmproxy","source":"api","error":{"message":"Missing LLM Proxy API key."'*'"code":1,"kind":"provider"}}]') ;;
  *)
    printf 'unexpected native LLM Proxy missing-key output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(env -u CODEXBAR_BACKEND -u CODEBUFF_API_KEY CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider manicode --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"codebuff","source":"api","error":{"message":"Codebuff API token is not configured."'*'"code":1,"kind":"provider"}}]') ;;
  *)
    printf 'unexpected native Codebuff missing-key output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(env -u CODEXBAR_BACKEND HOME="$work/empty-opencode-go-home" CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider opencodego --source auto --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"opencodego","source":"auto","error":{"message":"OpenCode Go not detected. Log in with OpenCode Go or use it locally first.","code":1,"kind":"provider"}}]') ;;
  *)
    printf 'unexpected native OpenCode Go detection output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(env -u CODEXBAR_BACKEND HOME="$work/empty-opencode-go-home" CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider opencodego --source web --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"opencodego","source":"web","error":{"message":"OpenCode Go source '\''web'\'' has no native Linux implementation yet","code":1,"kind":"provider"}}]') ;;
  *)
    printf 'unexpected native OpenCode Go web-source output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(env -u CODEXBAR_BACKEND -u AZURE_OPENAI_API_KEY -u AZURE_OPENAI_ENDPOINT \
  -u AZURE_OPENAI_DEPLOYMENT_NAME -u AZURE_OPENAI_API_VERSION CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider aoai --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"azureopenai","source":"auto","error":{"message":"Azure OpenAI API key not configured.'*'"code":1,"kind":"provider"}}]') ;;
  *)
    printf 'unexpected native Azure OpenAI missing-key output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(env -u CODEXBAR_BACKEND CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider azure-openai --source web --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"azureopenai","source":"web","error":{"message":"Source '\''web'\'' is not supported for azure-openai.","code":1,"kind":"provider"}}]') ;;
  *)
    printf 'unexpected native Azure OpenAI web-source output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(env -u CODEXBAR_BACKEND -u NEURALWATT_API_KEY -u NEURALWATT_API_URL CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider nw --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"neuralwatt","source":"api","error":{"message":"Missing Neuralwatt API key. Set apiKey in the CodexBar config file or NEURALWATT_API_KEY.","code":1,"kind":"provider"}}]') ;;
  *)
    printf 'unexpected native Neuralwatt missing-key output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(env -u CODEXBAR_BACKEND CODEXBAR_CONFIG="$config" \
  "$binary" usage --provider neural --source web --json 2>/dev/null || true)
case "$output" in
  '[{"provider":"neuralwatt","source":"web","error":{"message":"Source '\''web'\'' is not supported for neuralwatt.","code":1,"kind":"provider"}}]') ;;
  *)
    printf 'unexpected native Neuralwatt web-source output: %s\n' "$output" >&2
    exit 1
    ;;
esac
