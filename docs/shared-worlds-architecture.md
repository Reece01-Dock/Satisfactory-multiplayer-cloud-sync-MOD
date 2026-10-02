# Shared Worlds architecture (v2): authority + durable storage + P2P

Status: **plan** (Phase 1). Written from the code as it is on 2026-10-02. Companion documents:
[authority](shared-worlds-authority.md) · [storage](shared-worlds-storage.md) · [P2P](shared-worlds-p2p.md) ·
[locking problem](storage-locking.md).

Goal: remove GitHub as a requirement while keeping every safety property, then add P2P as a speed layer.

```
                Shared Worlds
                  Steam/EOS
                     │
              friend P2P mesh            fast path: presence, heartbeats, save transfer, migration
                     │
              World Authority            correctness: lease, generation, HEAD, fencing, membership
                     │
              Durable Storage            durability: immutable save objects, history, offline fallback
```

Rules: **P2P is the fast path. Durable storage is the fallback and the source of truth for save bytes. Only the
authority decides who may host.**

---

## 1. What exists today (inspected, not assumed)

| Piece | Where | What it does | Keep? |
|---|---|---|---|
| `IWorldRepository` | `Core/Storage/Storage.h` | Versioned tree of small files; **one atomic primitive** `Commit(ExpectedHead, changes)` | Keep the contract |
| `WorldStore` | `Core/Lease/Lease.h` | Read-modify-CAS loop over `state/current.json` (`Mutate`, `UpdateDocument`), jittered retry | Keep unchanged |
| `LeaseManager` | `Core/Lease` | `Acquire` (generation+1), `Renew`, `CommitRevision` (requires generation+nonce and `head == base`), `Release` (+ handoff reservation), `CheckFence` | **Keep unchanged** |
| `IObjectStore` | `Core/Storage/Storage.h` | Content-addressed `.sav` objects (SHA-256), immutable, idempotent `Put` | Keep; becomes `ISharedWorldObjectStore` in docs |
| `EncodingObjectStore` | `Core/Storage` | zstd packaging on top of any object store | Keep |
| `SaveStorageRouter` | `Core/Storage` | Routes a world's objects to the store named in `world.json` (`saveStorage`) | Keep, generalise to a store chain |
| `SyncEngine::Upload` | `Core/Sync/Sync.cpp:294` | snapshot → hash → `Has` → `Put` → read-back verify → `CommitRevision` | **Already durable-before-HEAD** (§3) |
| Local cache, backups, unsynced-progress recovery | `Core/Sync` | verified object cache, conflict backups | Keep; extend into the P2P cache |
| `HostElection` | `Core/HostElection` | Deterministic integer scoring (RTT, loss, jitter, reachability, save cached, storage ready) | Keep: already "ranking only" |
| `HostMigrationEngine` | `Core/HostMigration` | Planned handoff / crash takeover state machine on top of the lease | Keep; feed it P2P inputs |
| `HostHandshake` / `UEHostVerifier` | `Core/HostHandshake`, module | Verifies the advertised host session actually resolves before joining | Keep |
| `WorldSession` | `Core/World` | Per-world HOST/JOIN state machine, ticked from the game thread | Keep |
| GitHub provider + device-flow auth | `Core/Providers` | `IWorldRepository` on a Git branch (FF-only ref = CAS) + release-asset object store | Becomes **optional** |
| Folder provider | `Core/Storage/FileStorage` | Mini-git on disk with lock+rename CAS | Keep (LAN / tests) |
| rclone engine | module `Rclone/` | librclone DLL; `FRcloneObjectStore` on any of ~69 backends; provider connect UI | Keep as durable object storage |
| Discovery | `Services/SharedWorldDiscoveryService` | Steam friends list via UE OnlineSubsystem; "hosting now" read from the lease in storage | Extend with P2P |
| Joining | `SharedWorldJoinManager`, lease `JoinInfo` | Host publishes its game session id in the lease; clients join through the game's online sessions | Keep |
| In-session invite push | `SharedWorldInviteBridge` | Unreal RPC inside a running game session | Keep |

### Safety properties that already hold and must not regress

1. Exactly one host per generation (CAS on `state/current.json`).
2. Every write is fenced by `generation + nonce`; a stale host gets `Fenced`.
3. Revision N+1 can only be committed on top of N (`head == base`).
4. A save object is uploaded and verified **before** the commit that references it (`Sync.cpp:370-418`).
5. Objects are immutable and content-addressed; a failed upload can damage nothing.
6. Planned handoff reserves the next acquisition for one successor.
7. Ambiguous commit outcomes are resolved by re-reading the head.

**Key consequence:** all of these live *above* `IWorldRepository`. GitHub only provides the CAS. Replacing GitHub
therefore means providing a different, equally atomic `IWorldRepository`, **not** rewriting lease logic.

---

## 2. Target design

### 2.1 Layers and their contracts

