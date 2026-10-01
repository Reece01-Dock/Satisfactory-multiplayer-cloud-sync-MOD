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
| `tools/package-smr.ps1` | After Alpakit: bump version + build `SharedWorlds.zip` for ficsit.app |

**Status: development preview.** The core is implemented and tested; the
Unreal layer compiles and packages (Shipping). In-game runtime validation
is still outstanding — see `STATUS.md`.

### Upload a new version to ficsit.app

1. Package `SharedWorld` with Alpakit (Shipping, Windows).
2. Version bump + SMR zip happen automatically if you set Alpakit’s
   **After Package** command to:

```text
powershell -NoProfile -ExecutionPolicy Bypass -File "E:\Documents\Satisfactory-multiplayer-cloud-sync-MOD\tools\after-alpakit-sharedworld.ps1"
```

   Or run manually from the repo:

```powershell
powershell -NoProfile -File tools\after-alpakit-sharedworld.ps1
```

3. Upload **`Desktop\SharedWorlds.zip`** — not `SharedWorld-Windows.zip`.

Each run bumps the patch (`1.0.0` → `1.0.1`). Use
`after-alpakit-sharedworld.ps1 -Bump Minor` (or `Major`) for larger bumps,
or `tools\package-smr.ps1 -NoBump` to rebuild the zip without changing the version.

The original project is by Vojtak42; this repository is licensed under the
terms in `LICENSE`.
