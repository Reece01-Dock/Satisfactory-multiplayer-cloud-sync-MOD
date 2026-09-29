# Testing Shared World

Headless tests exercise the **same** `SharedWorldCore` library the Satisfactory
mod links. There is no parallel “test-only” host-election or lease algorithm.

## One-command suite

```bash
cmake -S shared-world-mod/core-tests -B build -A x64          # Windows
cmake -S shared-world-mod/core-tests -B build -G Ninja        # Linux
cmake --build build --config Debug
./build/Debug/sw_tests.exe              # Windows
./build/sw_tests                        # Linux
```

Useful filters:

```bash
sw_tests Scenario          # A–L required scenarios
sw_tests Migration         # host migration + election
sw_tests Chaos --seed 42   # reproducible chaos
sw_tests Benchmark         # election cost at 4..50 players
sw_tests --list
```

Environment: `SW_TEST_SEED=482913` is equivalent to `--seed 482913`.

CI (`.github/workflows/core.yml`) runs the full headless suite on every change
under `shared-world-mod/**`. Overnight chaos:

```bash
cmake -S shared-world-mod/core-tests -B build -DSW_CHAOS_FULL=ON
cmake --build build --config Debug
./build/Debug/sw_tests.exe Chaos_Large
```

### Suite summary

At the end of a run you should see counters such as:

```
Host elections recorded: ...
Simultaneous acquisitions: ...
Host crashes exercised: ...
Migrations exercised: ...
Storage failure paths: ...
Git/CAS conflict paths: ...
Invariant checks: ...
```

## Architecture

```
SharedWorldCore          ← production algorithms
  HostElection / HostMigration / Lease / Sync / NetworkQuality
        ▲                         ▲
        │                         │
  Satisfactory mod          core-tests (simulator)
  (real net, clock,         SimulatedClock,
   storage adapters)        SimulatedNetworkQualityProvider,
                            MemoryRepository / MemoryObjectStore
```

## Scenario map

| ID | Name | Primary coverage |
|---|---|---|
| A | Normal startup | Bootstrap host + client join without acquire |
| B | Simultaneous Play | 16-way lease race → one host |
| C | Best host selection | Peer matrix + dynamic loss/jitter update |
| D | Intentional migration | Final save → handoff → generation+1 |
| E | Host crash | Lease expiry → successor → stale fenced |
| F | Old host returns | Renew/commit/release all `Fenced` |
| G | Network partition | Coordinator decides; never two hosts |
| H | Interrupted upload | Incomplete put never becomes head |
| I | Git/CAS conflict | Concurrent mutate; history intact |
| J | Corrupt save | Bad download leaves local untouched |
| K | Unsynced local progress | Stale generation cannot overwrite newer head |
| L | Steam/session join | Join path never acquires authority |

Existing `Lease_*`, `Sync_*`, `World_*`, and `HostElection_*` tests remain the
deep unit coverage behind these scenarios.

## Mod development mode

In non-shipping builds:

| Command | Effect |
|---|---|
| `/sharedworld diag` | Host, generation, revision, candidate scores, matrix |
| `/sharedworld migrate auto` | Planned migration to ranked READY successor |
| `/sharedworld dev leave` | Simulate planned host leave |
| `/sharedworld dev crash` | Simulate host connection loss |
| `/sharedworld dev latency` / `loss` / `nostorage` / `clear` | Inject scoring faults |

Production builds omit `dev` injectors.

---

## Two-instance Satisfactory integration

Headless tests cannot prove UE session join/travel. Use two game instances.

### Setup

1. Build/install the mod into two Satisfactory installs (or one install +
   `-EpicPortal` / separate user data dirs so saves and sessions do not collide).
2. Point both at the **same** Shared World storage (GitHub world or shared folder).
3. Enable logging: both clients should write
   `%LOCALAPPDATA%/SatisfactorySharedWorld/` logs; correlate with
   `generation` / `revision` / player id fields.

### Script checklist

```
[ ] A: Play Shared World → becomes host (generation N, revision R)
[ ] B: Play Shared World → JoinReady → joins A's session (no lease acquire)
[ ] Both UI show same host name, generation N, revision R
[ ] A: /sharedworld migrate auto  (or leave via stop)
[ ] Overlay: saving → preparing → transferring → reconnecting
[ ] B becomes host at generation N+1, same committed revision
[ ] A rejoins as client (or second join)
[ ] Kill A's process while hosting
[ ] Wait lease TTL (~90s) + grace; B (or preferred) takes over
[ ] Collect both logs; confirm no dual-host windows
```

Optional launch helpers (adjust paths):

```powershell
# Example only — use your Steam/Epic install paths
Start-Process "FactoryGame.exe"
Start-Sleep 15
Start-Process "FactoryGame.exe" # second instance if your platform allows
```

Steam: friends-list join into a Shared World must land in CLIENT mode (scenario L).

---

## Multi-PC / chaos checklist (3–4 machines)

| # | Case | Pass criteria |
|---|---|---|
| 1 | Steam join | Joiner is CLIENT; same generation/revision |
| 2 | Mod Play join | Same as above via Shared World UI |
| 3 | Intentional migration | Preferred successor hosts; others auto-reconnect |
| 4 | Host crash | Takeover after lease expiry; stale host fenced |
| 5 | Router disconnect host | Same as crash path |
| 6 | High latency / loss on preferred | Ranking demotes them before migrate |
| 7 | Storage outage mid-save | No premature lease release; no fake commit |
| 8 | Old host returns | All authority ops fail |
| 9 | Everyone presses Play | Exactly one host |
| 10 | Geo latency mix | Debug diag shows expected preferred successor |

Use `/sharedworld diag` on each machine and photograph / copy the candidate table
before and after each case.
