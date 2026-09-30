# FICSIT / SML API audit

Audit date: 2026-09-30. Scope: the whole mod, with the UE/Satisfactory layer
(`shared-world-mod/SharedWorld/Source/SharedWorld`) as the target and
`SharedWorldCore` as the fixed, proven core.

## 0. Evidence and its limits (read first)

| Item | Status |
|---|---|
| `docs.ficsit.app` | **Not reachable** from the audit environment (egress proxy blocks it; GitHub access is limited to this repo). No page was fetched in this audit. |
| What the doc/version conclusions rest on | (a) `docs/research.md`, written from the SML 3.12.0 sources and the `satisfactorymodding/Documentation` repository (September 2026); (b) web-search result summaries this session: SML **3.12.0** is a release for **Satisfactory 1.2** (UE upgrade, mods must be recompiled) and docs.ficsit.app has a page "Upgrading from SML 3.11.x to 3.12.x" (`.../latest/Development/UpdatingFromSml311.html`); the SML 3.11.x series targets Satisfactory 1.1. |
| Stable vs `/latest/` | Search results show version-selector entries `latest`, `v3.11.3`, `v3.10.0`, ... . Whether `latest` is currently the stable 3.12 docs or a development build **could not be confirmed**. The mod already targets SML `^3.12.0` / `GameVersion >=491125`, which matches the SML 3.12.0 release, so `latest` = 3.12 is assumed. |
| Game / SML headers | Not present in this environment. **The UE module cannot be compiled here.** Every UE-side change below is written against API usage already present in the repo (which was checked against the SML 3.12 headers earlier) and is marked *not compiled*. |
| Runtime | Nothing was run in the game. |

Consequences: no API in this audit was newly "discovered" from docs. Where a
finding says *No documented replacement found* it means: not found in
`docs/research.md` sources or in the repo's headers usage, and not verifiable
now. Those items need re-checking when docs access is available; they are
listed in `docs/ficsit-compatibility.md`.

## 1. API usage inventory

Status: GOOD / SHOULD REFACTOR / UNSUPPORTED-HACK / UNKNOWN / VERSION-SENSITIVE.

