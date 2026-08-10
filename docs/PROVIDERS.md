# Providers

The registry currently exposes 67 stable native providers:

`codex`, `openai`, `azureopenai`, `claude`, `clinepass`, `cursor`, `opencode`, `opencodego`, `alibaba`,
`alibabatokenplan`, `qwencloud`, `factory`, `fireworks`, `gemini`, `antigravity`, `copilot`, `devin`, `zai`, `minimax`, `manus`,
`kimi`, `kilo`, `kiro`, `vertexai`, `augment`, `jetbrains`, `moonshot`, `amp`, `t3chat`, `ollama`, `synthetic`,
`warp`, `openrouter`, `elevenlabs`, `windsurf`, `zed`, `perplexity`, `mimo`, `doubao`, `sakana`, `abacus`,
`mistral`, `deepseek`, `deepinfra`, `codebuff`, `crof`, `venice`, `commandcode`, `qoder`, `stepfun`, `bedrock`,
`grok`, `groq`, `llmproxy`, `litellm`, `deepgram`, `poe`, `chutes`, `neuralwatt`, `clawrouter`, `longcat`,
`sub2api`, `wayfinder`, `zenmux`, `aiand`, `zoommate`, and `xai`.

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

## Adding a provider

Add the descriptor and native enum, implement the provider fetcher, route it in `backend.c`, add focused tests, update
the config normalization list, and update the provider count/list here. Provider-specific configuration must be
preserved by normalization and redacted by diagnostic output.