| Layer | Interface | Implementations |
|---|---|---|
| Authority | `IWorldRepository` (unchanged contract) wrapped by `ISharedWorldAuthority` façade | **Shared Worlds Authority service** (new, Cloudflare Durable Object), GitHub (optional), Folder (LAN), Memory/Fake (tests) |
| Durable storage | `IObjectStore` (`ISharedWorldObjectStore`) | Authority-issued signed URLs to R2 (new, zero setup), rclone providers (built), GitHub releases (optional), folder |
| P2P | `ISharedWorldPeerNet` (new) | Steam Networking Messages (Steam players), EOS P2P (to be evaluated, §P2P doc), Null (tests) |
| Object lookup | `ObjectFetchChain` (new) | local cache → connected peers → durable store, always hash-verified |

### 2.2 `ISharedWorldAuthority`

A thin façade over the existing classes, so callers speak in domain terms while the safety logic stays where it is:

```cpp
class ISharedWorldAuthority {
  GetWorldState / AcquireLease / RenewLease / ReleaseLease   -> LeaseManager over the authority repository
  CommitRevision / SetHostReady                              -> LeaseManager::CommitRevision / Renew(HostReady)
  GetMembership / AddMember / RemoveMember                   -> WorldStore::UpdateDocument(players.json)
};
```

The authority service itself only has to implement the `IWorldRepository` primitive (atomic multi-file commit with
expected head) plus authentication and membership checks. See [authority](shared-worlds-authority.md).

### 2.3 Save publication (target pipeline)

```
create save → SHA-256 → local cache → durable Put (≥1 required target) → verify exists
  → revision metadata → CommitRevision (fenced CAS at the authority)
  → success → REVISION_ANNOUNCE over P2P → idle peers may prefetch
```

Today's pipeline already does everything up to and including the fenced commit, in that order. New: the P2P
announcement, prefetch, mirror targets (async) and storage-health tracking. Upload failure → HEAD does not move
(already true). Commit `Fenced`/`Conflict` → object kept, not published, state re-read (already true).

### 2.4 Object lookup

```
need sha256:X → local cache (verified) → peers that answered SAVE_HAVE (verify full hash; corrupt → drop peer, next)
              → durable store (verify) → fail
```

Never trust peer bytes: the full SHA-256 is checked before the file is used, exactly like cloud downloads today.

---

## 3. Scenarios (how each required flow maps onto the design)

### Normal hosting
```
Play → authority: AcquireLease(gen N → N+1) ✓ → fetch HEAD object (cache/peer/cloud) → host
     → Renew(HostReady, JoinInfo) → HOST_HEARTBEAT over P2P every few s; lease renew at the authority every 30-45 s
     → autosave: Upload (durable) → CommitRevision → REVISION_ANNOUNCE
```

### Offline startup (spec §15)
```
Reece hosted rev 184 (ABC123 durable, HEAD committed) and shut down.
Tom: Play → no peers online → authority: lease expired → AcquireLease(gen 82→83) ✓
     → cache miss → peers: none → durable store: Get(ABC123) → verify → host rev 184
```
Needs: authority reachable, Tom is a member, Tom can read durable storage (storage access model, storage doc §4).

### Fast migration (spec §16)
```
Reece hosts; Tom + Jack connected; Tom prefetched ABC123.
Reece crashes → HOST_HEARTBEAT stops (P2P, ~5 s) → peers run HostElection on shared inputs → Tom ranked first
  → Tom: AcquireLease(expected gen 82) at the authority ✓ gen 83 → ABC123 in cache, verify → host
  → Renew(HostReady + JoinInfo) → MIGRATION_STATUS / authority state → Jack reconnects
```
Note: today crash takeover waits for the lease TTL (45 s + 15 s skew grace). P2P heartbeat loss lets candidates *start
preparing* early, but **acquisition still requires the authority to consider the lease expired** — or an explicit
"host gone" vote rule at the authority (open question A3 in the authority doc). Never P2P-only.

### Simultaneous host acquisition / partition (spec §18)
Both Tom and Jack call `AcquireLease(expected gen 82)`. The authority serialises them (single Durable Object per
world); one commit wins with gen 83, the other gets `Conflict`, re-reads, sees Tom, cancels, joins.
This is today's `WorldStore::Mutate` behaviour; tests `Lease_Race*` already cover it against Memory/File repos and
will run unchanged against the fake authority.

### Storage failure (spec §21)
Durable `Put` fails → no commit → world keeps running locally → UI: "World storage unavailable — progress cannot be
safely saved until storage is restored." The uncommitted local save is kept as unsynced progress (existing recovery
code) and uploaded later only if HEAD is still its base. It is **never** announced to peers as authoritative.

---

## 4. Implementation plan (incremental, existing system keeps working)

