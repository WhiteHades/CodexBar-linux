# Architecture

CodexBar Linux is one C23 process with several presentation modes over a shared provider/runtime core.

## Data flow

1. `config.c` loads and normalizes the private JSON configuration.
2. `provider_registry.c` resolves stable IDs, aliases, capabilities, source modes, dashboards, and status endpoints.
3. `backend.c` plans account-scoped provider requests and invokes the native provider implementation.
4. `runtime.c` reconciles configuration revisions, fetches status concurrently, preserves last-good status, attaches pace,
   records history, and emits account-scoped transitions, hooks, and notifications.
5. `model.c` owns the canonical in-memory snapshot. `render.c` produces text, JSON, and Waybar projections.
6. `tui.c`, `desktop.c`, and `serve.c` expose the snapshot through ncurses, StatusNotifierItem, and HTTP surfaces.

## Boundaries

- Provider files parse remote or local data into `CodexBarProvider`; presentation code does not know provider protocols.
- All HTTP uses `http.c`, which enforces protocol, redirect, timeout, cancellation, and response-size policy.
- All child processes use `process.c`, which owns deadlines, process groups, output bounds, and reaping.
- Account secrets remain in private config/token stores and are never serialized by normal renderers.
- Provider identity, plan, quota, balance, and cost fields are never borrowed from a different provider.

## Persistence

- Configuration: `${XDG_CONFIG_HOME:-~/.config}/codexbar/config.json`, mode `0600`.
- Utilization history: `${XDG_STATE_HOME:-~/.local/state}/codexbar/history` unless overridden.
- Provider-owned credentials and local databases are read in place; CodexBar does not rewrite them.

## Refresh model

`refresh_coordinator.c` coalesces overlapping work. `refresh_policy.c` calculates bounded adaptive intervals. Runtime state
is invalidated when the loaded config digest changes, preventing stale work from publishing into a new configuration.
Status polling is best-effort and does not replace successful usage with a transient status failure.