| API / class | Files | Purpose | Documented supported approach | Status |
|---|---|---|---|---|
| `UGameInstanceSubsystem` (`USharedWorldSubsystem`) | `SharedWorldSubsystem.{h,cpp}` | Runtime coordinator | UE subsystem lifetime = GameInstance; survives map travel, which is what a cross-world coordinator needs. Delegates added in `Initialize`, removed in `Deinitialize` (ticker, `OnWorldBeginTearDown`, `OnWorldInitializedActors`, `OnNetworkFailure`). | GOOD |
| SML root modules (`UGameInstanceModule`, `UMenuWorldModule`/`UGameWorldModule`, chat command) | `SharedWorldRootModules.cpp`, `SharedWorldChatCommand.cpp` | Entry points without Blueprint assets | `bRootModule` native classes, chat commands via `AChatCommandInstance` (docs.ficsit.app SML pages, per `research.md` §2) | GOOD |
| `UWidgetBlueprintHookManager::RegisterWidgetBlueprintHook` | `UI/SharedWorldGameInstanceModule.cpp` | Inject Shared Worlds into the main menu | The SML-supported widget extension point | GOOD (hook), but see UI rows |
| `FindFProperty` on FG widgets (`mMainMenuList`, `mManageSessionList`, `mDisplayName`, `mFrontEndButton`, `mContent`, ...) + `ProcessEvent` (`SetTitle`, `SetText`) | `UI/SharedWorldFgWidgets.cpp`, `SharedWorldNativeMenu.cpp`, `SharedWorldGameInstanceModule.cpp`, `SharedWorldBrowserWidget.cpp` | Drive vanilla widgets after injection | No documented API for these private widget members | UNSUPPORTED/HACK · VERSION-SENSITIVE (UI is not compiled and not run; the whole layer is a compat risk) |
| Hard-coded `/Game/FactoryGame/Interface/...` widget/font paths (`LoadClass`, `StaticLoadObject`) | `Public/UI/SharedWorldFgWidgets.h`, `SharedWorldNativeMenu.h` | Reuse vanilla button/background/fonts | Asset paths are game content, not API | VERSION-SENSITIVE |
| `UFGSaveSystem::LoadSaveGameHeaderSync`, `LoadSaveFile`, `SaveGame`, `GetAbsolutePathForSaveGame`, `GetSaveDirectoryPath` | `SharedWorldHostController.cpp`, `SharedWorldSubsystem.cpp` | Enumerate/load/save | Public `FACTORYGAME_API` (headers) | GOOD/VERSION-SENSITIVE; save location semantics UNKNOWN until run (see §3) |
| `FFGSavePlatform::GetUserSaveDirectoryPath` | `SharedWorldSubsystem.cpp` (`ResolveWritableSaveDirectory`) | Where new `SharedWorld_*.sav` are written | Header-exported | VERSION-SENSITIVE — must equal where `LoadSaveGameHeaderSync` reads (now checked at load, §3) |
| `UFGSessionSettings::mCurrentSessionDefinition` / `mSessionDefinitionName` (reflection write) | now `SharedWorldGameShims.cpp` | Force an online-capable session type before `LoadSaveFile` | `SetSessionDefinition`/`ApplySettingsModel` go through `UFGSessionSettingsModel`, which is not `FACTORYGAME_API`. **No documented replacement found.** | UNSUPPORTED/HACK · VERSION-SENSITIVE — isolated + validated this audit (§3) |
| Hard-coded `SessionDef_*` names | `SharedWorldGameShims.cpp` | Prefer a known online definition | Definitions come from `UOnlineIntegrationState::GetSessionDefinitions()`; names are game data | VERSION-SENSITIVE (fallback "first `bCreateOnlineSession`" now logs a warning) |
| `USessionMigrationSequence::Start()` after `LoadSaveFile` | `SharedWorldHostController.cpp` | Begin the load/travel | Sibling APIs require the caller to `Start()`; whether `LoadSaveFile` already starts it is UNVERIFIED (double start would double-travel) | UNKNOWN |
| `UCommonSessionSubsystem::MakeOnlineSessionId` → `ResolveOnlineSession` → `UCommonSessionStatics::JoinSession` | `SharedWorldJoinManager.cpp` | Join a friend's session by published id | The game's own OnlineIntegration abstraction (Steam/EOS/crossplay agnostic); `research.md` §4 | GOOD (round trip across stores UNKNOWN until run) |
| `FSessionJoinParams::RawAddress` + `CreateSessionJoiningSequence` | `SharedWorldJoinManager.cpp` | Address join | Same | GOOD |
| `ULocalUserInfo::GetGameSession()->GetSessionHandle()` polled every 2 s, 90 s timeout | `SharedWorldHostController.cpp` `TickPublishSession` | Discover our own session id → `HostReady` | A push callback/delegate for "session created" may exist in OnlineIntegration; **not found in the material available** | SHOULD REFACTOR (polling) — blocked on header access |
| `APlayerState::ExactPing`, `GameState->PlayerArray` | `SharedWorldNetworkQuality.cpp`, `SharedWorldHostController.cpp` | RTT, player list | Engine-provided | GOOD (engine stat) |
| Synthesised jitter (`ping/10`) and loss (`ping>200 ? 2%:0`) | `SharedWorldNetworkQuality.cpp` | Feed election scoring | Not engine measurements | SHOULD REFACTOR (P2) |
| `FHttpModule` on core worker threads with bounded wait | `SharedWorldHttpClient.cpp` | GitHub / cloud storage | Standard UE HTTP; blocking only on `sw::SerialQueue` workers | GOOD |
| `FPlatformProcess::Sleep` GitHub device-flow poll on `Async(Thread)` | `SharedWorldSubsystem.cpp` | OAuth polling | Off the game thread; interval honoured | GOOD |
| `GetEnvironmentVariable("SHAREDWORLD_GITHUB_CLIENT_ID")` + build-time `SHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED` | `SharedWorldSubsystem.cpp`, `SharedWorld.Build.cs` | OAuth client id (public, not a secret) | Developer override only; shipped builds bake the id | GOOD as a dev override |
| Windows Credential Manager | `SharedWorldCredentialStore.cpp` | GitHub token | Not in save/config/logs | GOOD |
| `%LOCALAPPDATA%/SatisfactorySharedWorld/settings.json` via core `LocalSettings` | `SharedWorldSubsystem.cpp`, core | Player settings | SML config system (`UModConfiguration`) is asset-based (needs content). Mod has `CanContainContent:false`. **Not changed**: needs editor-authored assets that cannot be produced or verified here | SHOULD REFACTOR (P3), blocked on editor |
| `.uplugin` | `SharedWorld.uplugin` | Metadata | `SemVersion ^3.12.0`, `GameVersion >=491125`, `RequiredOnRemote`, `RemoteVersionRange`, `TargetDenyList Server` | GOOD; see §5 |
| Log category `LogSharedWorld` only, structured `[SharedWorld/Area] event=...` text | everywhere | Logging | Single UE category; areas are text tags | SHOULD REFACTOR (P3): real categories (`SharedWorldLease`, `...Join`, ...) enable per-area verbosity |