| Phase | Work | Exit criterion |
|---|---|---|
| **1** | This plan + authority/storage/P2P docs + PoC list | Decisions D1-D4 below answered |
| **2** | Interfaces without behaviour change: `ISharedWorldAuthority` façade over `LeaseManager`/`WorldStore`; `ObjectFetchChain` (cache → durable); `ISharedWorldPeerNet` with Null impl; storage-health model | All existing core tests green; game behaves identically |
| **3** | Fake authority: an in-process `IWorldRepository` with the authority service's exact wire semantics (membership checks, auth errors, size limits) + test matrix of spec §26 | Tests: upload fails → HEAD unchanged; simultaneous acquire → exactly one; stale host rejected; crash before/after commit |
| **4** | Durable-before-HEAD hardening: storage-health tracking, "≥1 required target" policy, async mirrors, explicit "storage unavailable" state in `WorldSession` | Fault-injection tests |
| **5** | Authority service (Cloudflare Worker + Durable Object) + `FAuthorityRepository` HTTP client; auth (see authority doc); membership | Live PoC: two PCs, simultaneous Play, exactly one host |
| **6** | `ISharedWorldPeerNet` on Steam Networking Messages (PoC first) | Two Steam accounts exchange HELLO from the main menu |
| **7** | SWP/1: HELLO, WORLD_STATUS, HOST_HEARTBEAT, PING | Browser shows live host/players from P2P |
| **8** | SAVE_HAVE / SAVE_REQUEST / SAVE_CHUNK + prefetch + cache limits | Corrupt-peer test; cloud fallback test |
| **9** | P2P inputs into `HostElection` (RTT, loss, cached save) | Deterministic ranking test from recorded inputs |
| **10** | Migration integration: early heartbeat-loss detection, `MIGRATION_PROPOSAL/STATUS`, UI stages | Fast-migration test with cached save |
| **11** | UI: Live/Offline, host, cached-by, storage health, migration stages (existing theme) | Screens reviewed in game |
| **12** | Existing-world migration GitHub → authority (copy state + objects, mark old world "moved", rollback window) | Migrated world plays; old copy refuses to host |
| **13** | Fault injection + two/three-PC integration runs | Checklist in `docs/testing.md` |

Order rationale: correctness layers (authority, durability) before speed layers (P2P). Phases 2-4 need no external
service and no decisions beyond D1.

---

## 5. Decisions needed from the project owner

| # | Decision | Why it matters | Recommendation |
|---|---|---|---|
| **D1** | Who runs the authority service (Cloudflare account, domain, cost owner) | It is a real hosted component; every world depends on it being up | Project-run Cloudflare Worker + Durable Objects; tiny metadata only; free/low tier expected (verify pricing) |
| **D2** | Durable storage access for members (storage doc §4) | A friend can't read the creator's Google Drive by default | Default: authority-issued signed URLs on project R2 (zero setup). Optional: per-world rclone provider with provider-native folder sharing (already built) |
| **D3** | Player identity verification (authority doc §4) | Must not trust "steamId: whatever" | Steam OpenID browser sign-in (verifiable without Coffee Stain keys) + per-device key; Epic: own EOS app with Epic Account Services, or invite-capability fallback |
| **D4** | Epic/EOS P2P | Steam P2P cannot reach Epic players | Phase 6 Steam-only; Epic players use cloud fallback until an EOS path is confirmed (P2P doc §2) |

---

## 6. ASSUMED / TESTED / CONFIRMED / UNSUPPORTED

| Claim | Status | Evidence / next step |
|---|---|---|
| Lease/fencing logic depends only on `IWorldRepository::Commit` CAS | **CONFIRMED** | `Lease.h` header comment + code; GitHub, File, Memory repos all plug in |
| Save object is durable before HEAD moves | **CONFIRMED** (code) | `Sync.cpp:370-418`: Has/Put/verify, then `CommitRevision` |
| Steam Networking Messages callable from the mod in the main menu | ASSUMED | Game ships `steam_api64.dll` (Steamworks 1.57, has ISteamNetworkingMessages). PoC in Phase 6 |
| Steam P2P works between two players of app 526870 without our own app id | ASSUMED | PoC |
| EOS P2P via the game's EOS platform is allowed and reachable | ASSUMED / policy risk | `SocketSubsystemEOS` ships; using CSS's EOS deployment for our traffic needs a policy decision (see native-architecture §1) |
| Our service can verify Steam session/web-API tickets for app 526870 | **UNSUPPORTED** (as far as known) | Requires the app's publisher Web API key (Coffee Stain). Alternative: Steam OpenID |
| Steam OpenID sign-in verifies a SteamID for any website | ASSUMED (documented by Valve) | PoC in Phase 5 |
| Durable Objects give single-threaded, strongly consistent per-object storage | ASSUMED (documented by Cloudflare) | PoC in Phase 5 with concurrent acquire test |
| R2 presigned URLs usable from the game via `FHttpModule` | ASSUMED | PoC in Phase 5 |
| Friends list readable via UE OnlineSubsystem (Steam) | **TESTED** | `SharedWorldDiscoveryService` works in game today |
| librclone runs in-process | **TESTED** | Self-test PASS in game (2026-10-02) |
| rclone browser sign-in + read/write/delete on Dropbox | **TESTED** | Dropbox connected and verified in game (2026-10-02) |
