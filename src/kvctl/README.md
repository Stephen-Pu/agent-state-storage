# kvctl

Operator CLI. LLD §8.3.

Subcommands (MVP):
- `kvctl inspect <prefix>`        — show tier residency, refcount, owner node.
- `kvctl tier-stats [--node N]`   — per-tier capacity / hit-rate.
- `kvctl quota <tenant>`          — show / set tenant quota.
- `kvctl trace <request-id>`      — request-level trace (Tier-3 metrics).
- `kvctl members`                 — cluster membership view.
- `kvctl drain <node>`            — initiate graceful drain.
