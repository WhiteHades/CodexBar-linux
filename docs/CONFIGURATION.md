# Configuration

Use the CLI for ordinary configuration. It validates providers, preserves provider-specific fields, writes atomically,
rejects concurrent changes, and keeps the file private. `config providers` lists all 69 registered providers.

```sh
codexbar-linux config validate
codexbar-linux config dump --pretty
codexbar-linux config providers
codexbar-linux config enable --provider claude
codexbar-linux config disable --provider codex
printf '%s\n' "$PROVIDER_API_KEY" | \
  codexbar-linux config set-api-key --provider openrouter --stdin
```

The default file is `${XDG_CONFIG_HOME:-~/.config}/codexbar/config.json`. Override it with `CODEXBAR_CONFIG` for an
isolated profile or test. A minimal document is:

```json
{
  "version": 1,
  "refreshFrequency": "adaptive",
  "providers": [
    {"id": "codex", "enabled": true},
    {"id": "openrouter", "enabled": true}
  ]
}
```

Common provider fields are `id`, `enabled`, `source`, `apiKey`, `accountSlug`, `endpoint`, `organizationID`,
`workspaceID`, `usageScope`, `cookieSource`, `cookieHeader`, and `tokenAccounts`. The accepted fields depend on provider capabilities; `config
validate` rejects unsupported source/key combinations.

Fireworks requires both an API key and account slug. Set `apiKey` and `accountSlug` on its provider entry, or use
`FIREWORKS_API_KEY` (or `FIREWORKS_KEY`) with `FIREWORKS_ACCOUNT_SLUG`.

IBM Bob accepts `apiKey` or `BOBSHELL_API_KEY`. It also supports token accounts for keeping multiple Bob API keys
separate.

Notion AI accepts manual cookies only. Enable it with a private provider entry such as:

```json
{
  "id": "notion",
  "enabled": true,
  "source": "web",
  "cookieSource": "manual",
  "cookieHeader": "token_v2=your-session-cookie",
  "workspaceID": "optional-workspace-uuid"
}
```

`cookieHeader` can contain the bare `token_v2` value, a `Cookie` header containing `token_v2`, or a full browser Copy
as cURL capture. Notion does not import browser cookies automatically and does not read a provider environment
variable. If `workspaceID` is absent or unknown, the provider selects the first Business or Enterprise workspace, then
the first visible workspace.

## Sources

`auto` follows the provider registry’s current priority and falls back only across declared recoverable paths. Explicit
`api`, `web`, `cli`, or `oauth` sources are strict: they do not silently use another credential source. Run
`codexbar-linux diagnose --provider ID --format json` to see the selected source and safe configuration shape.

## Multiple accounts

Providers that support token accounts accept an `accounts` list with stable IDs, labels, and provider-specific
credentials. Select accounts at read time:

```sh
codexbar-linux usage --provider zai --account Team
codexbar-linux usage --provider zai --account-index 2
codexbar-linux usage --provider zai --all-accounts --format json
```

Codex managed accounts use `codexbar-linux codex-accounts`. Account credentials, refresh state, histories, warnings,
and last-good values are isolated by stable account ownership.

## Environment

Provider-standard environment variables are supported where the provider declares them. Useful global overrides:

- `CODEXBAR_CONFIG`: config path.
- `CODEXBAR_HISTORY_DIR`: utilization history directory.
- `CODEXBAR_THEME`: `mocha`, `dark`, or `system`.
- `CODEXBAR_DISABLE_STATUS`: disable public service-status requests.
- `CODEXBAR_DISABLE_HISTORY`: disable history writes.
- `CODEXBAR_DASHBOARD_TOKEN`: bearer token for the dashboard server.

Never put secrets in command-line arguments or committed files. Prefer stdin or the provider’s established credential
store.
