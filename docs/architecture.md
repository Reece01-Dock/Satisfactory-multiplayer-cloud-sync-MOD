# Architecture

```
 Satisfactory (per player)                         Helper (per player)                      Cloud storage (shared)
┌──────────────────────────────┐   HTTP 127.0.0.1   ┌───────────────────────────────┐      ┌──────────────────────────────┐
│ SharedWorld SML mod          │   bearer token     │ shared-world-helper (Go)      │      │ worlds/<id>/world.json   CAS │
│  USharedWorldSubsystem ──────┼───────────────────►│  ipc      (auth, validation)  │      │ worlds/<id>/saves/           │
│  USharedWorldPanel (menu UI) │   poll / events    │  world    (state machine,     │      │   r00000184-g00000591-….sav  │
│  USharedWorldHostController  │◄───────────────────┼            HOST/JOIN decision)│─────►│   r00000185-g00000592-….sav  │
│  USharedWorldJoinManager     │                    │  lease    (acquire/renew/     │      │   (immutable, create-only)   │
│  USharedWorldIPCClient       │                    │            commit/release)    │      └──────────────────────────────┘
│  /sharedworld chat command   │                    │  syncer   (download/upload,   │
└──────────────┬───────────────┘                    │            backups, state)    │
               │ game APIs                          │  savefile (hash, validate)    │
               ▼                                    │  store    (provider iface)    │
   UFGSaveSystem, OnlineIntegration                 └───────────────────────────────┘
```

## Responsibilities

| Component | Owns | Never does |
|---|---|---|
| **Mod** `USharedWorldSubsystem` | polling the helper, reacting to session states, game lifecycle events | decide HOST vs JOIN, talk to the cloud, hold credentials |
| **Mod** `USharedWorldHostController` | loading the placed save, publishing the session id, `SaveGame` + reporting completed saves, checkpoints | upload, touch files |
| **Mod** `USharedWorldJoinManager` | calling the game's join APIs with the published join data | start a second copy |
| **Mod** `USharedWorldPanel` | main-menu UI | any logic beyond rendering + button → subsystem call |
| **Mod** `USharedWorldIPCClient` | discovery, launching the helper, authenticated requests | log the token |
| **Helper** `world.Manager` | the only session state machine; the HOST/JOIN decision; heartbeat; recovery | — |
| **Helper** `lease.Manager` | host lease, fencing tokens, compare-and-swap on the record | trust unvalidated records |
| **Helper** `syncer.Syncer` | verified download, fenced upload, local backups, local sync state | make a blob authoritative without a commit |
| **Helper** `store.Provider` | storage primitives (CAS record, create-only blobs) | contain lease logic |

State lives in exactly one place: the cloud record is the truth about the
world; the helper's `SessionView` is the truth about this machine's
session; the mod only mirrors `SessionView` for display.

## Data model — `worlds/<id>/world.json`

```json
{
  "schemaVersion": 1,
  "worldId": "our-factory",
  "worldName": "Our Factory",
  "generation": 592,
  "head": {
    "number": 185, "blobKey": "worlds/our-factory/saves/r00000185-g00000592-3f2a….sav",
    "sha256": "3f2a…", "size": 48213377, "createdAt": "…",
    "uploader": {"playerId": "…", "displayName": "Reece", "platform": "EOS", "installId": "…"},
    "generation": 592, "baseRevision": 184, "reason": "checkpoint"
  },
  "history": [ /* newest first, head included, max 50 */ ],
  "lease": {
    "generation": 592,
    "holder": {"playerId": "…", "displayName": "Reece", "platform": "EOS", "installId": "…"},
    "sessionNonce": "…",
    "acquiredAt": "…", "renewedAt": "…", "expiresAt": "…",
    "baseRevision": 185,
    "phase": "HOSTING",
    "join": {"kind": "online-session-id", "value": "…"},
    "players": [{"displayName": "Reece"}, {"displayName": "Vojta"}]
  },
  "lastSession": {"host": {…}, "generation": 591, "endedAt": "…", "reason": "expired"}
}
```

Differences from the schema sketched in the request, and why:

