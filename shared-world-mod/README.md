# SharedWorld SML mod

C++ SML mod (no cooked assets). Drop `SharedWorld/` into the `Mods/` folder
of an SML 3.12 development project and package with Alpakit.

| Area | Responsibility |
|---|---|
| `SharedWorldSubsystem` | Session tick, host/join/migrate, GitHub auth |
| `SharedWorldHostController` | Load save, publish session, checkpoints, upload |
| `SharedWorldJoinManager` | Join via OnlineIntegration session APIs |
| `UI/` | Main-menu Shared Worlds browser and session UI |
| `SharedWorldChatCommand` | `/sharedworld …` host commands |
| `SharedWorldCore/` | Pure C++20 engine (lease, sync, GitHub, migration) |

See repo-root `docs/` and `STATUS.md` for architecture and status.
