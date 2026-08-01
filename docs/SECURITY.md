# Security

## Credential handling

- Configuration is created with mode `0600` and updated atomically.
- Secrets are accepted through stdin or provider-owned stores; diagnostic and normal renderers redact them.
- Provider credentials are never shared across providers or accounts.
- Browser cookies are opt-in provider inputs and are sent only to the declared HTTPS origin.

## Network policy

- Remote provider endpoints require HTTPS unless a provider explicitly supports a validated loopback/private endpoint.
- Redirects are denied or restricted to the same origin.
- Requests have deadlines, cancellation, and maximum response sizes.
- Dashboard service binds to loopback by default. Non-loopback binding requires a bearer token and explicit plain-HTTP
  acknowledgement; place it behind TLS for any non-local use.

## Local inputs

- JSONL, JSON, XML, SQLite, procfs, and child-process output are bounded and treated as untrusted.
- Child processes run with deadlines and isolated process groups and are fully reaped.
- Paths are canonicalized where identity or project attribution depends on them.
- History and transition state are keyed by stable account ownership to prevent cross-account publication.

## Reporting

Do not open a public issue containing tokens, cookies, account identifiers, private endpoints, or raw config files.
Use the repository’s private security-reporting channel. Include the version, provider, selected source mode, redacted
diagnostic output, and deterministic reproduction when possible.
