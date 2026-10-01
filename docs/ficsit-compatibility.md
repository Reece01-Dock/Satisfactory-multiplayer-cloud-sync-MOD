# FICSIT compatibility tracker

Update this file on every Satisfactory / SML upgrade.

| Field | Value |
|---|---|
| Satisfactory version | 1.2 (`GameVersion >=491125` in `SharedWorld.uplugin`) |
| SML version | 3.12.x (`SemVersion ^3.12.0`) |
| Docs version | `docs.ficsit.app/satisfactory-modding/latest` — **stable-vs-development not confirmed** (site unreachable during the 2026-09-30 audit); upgrade page: `.../latest/Development/UpdatingFromSml311.html` |
| Last audited | 2026-09-30 (`docs/ficsit-api-audit.md`) — source/headers review; UE module **compiles** (Alpakit Dev Shipping); still no in-game run |

## Known internal / reflection dependencies (HIGH RISK)

| Dependency | Where | Failure mode |
|---|---|---|
| `UFGSessionSettings::mCurrentSessionDefinition`, `mSessionDefinitionName` (reflected write) | `SharedWorldGameShims.cpp` | Validated + rolled back; hosting refuses with a reason |
| `SessionDef_*` names | `SharedWorldGameShims.cpp` | Falls back to first `bCreateOnlineSession` definition (warns) |
| FG widget members `mMainMenuList`, `mManageSessionList`, `mManagePlayers`, `mSessionSettings`, `mDisplayName`, `mFrontEndButton`, `mContent`; functions `SetTitle`, `SetText` | `UI/SharedWorldFgWidgets.*` (callers in `UI/*`) | Menu entries missing / mis-labelled; reads return null/empty |
| Widget/font asset paths under `/Game/FactoryGame/Interface/...` | `Public/UI/SharedWorldFgWidgets.h`, `SharedWorldNativeMenu.h` | Load returns null; UI degrades |
| Main-menu Shared Worlds entry | `UI/SharedWorldGameInstanceModule.cpp` (WidgetBlueprintHooks) | SML-style Direct hooks inject FrontEnd button + browser into shipping `BP_MainMenuWidget`; no FactoryGame BP pack required |
| Manage Session `Widget_ManageSession` (+ baked `mSharedWorld` / `SharedWorldSession`) | same | Same for pause menu |

## Version-sensitive public APIs (MEDIUM)

`UFGSaveSystem` (`LoadSaveGameHeaderSync`, `LoadSaveFile`, `SaveGame`, `GetAbsolutePathForSaveGame`, `GetSaveDirectoryPath`),
`FFGSavePlatform::GetUserSaveDirectoryPath`, `USessionMigrationSequence::Start`,
`UCommonSessionSubsystem` (`MakeOnlineSessionId`, `ResolveOnlineSession`, `OnlineSessionIdToString`, `CreateSessionJoiningSequence`),
`UCommonSessionStatics::JoinSession`, `ULocalUserInfo::GetGameSession`, `UOnlineIntegrationState::GetSessionDefinitionByName/GetSessionDefinitions`,
`AFGPlayerController::GetGameUI()->IsPauseMenuOpen`, `ClientReturnToMainMenuWithTextReason`.
Game build number = engine changelist (unconfirmed against save headers).

## Documented SML interfaces (LOW)

Root modules, `AChatCommandInstance`/`UCommandSender`, plugin descriptor fields.
(WidgetBlueprintHooks inject the Shared Worlds FrontEnd/Extensions button + browser page, same pattern as SML Mods.)

## Open runtime validations

See `STATUS.md` "Runtime validation needed". Added by the audit: save-path guard false positives (`save path mismatch` log), `probe_called_on_game_thread` must never appear in logs.
