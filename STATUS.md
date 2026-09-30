# STATUS

_Last updated: 2026-09-29 — Shared Worlds browser redesigned for progressive disclosure (Create / Join Friend / sections); Discovery/Invite/Creation services wired; Shipping build installed._

## Architecture in one paragraph

Everything runs inside the mod — no helper process, no localhost server.
`SharedWorldCore` (pure C++20, no UE headers) holds the whole engine:
host lease with fencing generations, compare-and-swap commits, content-
addressed saves, sync/backups/restore, the session state machine, host
migration and crash recovery, membership, local settings and storage
providers. The `SharedWorld` UE module is a thin adapter: FHttpModule,
UE_LOG and Windows Credential Manager behind the core's interfaces, and a
game-instance subsystem that ticks one `sw::WorldSession` per world and
drives the game (load save / join / save) when a session asks. Design:
`docs/native-architecture.md`. UI integration: `docs/ui-integration.md`.

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
| H2 Native menu UI (main menu entry, browser, Manage Session, migration overlay) | **Written, not compiled** — see `docs/ui-integration.md` |
| I Runtime validation in game | **Not started** — needs the game |

## Verified behaviour (153 core tests, `shared-world-mod/core-tests`)

Run under GCC, Clang, ASan+UBSan and TSan on Linux and MSVC on Windows (CI `core.yml`).

* **Exactly one host.** 16 simultaneous acquires → one host (memory, folder and fake-GitHub providers). Mutation-checked: disabling CAS gives 14–16 hosts; on GitHub, allowing non-fast-forward ref updates breaks it (the design depends on `force:false`).
* **Fencing.** A replaced host coming back cannot renew, commit or release; its save is kept as a conflict backup. A client holding revision N can never overwrite N+1.
* **Safe transfer.** Corrupt / truncated / extended downloads are rejected and the local save stays byte-identical; interrupted uploads leave the cloud head intact; a lost commit response is resolved, never blindly retried; truncated local saves are never uploaded.
* **Host flows.** HOST then JOIN; 5 simultaneous Play → one host; stop uploads + releases; checkpoints advance revisions; newer cloud revision refuses upload (backup kept).
* **Host election (peer matrix).** Every candidate is scored from a full NxN RTT/jitter/loss matrix (not ping-to-host alone). Configurable `HostScoreWeights`; high loss/jitter loses to a slightly higher but stable RTT. Hard gates: storage, compatibility, session capability, host eligibility. Deterministic tie-breakers. Hysteresis blocks tiny score flips.
* **Host migration engine.** Planned and crash state machines with event traces, timeouts, successor readiness, ranking freeze during planned handoff, and fallback when the preferred successor fails. Headless `MultiplayerSimulator` (2–50 players) drives the *same* SharedWorldCore algorithms used by the mod.
* **Scenarios A–L** (`Scenario_*` tests): startup, 16-way Play, matrix election, planned migration, crash, stale return, partition, interrupted upload, CAS conflict, corrupt download, unsynced progress, Steam/session client join.
* **Benchmarks.** Election ranking cost measured at 4 / 8 / 16 / 32 / 50 players.
* **Migration scenarios 1–10** covered by automated tests (best successor, fallback, loss vs ping, save-cache emergency bias, crash + generation bump, stale host fenced, session publish failure, storage outage mid-migration, multi-candidate race, frozen ranking).
* **Chaos suite.** Seeded fuzz (`--seed` / `SW_TEST_SEED`); small suite in CI; large overnight suite via `-DSW_CHAOS_FULL=ON`. Suite summary prints election/crash/migration/storage counters.
* **Docs.** `docs/testing.md` — one-command harness, two-instance Satisfactory checklist, multi-PC plan. `docs/ui-integration.md` — menu hooks and screens.
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
* `USharedWorldSubsystem`: loads `%LOCALAPPDATA%/SatisfactorySharedWorld/settings.json`, one runtime per world, ticks sessions every second, reacts to `READY_TO_HOST` (load save, going to the menu first if needed), `JOIN_READY` (join from the menu), `MIGRATING` (migration save), `LEASE_LOST` / handover (return to menu, old host rejoins as client), host load timeout (5 min). Creation, history, membership and invites run on a background queue. Migration overlay follows `HostMigrationEngine` / session state.
* **Native menus (H2):** `USharedWorldGameInstanceModule` registers SML WidgetBlueprintHooks on `mMainMenuList` / `mManageSessionList` and runtime-injects after **Join Game** / under Manage Session. Browser, details, session management, and migration/recovery overlays are C++ UMG under `UI/`. Corner overlay disabled as primary UX (`docs/ui-integration.md`).
* Chat: `/sharedworld status | history | players | save | stop | migrate <player> | allow <player> [role] | remove <player> | open | restrict | granthost <github-user> | log`.