## 2. Findings and evidence blocks

### F1 — Host verifier: dangling promise, wrong thread, UObject access off-thread — **P0, FIXED (not compiled)**

* **Current implementation:** `FSharedWorldUEHostVerifier::Probe` (`SharedWorldUEHostVerifier.cpp`), run on a `SerialQueue` worker by `WorldSession::DoVerifyHost`.
* **Problem:** (1) `ResolveOnlineSession(...).Next([&Promise]...)` captured a stack `std::promise` by reference; on a probe timeout the function returned and the late completion wrote into a destroyed object (undefined behaviour / crash while a friend joins). (2) `GetSubsystem`, `ResolveOnlineSession`, `USessionInformation` handle reads and `TWeakObjectPtr::Get()` on the responder ran on a worker thread. (3) `DebugVerifyHost` (`/sharedworld verify`) called `Probe` and a blocking cloud `Load()` on the game thread.
* **Documentation:** Unreal threading rules (UObjects/GC are game-thread); `ResolveOnlineSession` returns a `TFuture<USessionInformation*>` (`research.md` §4). No page states the future's completion thread, so the fix does not assume one.
* **Recommended implementation (done):** all UObject work in one `AsyncTask(GameThread)` hop; only plain data crosses back; promise held by `shared_ptr` captured by value; `Probe` refuses to run on the game thread (`probe_called_on_game_thread`); `DebugVerifyHost` is now asynchronous (`FDone`), the chat command replies via the existing `Reply` path; `ObserveVerifySample` dispatched to the game thread.
* **Risk:** Low. **Behaviour change:** `/sharedworld verify` reply arrives asynchronously.
* Guarantees: verification still never grants or breaks a lease (`WorldSession::DoVerifyHost` re-reads the cloud lease after the probe; failures only retry or wait for expiry).

### F2 — Blocking cloud reads on the game thread every overlay update — **P0/P3, FIXED (not compiled)**

* **Current implementation:** `ObservePlannedHostLeave`, `ComputeMigrationProgress`, `IsHostMigrationInFlight` in `SharedWorldSubsystem.cpp` called `WorldStore::Load()` (GitHub/folder network I/O) from the 1 Hz tick and overlay updates.
* **Problem:** a slow or failing GitHub call stalls the frame loop for the HTTP timeout, during exactly the moment (host migration) the player is watching the game.
* **Documentation:** none needed beyond the game-thread rule; the mod's own docs already state "Creation, history, membership and invites run on a background queue".
* **Recommended implementation (done):** `sw::RefreshCache<StateSnapshot>` (new, in `SharedWorldCore/Util/RefreshCache.h`, unit-tested) per runtime; `USharedWorldSubsystem::RefreshCloudCache` refills it on the background queue at most every 2 s; readers only call `Get()`. Cache is cleared on a failed read (never serves stale success).
* **Risk:** Low. **Behaviour change:** overlay state may lag the cloud by up to ~2 s (it already polled at 1 s).

