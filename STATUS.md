# STATUS

_Last updated: 2026-09-27 — Milestones 1–2 done, first vertical slice (helper) done and tested, mod written but not yet compiled or run in game._

## Completed work

| Milestone | State |
|---|---|
| 1 Research | **Done** — `docs/research.md` (existing BAT workflow, SML 3.12 architecture, save lifecycle and format, session/join APIs, multiplayer requirements, provider CAS capabilities) |
| 2 Skeleton | **Done** — `shared-world-helper/` (Go), `shared-world-mod/SharedWorld/` (SML C++), `docs/` |
| 3 Status AVAILABLE/ACTIVE | Helper **done & tested**; mod panel written, **not run in game** |
| 4 Lease system | **Done & tested** — acquire, heartbeat, expiry, release, fencing generation, CAS |
| 5 Safe download | **Done & tested** — revision lookup, temp file, SHA-256 + size + full save-structure check, backup, atomic replace |
| 6 Hosting decision | **Done & tested** in helper (HOST / JOIN / WAITING_FOR_HOST / ALREADY_HOSTING_ELSEWHERE); mod side written |
| 7 Upload | **Done & tested** in helper (snapshot, create-only blob, read-back verify, fenced commit, conflict backups); mod triggers written |
| 8 Auto-join | Mod code written against verified game APIs; **runtime unverified** |
| 9 Crash recovery | Helper-side recovery **done & tested** (game crash, stale host return, unsynced progress); manual kill tests in game pending |
| 10 UX | Main-menu panel (C++ UMG overlay) + `/sharedworld` chat command written; polish pending |

## Verified behaviour

Verified by automated tests in this repo (`cd shared-world-helper && go test -race ./...`,
all green, also 15× repeated under the race detector):

* Play Shared World returns **HOST** for the first player and **JOIN** with
  the host's name and join data for the next; the joining player downloads nothing.
* 16 simultaneous acquisitions → exactly one host (on both the in-memory
  and the filesystem store). Mutation check: with CAS disabled the same test
  fails (4 hosts), so it genuinely detects the race.
* Crashed host: heartbeat stops when the game PID disappears, the lease
  expires after TTL + skew grace, another player becomes host with a higher
  generation from the last committed revision.
* Replaced host coming back: renew/commit/release rejected (fenced), its
  save kept as a conflict backup, state `LEASE_LOST`.
* Revision N can never overwrite N+1 (commit and upload both refuse; the
  message reports cloud vs local revision).
* Corrupt download (bit flip / truncated / extended) → rejected; local save byte-identical.
* Upload interrupted mid-transfer → cloud head unchanged and still downloadable; retry works.
* Commit applied but response lost → recognised as success.
* Truncated save (crash mid-write) → never uploaded (header parse + every
  zlib chunk inflated + declared body size checked).
* Host crashed and nobody else played → its unsynced progress is uploaded on its next Play.
* Same player, second instance/PC → refused, no second host.
* Tests were mutation-checked where it matters: Race 1 fails with CAS
  disabled; the NEWER_SAVE_EXISTS test hangs without the lock fix it
  was written for (a real deadlock found during review).
* IPC: no token → 401, browser `Origin` → 403, rebinding `Host` → 403,
  unknown JSON fields → 400; token never appears in responses or logs.

Verified with the real binary (`shared-world-helper/scripts/smoke-two-helpers.sh`):
two helper processes with separate data dirs sharing one store — create
world → A Play → HOST → publish join → B Play → JOIN (nothing downloaded)
→ A final save → revision 2 uploaded, lease released → B Play → HOST with
A's revision 2 (byte-identical). Second helper on the same data dir is refused.

Cross-checked, not run: every game/SML include and symbol the mod uses
(26 symbols: `UFGSaveSystem::*`, `UCommonSessionSubsystem::*`,
`UCommonSessionStatics::JoinSession`, `ULocalUserInfo::GetGameSession`, SML
modules/chat commands, …) exists with the used signature in the SML `dev`
headers (commit `ae723cf`, SML 3.12.0).

## Current architecture

