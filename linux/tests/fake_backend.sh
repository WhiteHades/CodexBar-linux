#!/bin/sh

cat <<'EOF'
[{"provider":"codex","account":"dev@example.com","plan":"Pro","source":"oauth","usage":{"primary":{"usedPercent":28,"resetDescription":"resets Thu, Jul 23 at 10:16"},"secondary":{"usedPercent":71.4,"resetDescription":"resets Fri, Jul 24 at 08:00"},"tertiary":null},"credits":{"remaining":12.5}},{"provider":"claude","source":"cli","usage":{"primary":{"usedPercent":91}},"error":null}]
EOF
