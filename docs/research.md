# Milestone 1 — Research findings

Everything below was checked against source, not assumed. Where something
could only be confirmed at compile level (the header exists and is
exported) and not at runtime (it does what we need in the running game),
it is marked **UNVERIFIED AT RUNTIME**.

Sources inspected (September 2026):

| Source | Version |
|---|---|
| This repo (`Page` branch of Vojtak42/Satisfactory-multiplayer-cloud-sync) | commit `046cae7` |
| `satisfactorymodding/SatisfactoryModLoader`, branch `dev` | commit `ae723cf` (2026-09-26), SML **3.12.0**, `GameVersion >=491125` |
| `satisfactorymodding/Documentation` (source of docs.ficsit.app) | `main`, shallow clone |
| `@etothepii/satisfactory-file-parser` (npm) | 4.1.2, used to cross-check the save format |

The SML repository ships the game's stripped C++ headers
(`Source/FactoryGame/Public`) and the game's `OnlineIntegration` plugin
headers (`Plugins/Online/OnlineIntegration`). These are the same headers
mods compile against, so an API found there is one a mod can call.

---

## 1. The previous BAT workflow (historical)

The original project used rclone batch scripts that:

1. Checked for an `online` lock folder — if present, "someone is online", stop.
2. Created the lock folder.
3. Renamed the player's own save folder aside, synced the cloud `MP`
   folder down, renamed it into the player's save folder.
4. Started the game, polled until it exited, slept 30 s.
5. Synced the folder back up, removed the lock, restored the player's own saves.

Concepts worth keeping: keep multiplayer saves separate from the player's
own saves; download before playing and upload after; any cloud provider;
only one host at a time.

Problems the successor must fix:

| Problem | Consequence |
|---|---|
| `ls` then `mkdir` is check-then-act, not atomic | two players starting together both "win" and both host |
| lock folder never expires | a crash or closed console window locks everyone out until someone deletes it by hand |
| no revisions, no hashes | a stale copy uploads over newer progress, silently |
| `rclone sync` mirrors (deletes remote files not present locally) | an empty or wrong local folder wipes the cloud copy |
| upload of whatever is on disk after a fixed 30 s wait | may upload a half-written save |
| renames the player's real save folder | a crash mid-script leaves the player's own saves renamed |
| "someone is online" gives no host/session info | the player must find and join the host manually |

Nothing from the scripts is reused as code. The successor is a new design.

## 2. SML architecture relevant to this mod

* **Mod modules.** `UGameInstanceModule`, `UMenuWorldModule` (main menu
  only) and `UGameWorldModule` (loaded save or joined session) with
  `DispatchLifecycleEvent(CONSTRUCTION | INITIALIZATION | POST_INITIALIZATION)`
  (`Mods/SML/Source/SML/Public/Module/*.h`).
* **Native C++ root modules work.** `PluginModuleLoader.cpp` collects
  native classes with `FindNativeClassesByType` and keeps those whose CDO
  has `bRootModule == true`. So the mod needs no Blueprint assets for its
  entry points.
* **Widget Blueprint Hooks** (`Patching/WidgetBlueprintHookManager.h`):
  `UWidgetBlueprintHookManager::RegisterWidgetBlueprintHook(UWidgetBlueprintHookData*)`
  inserts a widget into a game widget archetype. SML's own main-menu "Mods"
  button targets `/Game/FactoryGame/Interface/UI/Menu/MainMenu/BP_MainMenuWidget`
  with `EWidgetBlueprintHookParentType::Indirect_Child`; the SML asset's
  name table contains `mMainMenuList`, `mOptionsList` and `mSwitcher`, but
  which one is the parent could not be read from the binary asset.
  → The first slice shows the panel as a **viewport overlay** from the menu
  world module (guaranteed to work) and leaves the in-menu-list hook for
  when it can be verified in the editor.
* **Chat commands** (`Command/ChatCommandInstance.h`): `AChatCommandInstance`
  registered through `UGameWorldModule::mChatCommands`; executes on the
  server; `UCommandSender::GetPlayer()` identifies the caller. Used for
  in-game host controls (`/sharedworld save|stop|status`) until pause-menu
  buttons exist.
* **Multiplayer mod matching** (docs `ReleaseMod.adoc`, `UpdatingFromSml38/39.adoc`):
  `RequiredOnRemote` (default `true`) and `RemoteVersionRange` in the
  `.uplugin`. When a client connects the host checks the client's mod list;
  since SML 3.9 the client also refuses hosts missing its mods.
* **Build setup** follows `SML.Build.cs`: C++20, `FactoryGame` + `SML`
  public deps. `FactoryGame.Build.cs` itself depends on `HTTP`, `Json`,
  `JsonUtilities` and `OnlineIntegration`, so those are available to mods.