See `docs/architecture.md`. In one paragraph: the SML mod renders the
helper's session state and reports game events; the local Go helper owns
the only state machine and the HOST/JOIN decision; all shared state is one
JSON record per world updated only by compare-and-swap, with a
monotonically increasing generation as fencing token; saves are immutable,
uniquely named blobs that become current only through a fenced commit.
IPC is authenticated localhost HTTP (`docs/ipc-protocol.md`).

## Known limitations

* **No real cloud provider yet.** Only `filesystem` (single machine or a
  true network share) and in-memory (tests). Never point the filesystem
  provider at a Google Drive/OneDrive/Dropbox sync folder.
* **Mod not compiled or run.** This environment has no Unreal/SML toolchain.
* Exiting a hosted world without `/sharedworld stop` uploads the last
  *completed* shared save (last checkpoint, at most 15 min old) — progress
  after it is lost. Pause-menu hooks for a proper "save & release on exit"
  are not implemented.
* Panel is a viewport overlay on the main menu, not inserted into the
  game's menu list (widget-hook parent name unverified).
* World creation is an IPC call (`POST /v1/worlds`), no UI yet. No
  Backups / Restore / Players / Settings screens yet.
* Helper restart while hosting: resume via `POST /session/attach` exists in
  the helper, but the mod does not call it yet — the lease expires instead
  (safe, but the host loses authority).
* Lease-expiry judgement uses local clocks (+30 s grace). Skew cannot cause
  lost updates (fencing), only earlier/later takeover.
* The mod's player id comes from `ULocalPlayer::GetPreferredUniqueNetId()`;
  its format across Steam/Epic is not yet checked in game.

## Tests completed

| Package | Tests |
|---|---|
| `lease` | Race 1 (16 concurrent, fs + mem), Race 2 (expiry + skew grace), Race 3 (fencing), stale base revision, renew after unobserved expiry, same-user second instance, release, invalid/tampered records |
| `syncer` | round trip + backup, Race 4 (cloud changed during upload), stale client, Race 5 (3 corruption kinds), Race 6 (connection drop), ambiguous commit, truncated save, unsynced-change detection |
| `world` | HOST then JOIN, simultaneous Play ×5, clean stop, crash recovery + stale return, unsynced recovery, fenced host detects loss, second instance, corrupt cloud save releases lease, host without join info, newer cloud revision refuses upload (NEWER_SAVE_EXISTS + backup), illegal transitions |
| `store` | FS CAS across 6 instances (no lost updates), create-only blobs / no partial blobs, key traversal |
| `savefile` | valid, truncated, corrupt chunk, garbage, save names |
| `ipc` | auth / origin / host / strict JSON, HTTP HOST→JOIN |
| `logx` | secret redaction |
| smoke | two real helper processes (script) |

CI: `.github/workflows/helper.yml` runs vet + tests on Linux (race) and
Windows, the smoke test, and a Windows cross-build.

## Current blockers (need a real game)

1. **Does `LoadSaveFile` need `Start()`?** The mod calls `Start()` on the
   returned sequence (sibling APIs say the caller starts it). Must confirm
   it neither fails nor double-travels.
2. **Session-id round trip.** Confirm `OnlineSessionIdToString` on the
   host → `MakeOnlineSessionId` + `ResolveOnlineSession` on another
   machine/store resolves, and that `UCommonSessionStatics::JoinSession`
   starts the join itself.
3. **Save directory on Epic.** Confirm `UFGSaveSystem::GetSaveDirectoryPath()`
   is where `LoadSaveGameHeaderSync` looks (the game has common vs Epic locations).
4. **`SaveGame` callback timing.** Confirm the file is closed when the
   delegate fires (the helper's stability + structure checks protect either way).
5. Compile the mod in the SML 3.12 starter project (UE header details such
   as `FTSTicker`, `UUserWidget` viewport helpers were written from UE 5.3
   knowledge, not compiled here).

## Next task

1. Compile the mod in an SML dev environment; run the two-account in-game
   test for blockers 1–4 (two PCs, filesystem provider on a shared
   network folder, or one PC + two accounts).
2. Implement the first real cloud provider with native CAS (S3-compatible
   conditional writes: works with Cloudflare R2 / MinIO / AWS S3), plus
   OS-keychain credential storage.
3. Pause-menu "Stop hosting & upload" and save-on-exit hook; mod calls
   `session/attach` after reconnecting to a restarted helper.
