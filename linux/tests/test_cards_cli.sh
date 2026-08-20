#!/bin/sh

set -eu

binary=$1
backend=$2
work=$(mktemp -d "$PWD/codexbar-cards-cli.XXXXXX")
trap 'rm -rf "$work"' EXIT

output=$(COLUMNS=80 CODEXBAR_BACKEND="$backend" "$binary" cards)
case "$output" in
  *'Codex [oauth] PLAN Pro'*'Claude [cli]'*'@ dev@example.test'*'Session'*'72% left'*'Balance: 12.50 left'*) ;;
  *)
    printf 'unexpected card output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_BACKEND="$backend" "$binary" cards --provider codex --brief)
case "$output" in
  'codexbar AI Usage & Limits'*'Codex'*'session 28% used'*'28 / 100 request'*) ;;
  *)
    printf 'unexpected brief output: %s\n' "$output" >&2
    exit 1
    ;;
esac

output=$(CODEXBAR_BACKEND="$backend" "$binary" cards --provider codex --no-credits)
case "$output" in
  *Balance*)
    printf 'credits were not suppressed\n' >&2
    exit 1
    ;;
esac

binding_backend=$work/binding-backend.sh
cat >"$binding_backend" <<'EOF'
#!/bin/sh
printf '%s\n' '[{"provider":"claude","source":"oauth","usage":{"primary":{"label":"Session","usedPercent":40,"windowMinutes":300,"resetsAt":"2030-01-01T00:00:00Z"},"secondary":{"label":"Weekly","usedPercent":100,"windowMinutes":10080,"resetsAt":"2030-02-01T00:00:00Z"}}}]'
EOF
chmod +x "$binding_backend"
output=$(CODEXBAR_BACKEND="$binding_backend" "$binary" cards --provider claude)
case "$output" in
  *Session*'0% left'*Weekly*'0% left'*) ;;
  *)
    printf 'binding quota was not projected in card output: %s\n' "$output" >&2
    exit 1
    ;;
esac

aiand_backend=$work/aiand-backend.sh
cat >"$aiand_backend" <<'EOF'
#!/bin/sh
printf '%s\n' '[{"provider":"aiand","source":"api","usage":{"primary":null,"secondary":null,"tertiary":null,"providerCost":{"used":8.12344,"limit":0,"currencyCode":"JPY","period":"Last 30 days"},"updatedAt":"2026-01-01T00:00:00Z","dataConfidence":"exact"}},{"provider":"deepinfra","source":"api","usage":{"primary":{"usedPercent":0},"providerCost":{"used":3.94,"limit":20,"currencyCode":"USD","period":"Billing cycle"},"updatedAt":"2026-01-01T00:00:00Z"}},{"provider":"neuralwatt","source":"api","usage":{"primary":{"usedPercent":25,"resetDescription":"2.50 / 10 kWh"},"secondary":null,"tertiary":null,"identity":{"providerID":"neuralwatt","loginMethod":"Pro plan"},"providerCost":{"used":0,"limit":0,"currencyCode":"USD","period":"Neuralwatt prepaid balance"},"updatedAt":"2026-01-01T00:00:00Z","dataConfidence":"exact"}}]'
EOF
chmod +x "$aiand_backend"
output=$(CODEXBAR_BACKEND="$aiand_backend" "$binary" cards --provider aiand)
printf '%s\n' "$output" | grep -q 'API spend: 8.12 JPY · Last 30 days'
output=$(CODEXBAR_BACKEND="$aiand_backend" "$binary" cards --provider aiand --brief)
printf '%s\n' "$output" | grep -q 'API spend: 8.12 JPY · Last 30 days'
output=$(CODEXBAR_BACKEND="$aiand_backend" "$binary" cards --provider deepinfra)
printf '%s\n' "$output" | grep -q 'Extra usage: 3.94 USD / 20.00 USD'
output=$(CODEXBAR_BACKEND="$aiand_backend" "$binary" cards --provider neural)
printf '%s\n' "$output" | grep -q 'Pay-as-you-go: Balance: 0.00 USD'
printf '%s\n' "$output" | grep -q 'PLAN Pro plan'
if printf '%s\n' "$output" | grep -q 'API spend'; then
    printf 'Neuralwatt prepaid balance was rendered as spend\n' >&2
    exit 1
fi

if CODEXBAR_BACKEND="$backend" "$binary" cards --provider unknown >"$work/output" 2>"$work/error"; then
    printf 'unknown provider unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: Unknown provider: unknown' ]

output=$(CODEXBAR_BACKEND="$backend" "$binary" cards --provider codex --account first)
case "$output" in
  *'Codex [oauth]'*'@ dev@example.test'*) ;;
  *)
    printf 'account selection was not forwarded to the backend: %s\n' "$output" >&2
    exit 1
    ;;
esac

if CODEXBAR_BACKEND="$backend" "$binary" cards --provider both --all-accounts >"$work/output" 2>"$work/error"; then
    printf 'multi-provider account selection unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: Account selection requires a single provider.' ]

if CODEXBAR_BACKEND="$backend" "$binary" cards --provider codex --account-index 0 >"$work/output" 2>"$work/error"; then
    printf 'zero account index unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: --account-index must be a positive integer.' ]

if CODEXBAR_BACKEND="$backend" "$binary" cards --source local >"$work/output" 2>"$work/error"; then
    printf 'invalid source unexpectedly succeeded\n' >&2
    exit 1
fi
[ "$(cat "$work/error")" = 'Error: --source must be auto|web|cli|oauth|api.' ]
