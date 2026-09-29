# SharedWorld SML mod

C++ SML mod (no cooked assets). Drop `SharedWorld/` into the `Mods/` folder
of an SML 3.12 development project and package with Alpakit.

| File | Responsibility |
|---|---|
| `SharedWorldSubsystem` | Game-instance coordinator: ticks sessions, reacts to host/join/migrate states |
| `SharedWorldHostController` | Loads the verified save, publishes the session id, checkpoints, final upload |
| `SharedWorldJoinManager` | Joins via `ResolveOnlineSession` + `UCommonSessionStatics::JoinSession` |
| `SharedWorldPanel` / `UI/` | Main-menu Shared Worlds browser and session UI (C++ UMG) |
| `SharedWorldRootModules` | Native SML root menu/game world modules |
| `SharedWorldChatCommand` | `/sharedworld status|save|stop|…` |
| `SharedWorldCore/` | Pure C++20 engine (lease, sync, GitHub, migration) |

See `docs/` and `STATUS.md` in the repo root for architecture and status.
