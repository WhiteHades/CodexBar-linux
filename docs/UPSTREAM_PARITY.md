# Upstream parity ledger

`audit/upstream-parity.tsv` records the inclusive upstream range from
`77fc086b8e7e546274ebdfc347e02efcae973148` through `bff43f8a2cc736a9dd248f6fb275f594b3a472c7`.
Every commit in that reachable range has one row and one disposition:

- `ported`: implemented and tested in the native Linux code.
- `already-covered`: the native Linux code already had equivalent behavior.
- `non-linux`: the change applies only to an upstream platform or release process.
- `superseded`: a later upstream commit replaces the behavior before it needs a Linux port.
- `pending`: not reviewed to a final disposition or still requires work.

The finding column groups actionable work discovered during review. `unreviewed` means no commit-level review has been
recorded. The evidence column must identify native files/tests, the platform-specific reason, or the superseding
upstream commit before a row can use a completed disposition. Pending rows use `-` until that evidence exists.
Findings whose source commits were already ancestors of the merge boundary are grouped on the boundary row; this keeps
the supplied review work visible without adding hashes outside the validated range.

Run the local invariant check with:

```sh
./Scripts/check-upstream-parity.sh
```

The check reconstructs the range from Git, rejects malformed or duplicate rows, reports missing and out-of-range
hashes, and prints disposition totals including the pending count. Use `--require-complete` when a workflow must reject
any pending commit. `make check` runs the invariant check and mutation tests without requiring the audit to be complete.

Do not update `UPSTREAM_REVISION` to the target until every pending row has a reviewed disposition and the applicable
native checks pass. Do not copy upstream Swift sources into this repository.