* docs.ficsit.app is blocked from this environment; its source repo was read instead.

## 3. Satisfactory save lifecycle

From `Source/FactoryGame/Public/FGSaveSystem.h`, `FGSaveManagerInterface.h`,
`FGSaveSession.h`, `FGGameState.h`, `FGGameMode.h`:

| Need | API (all public, `FACTORYGAME_API`) |
|---|---|
| Save directory | `static FString UFGSaveSystem::GetSaveDirectoryPath()` — **ask the game; never hardcode** |
| Absolute path of a save | `static bool UFGSaveSystem::GetAbsolutePathForSaveGame(const UWorld*, const FString& saveName, FString& out)` |
| Read a header | `bool UFGSaveSystem::LoadSaveGameHeaderSync(const FString& saveName, FSaveHeader&) const` |
| Load a save as host | `USessionMigrationSequence* UFGSaveSystem::LoadSaveFile(const FSaveHeader&, const FLoadSaveFileParameters&, APlayerController*)` |
| Save now, with completion | `void UFGSaveSystem::SaveGame(const FString& SaveName, FOnSaveMgrInterfaceSaveGameComplete)`; delegate is `(bool Success, const FText& ErrorMessage)` |
| Autosave | `UFGSaveSession::SetupAutosave`, rotating autosaves (`GetNumRotatingAutosaves`), `AFGGameState::mOnAutoSaveFinished` (BlueprintAssignable) |
| Session name | `AFGGameMode::GetSaveSessionName()`, `FSaveHeader::SessionName` |
| Save location quirks | `SaveFileExistsInCommonSaveDirectory`, `MoveSaveFileFromCommonToEpicLocation` exist: the game distinguishes a common and an Epic-specific save location |

Decisions that follow:

* **When is a save flushed?** The only first-party signal is the
  `SaveGame` completion delegate. The mod reports a save to the helper only
  from that callback. The helper additionally (a) waits until size/mtime
  stop changing and (b) fully validates the file (below) before uploading.
  Whether the delegate fires strictly after the file is closed is
  **UNVERIFIED AT RUNTIME**; the two helper-side checks make the design
  safe either way.
* **Autosaves** write their own files (`<session>_autosave_N`). The shared
  world uses its own save name (`SharedWorld_<worldId>`), written only by
  explicit `SaveGame` calls from the mod (checkpoints every 15 min and on
  stop), so autosave timing cannot corrupt an upload.
* **Save file format** (verified in `FSaveHeader::Type` and against
  satisfactory-file-parser 4.1.2): little-endian header — header version,
  save version, build version, [save name ≥ v14], map name, map options,
  session name, play duration, save ticks (int64), visibility (byte),
  editor object version, mod metadata + modded flag (≥ v8), save
  identifier (≥ v10), partition flag (≥ v11), MD5 checksum block (≥ v12),
  creative flag (≥ v13) — followed by zlib chunks, each starting with tag
  `0x9E2A83C1`, a v1/v2 chunk header (48/49 bytes) and compressed data.
  The first 4 bytes of the inflated body declare the body size
  (+4, or +8 for save version ≥ 37). The helper parses the header, inflates
  every chunk and checks the declared size, so **a save truncated by a
  crash mid-write is detected and never uploaded**.
* The common-vs-Epic save directory split means the correct directory can
  differ per store; getting it from `GetSaveDirectoryPath()` at runtime is
  the right call. Whether that returns the directory `LoadSaveGameHeaderSync`
  reads from on Epic builds is **UNVERIFIED AT RUNTIME**.

## 4. Session discovery and joining

The game uses its `OnlineIntegration` plugin (built on UE5 Online Services,
`OnlineServicesInterface`, with `EOSShared`). Relevant exported API
(`Plugins/Online/OnlineIntegration/Source/OnlineIntegration/Public`):

| Capability | API |
|---|---|
| Get the local user | `UOnlineIntegrationSubsystem::GetOnlineIntegrationState()->GetFirstUserInfo()` → `ULocalUserInfo*` |
| Host's current session | `ULocalUserInfo::GetGameSession()` → `USessionInformation*`; `->GetSessionHandle()` → `FCommonSession`; `.GetSessionId()` |
| Session id ⇄ string | `static FString UCommonSessionSubsystem::OnlineSessionIdToString(FOnlineSessionId)`; `static FOnlineSessionId MakeOnlineSessionId(const FString&)` |
| Resolve an id to a joinable session | `TFuture<USessionInformation*> UCommonSessionSubsystem::ResolveOnlineSession(ULocalUserInfo*, FOnlineSessionId, ...)` |
| Join a resolved session | `static USessionMigrationSequence* UCommonSessionStatics::JoinSession(APlayerController*, USessionInformation*, const FJoinSessionResponse&)` (BlueprintCallable) |
| Join by address | `UCommonSessionSubsystem::CreateSessionJoiningSequence(const FSessionJoinParams&)` with `FSessionJoinParams::RawAddress`, then `USessionMigrationSequence::Start()` |
| Friend's session | `TFuture<USessionInformation*> UCommonSessionSubsystem::FetchFriendSession(UOnlineFriend*, UOnlineIntegrationBackend*)`; friends via `ULocalUserInfo::GetFriends()` |
| Invites / platform join | `JoinStartupSession`, `SetPendingJoinRequest`, `OnProtocolActivationReceived(protocolUri, ...)` (comment: console shell joins) |
| Legacy OSS v1 path | `UFGGameInstance::JoinSession(ULocalPlayer*, const FOnlineSessionSearchResult&)` |

