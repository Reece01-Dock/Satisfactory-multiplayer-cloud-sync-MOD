# Satisfactory Shared World

One button in the main menu — **Play Shared World** — and the mod decides:

```
friend already hosting  →  join them (nothing is downloaded)
nobody hosting          →  claim the world, download + verify the latest save, host it
host crashed / left     →  the next player takes over from the last verified revision
```

Everything runs inside the mod: no helper program, no local server, no
command line. Friends join through the game's own Steam/Epic sessions;
the world's saves and host lock live in a GitHub repository (or a shared
folder).

Built for save safety first: an expiring host lease with fencing
generations, compare-and-swap commits, immutable content-addressed saves,
SHA-256 + full save-structure verification, and backups whenever anything
would be replaced. A client holding revision N can never overwrite
revision N+1. Divergent saves are never merged; restoring history creates
a new revision.

| Directory | What |
|---|---|
| `shared-world-mod/SharedWorld/Source/SharedWorldCore/` | The engine, pure C++20 (no UE headers) |
| `shared-world-mod/SharedWorld/Source/SharedWorld/` | UE/SML module: adapters, subsystem, menu UI, chat commands |
| `shared-world-mod/core-tests/` | Native CMake build + core tests |
| `docs/` | Setup, architecture, testing, UI notes |
| `STATUS.md` | What works, what is verified, what still needs the game |

**Status: development preview.** The core is implemented and tested; the
Unreal layer is written but has not been fully validated in game yet.
Read `STATUS.md` before using it.

The original project is by Vojtak42; this repository is licensed under the
terms in `LICENSE`.
