# STATUS

_Last updated: 2026-09-27 — native architecture (Phases A–G) implemented and tested in the core; the Unreal layer is rewritten onto it but has **not been compiled or run in game** (no UE/SML toolchain in this environment)._

## Architecture in one paragraph

Everything runs inside the mod — no helper process, no localhost server.
`SharedWorldCore` (pure C++20, no UE headers) holds the whole engine:
host lease with fencing generations, compare-and-swap commits, content-
addressed saves, sync/backups/restore, the session state machine, host
migration and crash recovery, membership, local settings and storage
providers. The `SharedWorld` UE module is a thin adapter: FHttpModule,
UE_LOG and Windows Credential Manager behind the core's interfaces, and a
game-instance subsystem that ticks one `sw::WorldSession` per world and
drives the game (load save / join / save) when a session asks. The Go
helper stays in the repo as the reference implementation and test oracle
(its save validator produced the shared conformance corpus). Design:
`docs/native-architecture.md`.

**Who does what with Steam / Epic vs storage:**

| Need | Handled by |
|---|---|
| Joining a friend's live game | The game's own Steam/Epic sessions: the host's session id is published in the world state; friends can also join through the friends list as usual |
| Player identity, members, roles | Steam/Epic account ids (`players.json` keys) — nobody types a GitHub name to play |
| Storing saves + the host lease | A GitHub repository or a shared folder (Steam/Epic storage is per-user or read-only and has no compare-and-swap) |
| Being able to *host* / take over | Write access to that storage: `/sharedworld granthost <github-user>` (GitHub collaborator invite). Players without it can still join games |

## Phase state

| Phase | State |
|---|---|
| A Core foundation (Result, JSON, SHA-256, clock, logging with redaction, queues) | **Done & tested** |
| B Model, repository/object-store interfaces, memory + folder providers | **Done & tested** |
| C Lease + fencing (Races 1–3), revisions, CAS | **Done & tested** |
| D Save validation (Go/C++ conformance corpus, 24 files agree) | **Done & tested** |
| E Sync: safe download/upload, backups, restore-as-new-revision, dedup, offline cache (Races 4–6) | **Done & tested** |
| F Session engine, host migration (successor reservation), crash recovery (candidate rules) | **Done & tested** |
| G GitHub provider (Git Data API fast-forward CAS + release-asset objects), device-flow sign-in, membership, local settings | **Done & tested** against a faithful fake GitHub |
| H UE integration (adapters, subsystem, host/join controllers, panel, chat) | **Written, not compiled** |
| I Runtime validation in game | **Not started** — needs the game |

## Verified behaviour (86 core tests, `shared-world-mod/core-tests`)

Run under GCC, Clang, ASan+UBSan and TSan on Linux and MSVC on Windows (CI `core.yml`).

* **Exactly one host.** 16 simultaneous acquires → one host (memory, folder and fake-GitHub providers). Mutation-checked: disabling CAS gives 14–16 hosts; on GitHub, allowing non-fast-forward ref updates breaks it (the design depends on `force:false`).
* **Fencing.** A replaced host coming back cannot renew, commit or release; its save is kept as a conflict backup. A client holding revision N can never overwrite N+1.
* **Safe transfer.** Corrupt / truncated / extended downloads are rejected and the local save stays byte-identical; interrupted uploads leave the cloud head intact; a lost commit response is resolved, never blindly retried; truncated local saves are never uploaded.
* **Host flows.** HOST then JOIN; 5 simultaneous Play → one host; stop uploads + releases; checkpoints advance revisions; newer cloud revision refuses upload (backup kept).
* **Migration.** Planned handover reserves the world for the successor and hands over the exact revision; after the window anyone may take over. A successor recorded without an install id (the host cannot know it) matches on player id.
* **Crash recovery.** Clients of a crashed host take over in a deterministic stagger after lease expiry; a crashed host resumes its own lease and recovers unsynced progress; recovery candidates are accepted only from the crashed generation, based on the head, validated and available — never because they are newest.
* **A client that quit to the menu never takes over** (mutation-checked: without the fix it silently became host).
* **Restore** creates a new revision; history is never rewritten.
* **Membership.** Roles gate edits; the last owner is protected; concurrent edits all land; edits never touch the lease document.
* **Credentials.** Tokens never appear in logs, settings, the repository, or the UI; the object-storage redirect never receives the Authorization header; device flow handles pending / slow_down / denied / expired and hostile replies.
* **Local settings.** Hostile world ids and paths are rejected; a damaged file is reported (the mod sets it aside instead of overwriting); a file from a newer mod is never overwritten.

