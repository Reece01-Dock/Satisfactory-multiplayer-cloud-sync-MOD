# SharedWorld SML mod

C++ SML mod (no cooked assets). Drop `SharedWorld/` into the `Mods/` folder
of an SML 3.12 development project and package with Alpakit.

| File | Responsibility |
|---|---|
| `SharedWorldSubsystem` | Game-instance coordinator: polls the helper, reacts to session states, keepalives |
| `SharedWorldIPCClient` | Discovery file, auto-launch of the bundled helper, authenticated HTTP |
| `SharedWorldHostController` | Loads the verified save, publishes the session id, `SaveGame` → report, checkpoints, final upload |
| `SharedWorldJoinManager` | Joins via `ResolveOnlineSession` + `UCommonSessionStatics::JoinSession` (or `RawAddress`) |
| `SharedWorldPanel` | Main-menu "Shared Worlds" panel, built in C++ UMG |
| `SharedWorldRootModules` | Native SML root menu/game world modules |
| `SharedWorldChatCommand` | `/sharedworld status|save|stop` (host only for save/stop) |
| `SharedWorldTypes` | Mirrors of the helper's IPC JSON |

Every game/SML API used was checked against the SML `dev` headers
(commit `ae723cf`); see `docs/research.md`. The mod has **not** been
compiled or run in this repository's CI (no Unreal toolchain); see `STATUS.md`.
