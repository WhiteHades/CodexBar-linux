#!/bin/sh

set -eu

binary=$1
fake_backend=$2
base=$PWD/.tmp/diagnose-cli
mkdir -p "$base"
work=$(mktemp -d "$base/run.XXXXXX")
trap 'rm -rf "$work"; rmdir "$base" 2>/dev/null || true' EXIT
export HOME="$work/home"
export XDG_CONFIG_HOME="$work/config"
export XDG_DATA_HOME="$work/data"
export XDG_CACHE_HOME="$work/cache"
mkdir -p "$HOME" "$XDG_CONFIG_HOME"

output=$(CODEXBAR_BACKEND="$fake_backend" "$binary" diagnose --provider codex --format json)
printf '%s\n' "$output" | grep -q '"schemaVersion":"1.0"'
printf '%s\n' "$output" | grep -q '"platform":"Linux"'
printf '%s\n' "$output" | grep -q '"provider":"codex"'
printf '%s\n' "$output" | grep -q '"source":"oauth"'
printf '%s\n' "$output" | grep -q '"dataConfidence":"unknown"'
printf '%s\n' "$output" | grep -q '"label":"primary"'
if printf '%s\n' "$output" | grep -q 'dev@example.test\|Resets Friday\|plan expires'; then
    printf 'diagnostic leaked usage metadata\n' >&2
    exit 1
fi

output=$(CODEXBAR_BACKEND="$fake_backend" "$binary" diagnose --provider both --format json)
printf '%s\n' "$output" | grep -q '"diagnostics":\['
printf '%s\n' "$output" | grep -q '"provider":"codex"'
printf '%s\n' "$output" | grep -q '"provider":"claude"'

sparse_backend=$work/sparse-backend.sh
cat >"$sparse_backend" <<'EOF'
#!/bin/sh
printf '%s\n' \
  '[{"provider":"clinepass","source":"api","usage":{"primary":null,'\
'"secondary":{"usedPercent":40,"windowMinutes":10080},"tertiary":null,'\
'"updatedAt":"2026-01-01T00:00:00Z"}}]'
EOF
chmod +x "$sparse_backend"
output=$(CODEXBAR_BACKEND="$sparse_backend" "$binary" diagnose --provider clinepass --format json)
printf '%s\n' "$output" | grep -q '"label":"secondary"'
if printf '%s\n' "$output" | grep -q '"label":"primary"'; then
    printf 'sparse diagnostic mislabeled secondary window\n' >&2
    exit 1
fi

confidence_backend=$work/confidence-backend.sh
cat >"$confidence_backend" <<'EOF'
#!/bin/sh
printf '%s\n' \
  '[{"provider":"deepinfra","source":"api","usage":{"primary":{"usedPercent":0},'\
'"secondary":null,"tertiary":null,"updatedAt":"2026-01-01T00:00:00Z",'\
'"dataConfidence":"exact"}}]'
EOF
chmod +x "$confidence_backend"
output=$(CODEXBAR_BACKEND="$confidence_backend" "$binary" diagnose --provider deepinfra --format json)
printf '%s\n' "$output" | grep -q '"dataConfidence":"exact"'

sed 's/"exact"/"percentOnly"/' "$confidence_backend" >"$work/percent-only-backend.sh"
chmod +x "$work/percent-only-backend.sh"
output=$(CODEXBAR_BACKEND="$work/percent-only-backend.sh" \
  "$binary" diagnose --provider deepinfra --format json)
printf '%s\n' "$output" | grep -q '"dataConfidence":"percentOnly"'

credential_backend=$work/credential-backend.sh
cat >"$credential_backend" <<'EOF'
#!/bin/sh
printf '%s\n' '[{"provider":"deepinfra","source":"api","error":{"message":"API key rejected","code":1,"kind":"provider"}}]'
EOF
chmod +x "$credential_backend"
output=$(env "DEEPINFRA_API_KEY=invalid$(printf '\177')key" CODEXBAR_BACKEND="$credential_backend" \
  "$binary" diagnose --provider deepinfra --format json)
printf '%s\n' "$output" | grep -q '"configured":false'

sed 's/deepinfra/neuralwatt/' "$credential_backend" >"$work/neuralwatt-credential-backend.sh"
chmod +x "$work/neuralwatt-credential-backend.sh"
output=$(env "NEURALWATT_API_KEY=$(printf '\342\200\203')" \
  CODEXBAR_BACKEND="$work/neuralwatt-credential-backend.sh" \
  "$binary" diagnose --provider neural --format json)
