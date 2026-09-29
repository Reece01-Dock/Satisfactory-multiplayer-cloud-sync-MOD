# Shared Worlds improvement plan (Part 18)

Baseline: extend `SharedWorldCore` lease/CAS/revision fencing. Do **not** replace host ownership with packets or a parallel authority.

## 1. Current relevant architecture

| Layer | Role |
|---|---|
| `LeaseManager` + `state/current.json` | Authoritative host (generation, nonce, TTL, Join, Phase) |
| `WorldSession` | Play decide → HOST/JOIN; heartbeats; upload/release |
| `SyncEngine` | Fenced revision download/upload |
| `HostElection` + `PeerQualityMatrix` | Deterministic group scoring (already modular) |
| `HostMigrationEngine` + `PendingHandoff` | Planned/crash migration SM (core-tested) |
| `USharedWorldSubsystem` | Tick sessions; react to ReadyToHost / JoinReady |
| `USharedWorldHostController` | SessionDef force → LoadSaveFile → publish session id |
| `USharedWorldJoinManager` | ResolveOnlineSession → JoinSession |

## 2. Files / classes that will change

**Stage 1 (core):** `Model.h/.cpp`, `Lease.h/.cpp`, `WorldSession.h/.cpp`, new `HostHandshake.h/.cpp`, `WorldSessionTests` / new handshake tests.

**Stage 2 (UE join):** `SharedWorldHostController`, `SharedWorldJoinManager`, `SharedWorldSubsystem`, UI status strings, types.

**Stage 3 (live quality):** new UE `INetworkQualityProvider` adapter; wire `HostMigrationEngine::SetMatrix` / `UpdateCandidates`.

**Stage 4:** checkpoint interval / dirty marker; crash UX overlay already partial.

**Stage 5:** Git diagnostics tool; mod `GitHubClientId` / sign-in UX; optional `gh` install guidance.

## 3. Reuse (do not duplicate)

- Keep CAS `Acquire` / `Renew` / `CommitRevision` / `Release`
- Reuse `HostScoreWeights` / `ScoreHost` / `RankHosts` (no second scoring system)
- Reuse `PendingHandoff` for successor reservation
- Reuse `JoinInfo` for Steam session transport
- Reuse `Summarize` for menu cloud status (extend with HostReady)

## 4. Systems that must NOT be added

- Packet-based host ownership
- Second lease store outside `state/current.json`
- Parallel “active host” flag that can disagree with the lease without fencing
- Blind full-save spam uploads every few seconds

## 5. Git authentication — findings

| Concern | Finding |
|---|---|
| Repo remote | `https://github.com/Reece01-Dock/Satisfactory-multiplayer-cloud-sync-MOD` |
| Credential helper | Git Credential Manager `2.7.3` (system gitconfig) |
| `git ls-remote origin` | **PASS** (credentials present) |
| GitHub CLI (`gh`) | **Not installed** |
| SSH | Not the remote protocol; `ssh -T` can hang if prompted |
| **Mod** GitHub OAuth | `GitHubClientId = ""` in `SharedWorldSubsystem.cpp` → device flow returns “not configured” |

Two separate problems: (A) developer Git to this repo — currently OK via GCM; (B) in-mod storage sign-in — broken until a real OAuth App client id is configured (and preferably build-time injected). Diagnostics will classify missing `gh`, missing GCM creds, empty client id, etc.

## 6. Proposed session-state changes

Extend `SessionState` (player-facing via `ToString` / UI mapping):

- `CheckingHost` — lease says host live; probing reachability  
- `HostVerified` — transient → Join  
- `HostUnreachable` / `JoinRetry` — retry while lease live  
- `WaitingForSession` — lease live, `!HostReady` or Join empty  
- `Joining` — game join in flight  
- `RecoveringHost` / `ElectingHost` — after lease expiry / handoff fail  

Host-side cloud: keep `LeasePhase::Preparing` until session published; set `Hosting` + `Join` + `bHostReady=true` only then.

## 7. Host handshake design

- Pure structs + `ValidateHelloAck(Req, Ack, ExpectedLease, HeadRevision)` in core  
- Transport behind `IHostVerifier::Probe(Hello, JoinInfo)` (UE implements; tests fake)  
- Nonce must match; validate worldId, generation, host playerId, protocol version, revision, sessionId when present  
- Unreachable ≠ steal lease; only lease expiry / CAS Acquire grants hosting  

## 8. Election metrics

- Feed `PeerQualityMatrix` from UE net samples (RTT/loss/jitter/reachability)  
- Build `HostCandidate` via existing `MakeCandidate`  
- Score via existing `ScoreHost` (median/worst RTT, loss, jitter, reachability, session/storage bonuses)  

## 9. Migration sequence

Unchanged authority path: save → upload → `Release(Successor)` → `PendingHandoff` → successor Acquire → download Head → load → publish → `HostReady` → clients Join. Fallback: `OnSuccessorFailed` → re-rank / expire handoff → election.

## 10. Test plan

Extend core tests for: dual Play CAS; HostReady gating; verify fail while lease live; crash expiry takeover; stale commit fence; handoff reservation; handshake mismatch; Git diagnostic classification unit tests. UE/Steam flows remain Phase I manual + structured logs.

## Incremental order

1. Model HostReady + handshake + session verify flow + tests — **done**
2. UE publish HostReady + join diagnostics + friends fallback UI — **partial** (HostReady + join logs done)
3. **UE `IHostVerifier` transport (OnlineIntegration ResolveOnlineSession) + host responder** — **this increment**
4. Live network quality provider — started (ExactPing + verify RTT samples)
5. Checkpoint window / crash UX polish
6. Git diagnostics + OAuth client-id configuration path — diagnostics script done; OAuth App still manual