Answers to the questions asked:

* **Detect an active multiplayer session** — yes: `ULocalUserInfo::GetGameSession()`
  on the host; `USessionInformation::Members`/`Host` hold members.
* **Discover/join a friend** — yes via `FetchFriendSession` +
  `JoinSession`, but it needs the friend object and backend; we do not need it
  because the host publishes its session id through the shared-world record.
* **Steam / EOS session data** — sessions are Online Services sessions
  with backend links (`UOnlineSessionBackendLink`, "mirror sessions" in
  `UOnlineIntegrationControllerComponent`), i.e. an EOS session plus
  platform mirrors. The string produced by `OnlineSessionIdToString`
  identifies the session independent of the local store.
* **Invoke joining directly** — yes: `UCommonSessionStatics::JoinSession`
  and `CreateSessionJoiningSequence` are the game's own entry points.
* **Connection URL / deep link** — `FSessionJoinParams::RawAddress` accepts
  an address. `OnProtocolActivationReceived` exists but is documented for
  console shell activation; not used.

**Chosen join design (first slice):**

1. Host (mod): once the hosted world exists, read the session id via
   `GetGameSession()->GetSessionHandle().GetSessionId()`, convert with
   `OnlineSessionIdToString`, publish it to the helper → cloud record.
2. Client (mod): `MakeOnlineSessionId(string)` → `ResolveOnlineSession`
   → `UCommonSessionStatics::JoinSession`.
3. If the host cannot obtain a session id within 90 s, it reports
   "hosting, no join data"; clients get a JOIN decision telling them to use
   the game's friends list. They never start a second copy.

**UNVERIFIED AT RUNTIME:** that the session-id string round-trips between
machines/stores, that `LoadSaveFile`'s returned sequence must be `Start()`ed
by the caller (sibling APIs say so), and that `JoinSession` starts its own
sequence. These are the top blockers in `STATUS.md` and need one in-game
test with two accounts.

## 5. Multiplayer compatibility requirements

* **Every player needs the mod.** It adds a replicated chat-command actor
  and is the only thing that shows "Play Shared World". The `.uplugin` sets
  `RequiredOnRemote: true` and `RemoteVersionRange: ">=0.1.0 <0.2.0"`, so
  SML refuses mismatched clients with a clear message instead of desyncing.
* **Every player who presses Play needs the helper**, including clients:
  whether to host or join, and the join data, come from the shared-world
  record in the cloud, which only the helper reads. A friend who joins
  through the friends list without the helper is fine as long as they have
  the mod.
* **Steam vs Epic.** The mod never uses store-specific APIs; it uses the
  game's Online Services layer and the store-neutral session id string.
  Store detection is only informational (`FUniqueNetIdRepl::GetType()`).
  Cross-store joining through that layer is **UNVERIFIED AT RUNTIME**.
* **Dedicated servers** are out of scope; the module is denied on `Server` targets.

## 6. Cloud storage: what the lease needs

The design needs one primitive from the metadata store: an atomic
compare-and-swap write of a small object. Blobs only need create-only
writes. Provider capabilities, from the providers' API documentation
(to be re-checked when each provider is implemented):

| Provider | CAS primitive for the record | Usable for the lease |
|---|---|---|
| S3 / R2 / MinIO | `PutObject` with `If-Match` / `If-None-Match` (conditional writes) | yes |
| Dropbox | `files/upload` with `mode: update:<rev>` | yes |
| OneDrive (Graph) | `PUT` with `If-Match: <eTag>` | yes |
| WebDAV | `If-Match` ETag on `PUT` (server-dependent) | yes, if the server honours it |
| Google Drive v3 | no documented conditional update | **no** — could store blobs, but needs another backend for the record |
| Sync-client folders (Drive/OneDrive/Dropbox desktop apps) | none: files replicate eventually | **no** |

This is why storage is split into `MetadataStore` (CAS) and `BlobStore`
(immutable blobs). Implemented today: in-memory (tests) and filesystem
(single machine / real network share). Cloud providers are the next milestone.
