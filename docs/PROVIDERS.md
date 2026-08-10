# Providers

The registry currently exposes 69 stable native providers:

`codex`, `openai`, `azureopenai`, `claude`, `clinepass`, `cursor`, `opencode`, `opencodego`, `alibaba`,
`alibabatokenplan`, `qwencloud`, `factory`, `fireworks`, `gemini`, `antigravity`, `copilot`, `devin`, `zai`, `minimax`, `manus`,
`kimi`, `kilo`, `kiro`, `vertexai`, `augment`, `jetbrains`, `moonshot`, `amp`, `t3chat`, `ollama`, `synthetic`,
`warp`, `openrouter`, `elevenlabs`, `windsurf`, `zed`, `perplexity`, `mimo`, `doubao`, `sakana`, `abacus`,
`mistral`, `deepseek`, `deepinfra`, `codebuff`, `crof`, `venice`, `commandcode`, `qoder`, `stepfun`, `bedrock`,
`grok`, `groq`, `llmproxy`, `litellm`, `deepgram`, `poe`, `chutes`, `neuralwatt`, `clawrouter`, `longcat`,
`sub2api`, `wayfinder`, `zenmux`, `aiand`, `zoommate`, `xai`, `notion`, and `ibmbob`.

Run `codexbar-linux config providers` for enabled state and display names. Run `codexbar-linux diagnose --provider ID
--format json` for capability and safe configuration diagnostics.

## Native contracts

- Every registry provider has a native fetch/parser path; no compatibility subprocess delegates usage to another
  CodexBar implementation.
- Source modes are declared centrally and explicit modes are strict.
- API endpoints are validated and bounded by central HTTP policy.
- Local providers use bounded database, file, procfs, or child-process readers.
- Multi-account providers preserve stable account labels and ownership in output, history, warnings, and refresh state.
- Service status is polled only for descriptors with an explicit supported status source.

## IBM Bob

Enable `ibmbob`, then store its API key with `config set-api-key --provider ibmbob --stdin` or set
`BOBSHELL_API_KEY`. IBM Bob aggregates monthly Bobcoin usage across every usable team visible to the key. Team details
remain separate in JSON output, and teams without a finite budget make the aggregate percentage unknown.

## Notion AI

Enable `notion`, set `cookieSource` to `manual`, and configure `cookieHeader` with a bare `token_v2`, a `Cookie` header
containing `token_v2`, or a full browser Copy as cURL capture. Linux does not import Notion browser cookies and does
not read a Notion environment variable. Set the optional `workspaceID` to a dashed or undashed workspace UUID; an
unknown value falls back to automatic selection.

Each refresh resolves the signed-in user and visible workspaces through `POST /api/v3/getSpaces`, then reads the
selected workspace through `POST /api/v3/getCreditRateLimitStatus`. The primary bar is Notion's rolling window and the
secondary bar is the calendar billing month. Usage above the allowance remains visible in raw output. Free and Plus
workspaces return a clear error because Notion reports allowances only for Business and Enterprise workspaces. The
registry exposes <https://status.notion.so> as metadata but does not poll it.

## Adding a provider

Add the descriptor and native enum, implement the provider fetcher, route it in `backend.c`, add focused tests, update
the config normalization list, and update the provider count/list here. Provider-specific configuration must be
preserved by normalization and redacted by diagnostic output.