## Known limitations

* **API audit (2026-09-30):** see `docs/ficsit-api-audit.md` and `docs/ficsit-compatibility.md`. docs.ficsit.app was unreachable, so findings rest on the repo, `docs/research.md` and headers; UE edits from the audit are **not compiled**.

* **Not compiled.** Written against SML 3.12 / UE 5.3 headers that were checked earlier, plus standard engine APIs; expect small compile fixes.
* **GitHub OAuth client id is empty** (`GitHubClientId` in `SharedWorldSubsystem.cpp`): the project owner must register a GitHub OAuth App with device flow enabled. Until then only folder storage works.
* **Installed-mod list** is not collected (the SML API was not verified), so worlds record no required mods.
* **Game build number** is taken from the engine changelist; must be confirmed to match save headers.
* **Successor readiness** is scored in SharedWorldCore (`HostElection` / `HostMigrationEngine`) and covered by headless tests; in-game exchange of readiness still needs live peer probes wired from the UE net driver.
* **Peer RTT matrix in game** uses injected/dev values until a `RealNetworkQualityProvider` probes Satisfactory sessions; the algorithms and simulator already consume the full matrix.
* **Peer recovery candidates** come from the host's own reports; clients cannot save the host's world.
* **Continue / Load Game** Shared-World awareness not yet wired (needs reliable last-played Shared World id); Load protection against split-brain is documented as follow-up in `docs/ui-integration.md`.
* Friends join via the game's own friends list; the mod does not open the platform invite dialog itself (API not verified).
* Exiting a hosted world without `/sharedworld stop` uploads the last completed save (checkpoints every 15 min). Graceful exit intercept (LEAVING SHARED WORLD) still uses the existing stop/migration path.

## Runtime validation needed (Phase I)

1. `LoadSaveFile` + `Start()` on the returned sequence: loads once, no double travel.
2. Session-id round trip across machines (`OnlineSessionIdToString` → `MakeOnlineSessionId` → `ResolveOnlineSession` → `JoinSession`), on Steam and Epic.
3. `UFGSaveSystem::GetSaveDirectoryPath()` is where `LoadSaveGameHeaderSync` looks (Steam vs Epic save folders).
4. `SaveGame` delegate fires after the file is closed (core waits for a stable file either way).
5. `ClientReturnToMainMenuWithTextReason` returns to Satisfactory's main menu (lease lost / handover / successor).
6. FHttpModule on worker threads; whether the game's libcurl forwards `Authorization` across a redirect host (the provider does not depend on it, but it must not leak).
7. Player id format from `GetPreferredUniqueNetId()` / `APlayerState::GetUniqueId()` is identical for the same account on host and clients.
8. Main menu: Shared Worlds under Join Game; Manage Session → Shared World; migration overlay during planned/crash handoff.

## Next steps

1. Compile in the SML starter project; fix compile errors.
2. Register the GitHub OAuth App; set the client id.
3. Two-PC test (Steam + Epic): create from a save, host, join, checkpoint, stop, crash the host, planned migration, restore.
4. Verify controller navigation on Shared Worlds entry + browser.
5. Wire Continue / Load Game Shared-World awareness when last-played id is reliable.