printf '%s\n' "$output" | grep -q '"configured":false'

neuralwatt_backend=$work/neuralwatt-backend.sh
cat >"$neuralwatt_backend" <<'EOF'
#!/bin/sh
printf '%s\n' '[{"provider":"neuralwatt","source":"api","usage":{"primary":{"usedPercent":25,"resetDescription":"2.50 / 10 kWh"},"secondary":null,"tertiary":null,"extraRateWindows":[{"id":"key-allowance","title":"Key Monthly","window":{"usedPercent":25}}],"providerCost":{"used":5,"limit":0,"currencyCode":"USD","period":"Neuralwatt prepaid balance"},"updatedAt":"2026-01-01T00:00:00Z","dataConfidence":"exact"}}]'
EOF
chmod +x "$neuralwatt_backend"
output=$(CODEXBAR_BACKEND="$neuralwatt_backend" "$binary" diagnose --provider nw --format json)
printf '%s\n' "$output" | grep -q '"label":"primary"[^}]*"hasResetDescription":true'
printf '%s\n' "$output" | grep -q '"label":"Key allowance"'
printf '%s\n' "$output" | grep -q '"extraWindowCount":1'

sed 's/deepinfra/aiand/' "$credential_backend" >"$work/aiand-credential-backend.sh"
chmod +x "$work/aiand-credential-backend.sh"
output=$(env "AIAND_API_KEY=$(printf '\342\200\203')" CODEXBAR_BACKEND="$work/aiand-credential-backend.sh" \
  "$binary" diagnose --provider aiand --format json)
printf '%s\n' "$output" | grep -q '"configured":false'

output=$(CODEXBAR_BACKEND="$fake_backend" "$binary" diagnose --provider antigravity --format json)
printf '%s\n' "$output" | grep -q '"provider":"antigravity"'
printf '%s\n' "$output" | grep -q '"source":"failed"'
printf '%s\n' "$output" | grep -q '"safeDescription"'

printf 'dummy-secret\n' | "$binary" config set-api-key --provider openrouter --stdin >/dev/null
hostile=$work/hostile-backend.sh
cat >"$hostile" <<'EOF'
#!/bin/sh
if [ "${DIAGNOSE_FAILURE:-}" = 1 ]; then
    printf '%s\n' '[{"provider":"openrouter","source":"api","error":{"message":"connection refused token dummy-secret","code":1,"kind":"runtime"}}]'
else
    printf '%s\n' '[{"provider":"openrouter","account":"dummy-secret","plan":"dummy-secret","source":"api-dummy-secret","note":"dummy-secret","usage":{"primary":{"label":"dummy-secret","usedPercent":25,"resetDescription":"dummy-secret"},"extraRateWindows":[{"id":"dummy-secret","title":"dummy-secret","window":{"usedPercent":5},"usageKnown":true}],"updatedAt":"2026-01-01T00:00:00Z"}}]'
fi
EOF
chmod +x "$hostile"

output=$(CODEXBAR_BACKEND="$hostile" "$binary" diagnose --provider openrouter --format json --redact)
printf '%s\n' "$output" | grep -q '"configured":true'
printf '%s\n' "$output" | grep -q '"modes":\["api"\]'
if printf '%s\n' "$output" | grep -q 'dummy-secret'; then
    printf 'diagnostic leaked hostile data or credentials\n' >&2
    exit 1
fi

output=$(DIAGNOSE_FAILURE=1 CODEXBAR_BACKEND="$hostile" \
  "$binary" diagnose --provider openrouter --format json)
printf '%s\n' "$output" | grep -q '"category":"network"'
printf '%s\n' "$output" | grep -q '"safeDescription":"Network error - check your connection"'
if printf '%s\n' "$output" | grep -q 'dummy-secret\|connection refused'; then
    printf 'diagnostic leaked raw error text\n' >&2
    exit 1
fi

file_stdout=$(CODEXBAR_BACKEND="$fake_backend" "$binary" diagnose --provider codex --format json --pretty \
  --output "$work/export/nested/diagnostic.json")
[ -z "$file_stdout" ]
[ -s "$work/export/nested/diagnostic.json" ]

if "$binary" diagnose --provider codex >"$work/output" 2>"$work/error"; then
    printf 'missing diagnose format unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: only JSON format is supported for diagnose' ]

if "$binary" diagnose --provider missing --format json >"$work/output" 2>"$work/error"; then
    printf 'unknown diagnose provider unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = "Error: unknown provider 'missing'" ]