### F3 — Session-definition reflection hack — **P1, ISOLATED (kept)**

* **Current implementation:** was `EnsureHostingSessionDefinition` in `SharedWorldHostController.cpp`; now `SharedWorldShim::EnsureHostingSessionDefinition` in `SharedWorldGameShims.{h,cpp}`.
* **Problem:** private member writes by name; a game update that renames or retypes them would fail silently or write the wrong type; no diagnostics.
* **Documentation:** No documented replacement found. Public `SetSessionDefinition`/`ApplySettingsModel` are unusable because `UFGSessionSettingsModel` is not `FACTORYGAME_API` (`SharedWorldHostController.cpp` comment, from the SML headers).
* **Recommended implementation (done):** keep the workaround, but validate `FObjectProperty::PropertyClass` against the definition, verify through the public `GetCurrentSessionDefinition()` after writing, **roll back** both members if the write did not take, return a one-line reason to the caller (logged in `host_load_failed`), warn when the arbitrary "first online-capable definition" fallback is used, and warn once when the running build differs from the audited changelist (`491125`).
* **Risk:** Low. **Behaviour change:** hosting now fails cleanly (with reason) instead of proceeding with an unverified single-player definition.

### F4 — Save path determinism — **P0, GUARDED (not compiled)**

* **Current implementation:** the core writes `SharedWorld_<id>.sav` to `ResolveWritableSaveDirectory` (`FFGSavePlatform::GetUserSaveDirectoryPath`, fallback `GetSaveDirectoryPath`); `BeginHosting` then loads by **name** (`LoadSaveGameHeaderSync`, `LoadSaveFile`).
* **Problem:** if the game's by-name lookup resolves a different folder than the one we wrote (Steam vs Epic layouts, id subfolders, common vs user directory — `research.md` §3), a **stale `SharedWorld_<id>.sav` from another folder could be hosted** instead of the verified download: wrong-world/old-revision hosting.
* **Documentation:** `UFGSaveSystem::GetAbsolutePathForSaveGame` (public) is the game's own name→path resolver.
* **Recommended implementation (done):** before `LoadSaveGameHeaderSync`, resolve the name with `GetAbsolutePathForSaveGame` and refuse (`host_load_failed … save path mismatch`, both paths logged) if it is not the file we wrote; if the game cannot resolve it, log `save_path_unverified` and continue. No paths are computed by hand for loading.
* **Risk:** Medium (a false mismatch, e.g. path canonicalisation on Windows, blocks hosting — `FPaths::IsSamePath` is used on absolute paths). **Behaviour change:** refuses to host on mismatch. Runtime validation item #3 in `STATUS.md` still applies.
* **Still open:** `ResolveExistingSavePath`'s recursive `FindFilesRecursive(...)[0]` fallback (used when *importing a personal save*) is order-dependent if two saves share a name.

### F5 — Core build was red under GCC 13 Release — **P0 (CI parity), FIXED**

`Save/SaveFile.cpp` returned `-Werror=maybe-uninitialized` (`Compressed`, `Uncompressed`, `LastSize`, `Age`, ...). `Reader::I64` results were unchecked and the locals uninitialised. Fixed by initialising and checking the reads (a short header now fails as `truncated chunk header`). Behaviour identical for valid saves (all 24 conformance saves still agree).

### F6 — Host verifier semantics — **P2, DOCUMENTED, not changed**

`Probe` "Verified" for a resolvable session builds the Ack from the Hello's own fields (revision, generation, nonce echo), and the empty-join case returns "Verified" (`friends_list_fallback`). It therefore proves *session discoverability only*, not lease generation/revision. This is acceptable for the current guarantee set because verification never grants or breaks a lease, but the log name `attempt_rtt_ms`/`ObserveVerifySample` feeds a *cloud resolve latency* into the election as if it were a host RTT. Real per-peer RTT comes from `APlayerState::ExactPing` once connected. No documented pre-join lobby/ping query was found; custom sockets were not added.

### F7 — Network quality inputs — **P2, DOCUMENTED**