* **One record, one CAS.** Lease, head revision and history live in one
  object so every check ("is this still generation 592?", "is head still
  184?") and every change happen in a single atomic write. Separate lock and
  metadata files would reintroduce check-then-act races.
* **`generation` is the fencing token** and is stored at the record level,
  so it survives lease release: the next acquisition is always a higher number.
* **Blobs are immutable and uniquely named** (`r<revision>-g<generation>-<sha16>.sav`).
  There is no `current.sav` to overwrite; "promote temp to current" is the
  CAS that points `head` at the new blob. Every accepted revision is a
  backup by construction.
* **`players` and `join` belong to the lease**: they are only meaningful
  while that host holds it, and they disappear with it.

The helper validates every record it reads (schema version, world id, blob
keys under the world's prefix with the exact name pattern, hashes, sizes,
generation consistency, history ordering, text lengths/control characters)
and refuses to act on — or write back — anything invalid.

## Lease and fencing

Timing defaults: TTL 90 s, heartbeat 20 s, skew grace 30 s.

* **Acquire**: CAS loop. If a lease exists and is live *for an observer*
  (`now < expiresAt + skewGrace`), return who holds it (→ JOIN). Otherwise
  `generation++`, write the new lease. Of N simultaneous acquirers exactly
  one CAS succeeds; the others re-read, see a live lease, and join.
* **Renew** (heartbeat): succeeds only if `record.generation == token.generation`
  and the nonce matches. A host that was offline longer than the TTL but was
  **not** replaced can renew and continue: nobody else could have written.
* **Commit** (upload finalize): additionally requires `head.number == token.baseRevision`
  and `new.number == base + 1`. Hence *revision N never overwrites N+1*.
* **Release**: fenced like renew; sets `lastSession`.
* Old host returns after takeover → every renew/commit/release fails with
  `ErrFenced`; its upload is preserved locally as a conflict backup.
* The helper only renews while the game is alive: a dead game PID stops
  renewal immediately; a hung game (no mod keepalive for 5 min — long enough
  for a slow save load) stops it too. Either way the lease then expires.

Clock skew only affects *when* others consider a lease expired (and the
grace absorbs up to 30 s). It can never cause a lost update: authority for
writes is the generation check inside the CAS, not the clock.

## Local session state machine (helper)

```
IDLE ─► CHECKING ─┬─► JOIN_READY ─────────────────────────────► IDLE (ack)
                  ├─► WAITING_FOR_HOST ─► JOIN_READY | CHECKING
                  └─► ACQUIRING ─┬─► CHECKING (lost the race)
                                 ├─► RECOVERING ─► READY_TO_HOST | DOWNLOADING
                                 └─► DOWNLOADING ─► READY_TO_HOST
READY_TO_HOST ─► HOSTING ⇄ UPLOADING ─► RELEASING ─► IDLE
any lease-holding state ─► LEASE_LOST (fenced) | ERROR (failure, game exited)
ERROR ─► CHECKING (retry) | IDLE (dismiss)
```

Transitions are listed in `internal/world/state.go`; anything else is
refused and logged as a bug. Remote status shown to players is derived from
the record: `AVAILABLE`, `STARTING`, `ONLINE`, `SAVING`, `STOPPING`,
`RECOVERABLE` (host stopped heartbeating), `NO_SAVE`, `UNREACHABLE`.

## Play Shared World

1. **CHECKING** — read the record (fresh, not cached).
2. Live lease held by someone else → **JOIN_READY** with host name and
   join data (or **WAITING_FOR_HOST** until the host publishes it). Nothing
   is downloaded.
3. Live lease held by the same player/install from another instance →
   ERROR `ALREADY_HOSTING_ELSEWHERE` (never two hosts, never self-join).
4. Otherwise **ACQUIRING**; losing the race loops back to 2.
5. **RECOVERING** — if the local shared save has changes that never reached
   the cloud: if the cloud head is still the revision it was based on, it
   is uploaded as `recovered`; otherwise it is kept as a conflict backup.
6. **DOWNLOADING** — download to a temp file next to the target, verify
   size + SHA-256 + full save structure, back up the existing local file,
   atomic rename.
7. **READY_TO_HOST** — the mod loads the save; on **HOSTING** it publishes
   the session id; checkpoints and the final save go through UPLOADING.

## Upload pipeline (fail closed at each step)

1. wait until the file stops changing; full structural validation
2. private snapshot + SHA-256 (the game may keep writing the original)
3. pre-check record: generation + nonce + base revision
4. upload to a new create-only key (unchanged content → no new revision)
5. read back, verify size + SHA-256
6. fenced CAS commit
7. on refusal: delete the unreferenced blob, preserve the snapshot as
   `…_conflict-g<gen>-r<base>.sav`, report cloud vs local revision
8. on an ambiguous commit (response lost): re-read; if head is our blob, success

## Failure matrix

| Scenario | Behaviour | Test |
|---|---|---|
| Two players press Play at the same instant | exactly one HOST, others JOIN | `TestRace1_…` (16 goroutines, fs + mem), `TestSimultaneousPlayOneHostOneJoin` |
| Host crashes | heartbeat stops (PID gone), lease expires after TTL+grace, next player recovers with generation+1 | `TestRace2_…`, `TestHostCrashRecoveryAndStaleReturn` |
| Old host reconnects | all writes fenced; state LEASE_LOST; its progress kept as backup | `TestRace3_…`, `TestReplacedHostDetectsLeaseLoss` |
| Cloud revision changed during upload | commit rejected, blob removed, local save preserved as backup | `TestRace4_CloudChangedDuringUpload` |
| Corrupt download (bit flip, truncation, extension) | hash/size mismatch, local file byte-identical, temp removed, lease released | `TestRace5_…`, `TestCorruptCloudSaveReleasesLease` |
| Internet drops mid-upload | head unchanged, old head still downloads, retry succeeds | `TestRace6_…` |
| Stale client (rev N) uploads over N+1 | refused: "A newer shared save exists", cloud/local revisions reported | `TestStaleClientUploadRefused`, `TestCommitRequiresCurrentBaseRevision` |
| Commit applied but response lost | detected on re-read, reported as success | `TestAmbiguousCommitIsResolved` |
| Game crashes during save | truncated/corrupt save fails structural validation, never uploaded | `TestTruncatedLocalSaveNeverUploaded`, `TestCheckFileRejectsTruncatedSave` |
| Host crashed, nobody else played | its unsynced local progress is uploaded on its next Play | `TestUnsyncedProgressRecoveredWhenNobodyElseHosted` |
| Same user launches twice / second PC | second instance gets `ALREADY_HOSTING_ELSEWHERE` | `TestSameUserSecondInstanceIsNotAHost`, `TestSameUserSecondInstanceBlocked` |
| Different users with different local revisions | local file differing from last sync is backed up before replacement | `TestHostCrashRecoveryAndStaleReturn`, `TestUploadThenDownloadRoundTrip` |
| Stale lock | leases expire; filesystem provider lock files older than 30 s are broken | `TestRace2_…` |
| Helper crash while hosting | lease expires unless the restarted helper is re-attached by the mod (`/session/attach`, generation re-checked) | manual (see STATUS) |
| Cloud API failure | Play: ERROR `CLOUD_UNREACHABLE`, local untouched; heartbeat: retries, host keeps authority if not replaced | partly (`lease` tests) |
| Tampered / foreign metadata | rejected before use | `TestInvalidRemoteRecordRejected` |
| Two helper processes on one data dir | second refuses to start | `scripts/smoke-two-helpers.sh` |

## Security

* IPC binds 127.0.0.1; 256-bit bearer token in a per-user discovery file;
  constant-time comparison; requests with an `Origin` header (browsers) and
  non-loopback `Host` headers (DNS rebinding) are rejected; bodies are
  limited to 64 KiB and decoded strictly (unknown fields rejected).
* The mod never receives cloud credentials. Credentials belong in the
  helper's data directory / OS keychain (provider milestone); nothing is
  written into saves.
* All ids, save names and storage keys are validated against strict
  patterns; paths are built by the helper, never taken from the mod (the mod
  sends a save *name* that must equal `SharedWorld_<worldId>`).
* Structured JSON logs redact any attribute whose key looks like a secret;
  a test and the smoke script check the token never appears in logs.
