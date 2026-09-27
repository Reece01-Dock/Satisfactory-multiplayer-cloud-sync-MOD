# Satisfactory Shared World

Successor to the batch-file cloud sync on the `Page` branch. One button in
the main menu — **Play Shared World** — and the software decides:

```
friend already hosting  →  join them (nothing is downloaded)
nobody hosting          →  claim the world, download + verify the latest save, host it
```

Built for save safety first: an expiring host lease with fencing tokens,
compare-and-swap metadata, immutable revisioned saves, SHA-256 + full
save-structure verification, and backups whenever anything would be
replaced. A client holding revision N can never overwrite revision N+1.

| Directory | What |
|---|---|
| `shared-world-mod/` | SML (C++) mod: main-menu panel, host/join driving, game lifecycle |
| `shared-world-helper/` | Local Go companion: lease, sync, cloud storage, authenticated localhost API |
| `docs/research.md` | Findings: old workflow, SML/game APIs, save format, join paths |
| `docs/architecture.md` | Design, data model, state machines, failure matrix |
| `docs/ipc-protocol.md` | Mod ⇄ helper API |
| `docs/setup.md` | Build and configure |
| `STATUS.md` | What works, what is verified, blockers, next steps |

**Status: development preview.** The helper is implemented and tested;
the mod is written against verified game headers but has not been compiled
or run in game yet, and no real cloud provider exists yet. Read
`STATUS.md` before using it.

The legacy website and batch scripts (`index.html`, `Downloads/`, …) are
kept unchanged for reference.

The original project is by Vojtak42; this repository is licensed under the
terms in `LICENSE`.