## Unreal layer (written, not compiled)

* `SharedWorld.Build.cs` depends on `SharedWorldCore`; the helper executable and IPC client are removed.
* Adapters: `FSharedWorldHttpClient` (blocking only on core worker threads, bounded wait, completion state survives a timeout), `FSharedWorldLogSink`, `FSharedWorldCredentialStore` (Windows Credential Manager).
* `USharedWorldSubsystem`: loads `%LOCALAPPDATA%/SatisfactorySharedWorld/settings.json`, one runtime per world, ticks sessions every second, reacts to `READY_TO_HOST` (load save, going to the menu first if needed), `JOIN_READY` (join from the menu), `MIGRATING` (migration save), `LEASE_LOST` / handover (return to menu, old host rejoins as client), host load timeout (5 min). Creation, history, membership and invites run on a background queue.
* Game events: network failure → `OnHostConnectionLost` / `OnJoinFailed`; client world ready → `OnJoinedAsClient`; client leaving on purpose → `OnLeftAsClient`; hosted world teardown → `OnWorldEnded` with retry until released.
* UI (C++ UMG, main menu): world list with status, host, players, revision, last played; Play / Cancel / Retry / Dismiss; details with steps, error detail, history, restore-by-number, remove from list; setup box with GitHub sign-in (device code shown in the panel), convert a save, add a friend's world.
* Chat: `/sharedworld status | history | players | save | stop | migrate <player> | allow <player> [role] | remove <player> | open | restrict | granthost <github-user> | log`.

## Known limitations

* **Not compiled.** Written against SML 3.12 / UE 5.3 headers that were checked earlier, plus standard engine APIs; expect small compile fixes.
* **GitHub OAuth client id is empty** (`GitHubClientId` in `SharedWorldSubsystem.cpp`): the project owner must register a GitHub OAuth App with device flow enabled. Until then only folder storage works.
* **Installed-mod list** is not collected (the SML API was not verified), so worlds record no required mods.
* **Game build number** is taken from the engine changelist; must be confirmed to match save headers.
* **Successor readiness** (compatible, storage reachable, has head cached) is not exchanged between players; planned migration trusts the host's choice and falls back to normal takeover if the successor cannot host.
* **Peer recovery candidates** come from the host's own reports; clients cannot save the host's world.
* Menu UI is an overlay panel, not inserted into the game's menu list; no dedicated in-game players/settings screens yet (chat commands cover them).
* Friends join via the game's own friends list; the mod does not open the platform invite dialog itself (API not verified).
* Exiting a hosted world without `/sharedworld stop` uploads the last completed save (checkpoints every 15 min).

## Runtime validation needed (Phase I)

1. `LoadSaveFile` + `Start()` on the returned sequence: loads once, no double travel.
2. Session-id round trip across machines (`OnlineSessionIdToString` → `MakeOnlineSessionId` → `ResolveOnlineSession` → `JoinSession`), on Steam and Epic.
3. `UFGSaveSystem::GetSaveDirectoryPath()` is where `LoadSaveGameHeaderSync` looks (Steam vs Epic save folders).
4. `SaveGame` delegate fires after the file is closed (core waits for a stable file either way).
5. `ClientReturnToMainMenuWithTextReason` returns to Satisfactory's main menu (lease lost / handover / successor).
6. FHttpModule on worker threads; whether the game's libcurl forwards `Authorization` across a redirect host (the provider does not depend on it, but it must not leak).
7. Player id format from `GetPreferredUniqueNetId()` / `APlayerState::GetUniqueId()` is identical for the same account on host and clients.

## Next steps

1. Compile in the SML starter project; fix compile errors.
2. Register the GitHub OAuth App; set the client id.
3. Two-PC test (Steam + Epic): create from a save, host, join, checkpoint, stop, crash the host, planned migration, restore.
4. Replace the chat-only players/settings management with menu screens.