`SharedWorldNetworkQuality.cpp` derives jitter and loss from ping (`ping/10`, `>200 ms → 2 %`). The election is deterministic but these are not measurements. Recommended: only feed fields the engine actually measures, and mark others "unknown" with a neutral weight — needs a core change to `LinkSample` (optional fields) plus tests, deferred.

### F8 — Host-ready by polling — **P2, BLOCKED**

`TickPublishSession` polls `GetGameSession()` every 2 s for 90 s, then publishes "HostReady without join data". A delegate-driven signal would remove the guess, but no supported callback is verifiable here. Kept; the timeout path is already safe (`friends_list_fallback`).

### F9 — Checkpoint interval — **P2, DOCUMENTED**

Hard-coded 5 min in `SharedWorldHostController.cpp` (STATUS/research text says 15 min). Should be a setting (`CheckpointIntervalSeconds` in the core `LocalSettings`) — needs a settings-UI change; deferred (§4).

### F10 — SML config — **P3, BLOCKED ON EDITOR**

SML's configuration system is Blueprint-asset-based (`UModConfiguration`), which needs `CanContainContent: true` and editor-authored assets. Not producible or verifiable here. Keep the existing `settings.json`; client id stays a dev-only env override; secrets stay in Credential Manager.

### F11 — UI extension — **P3/P1**

The hook registration is the supported route; everything after it (`FindFProperty` on private FG widget members, `ProcessEvent("SetTitle")`, fixed asset paths) is undocumented and the top compatibility risk after a game update. Not touched: no way to compile/run UI here. Recommended follow-up: centralise every reflected FG-widget access behind one shim like `SharedWorldGameShims`, each with a "missing → log and degrade" path.

## 3. Save / hosting / join lifecycle (traced)

```
WorldSession(ReadyToHost) ─ core downloads + verifies revision → SharedWorld_<id>.sav (core writes; hashes verified)
  → BeginHosting (game thread)
       GetAbsolutePathForSaveGame(name) == written path?      [NEW guard]
       LoadSaveGameHeaderSync(name)
       EnsureHostingSessionDefinition                          [isolated shim, validated, rollback]
       LoadSaveFile(header) → Sequence->Start()               [start semantics UNKNOWN]
  → OnGameWorldReady → TickPublishSession (poll ≤90 s) → OnHostingStarted(join) → lease HostReady
Client: lease joinable → probe (session resolve, off game thread) → ResolveOnlineSession → JoinSession
```

Original personal saves are never opened for writing here; the runtime save is always `SharedWorld_<worldId>`.

## 4. Priorities

| Pri | Item | State |
|---|---|---|
| P0 | F1 verifier lifetime/thread | fixed |
| P0 | F2 game-thread cloud I/O | fixed |
| P0 | F4 save-path guard | fixed |
| P0 | F5 core build red | fixed |
| P1 | F3 session-definition shim | isolated + validated |
| P1 | UI reflection (F11), `Start()` semantics, session-id round trip | needs game/headers |
| P2 | F6/F7/F8/F9 | documented; need core `LinkSample` change, settings, header access |
| P3 | Log categories, SML config | deferred |

## 5. Packaging / dependencies

* `.uplugin`: SML `^3.12.0` is the only mod dependency — correct, nothing reimplemented from other mods. `RequiredOnRemote: true` with `RemoteVersionRange` is right (chat actor and menu need every player to have the mod). `Server` targets denied. **Verify** on the docs release-mod page that `SemVersion ^3.12.0` and `GameVersion >=491125` match the *stable* SML 3.12.x/game 1.2 build before publishing to SMR (not verifiable here).
* Build.cs depends on `OnlineIntegration`, `OnlineServicesInterface`, `CoreOnline`, `OnlineSubsystem`, `OnlineSubsystemUtils` — the last two appear unused (only OnlineIntegration types are included); confirm at compile time before removing.
* No Alpakit packaging was possible here.

## 6. Verification performed

* `SharedWorldCore` + tests: 153 tests, 0 failed (GCC 13, Release; was 152 + a failing build before this audit).
* UE module: **not compiled** (no headers). All UE edits reviewed by hand; expect small compile fixes.
