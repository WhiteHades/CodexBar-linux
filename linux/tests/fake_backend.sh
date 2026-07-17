#!/bin/sh

cat <<'EOF'
[{"provider":"codex","account":"dev@example.com","source":"oauth","usage":{"primary":{"usedPercent":28,"resetDescription":"Resets in 2h"},"secondary":{"usedPercent":71.4,"resetDescription":"Resets Friday"},"tertiary":null},"credits":{"remaining":12.5}},{"provider":"claude","source":"cli","usage":{"primary":{"usedPercent":91}},"error":null}]
EOF
