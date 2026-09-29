# Shared Worlds UI integration

_Last updated: 2026-09-29_

## Goal

Shared Worlds must feel like a first-party Satisfactory feature: entries inside
the existing main menu and Manage Session lists, browser/details screens that
reuse Satisfactory-like layout tokens, and migration/recovery overlays driven
by **SharedWorldCore** state (never decorative UI-only values).

The old corner viewport overlay (`USharedWorldPanel` parented into the main-menu
canvas) is **disabled by default**. It remains in the tree only as a fallback
development surface if hooks fail at runtime.

## Hook points (current supported SML / FactoryGame)

| Surface | Game widgets (best effort) | Integration |
|---|---|---|
| Main menu list | `BP_MainMenuWidget` → `mMainMenuList` | SML `UWidgetBlueprintHookManager` inserts `USharedWorldMainMenuButton` |
| Main menu order | Label **"Join Game"** | Runtime inject: find label, insert **Shared Worlds** immediately after |
| Pause / Manage Session | `Widget_GameMenu` → `mManageSessionList` (Indirect_Child) | Hook inserts `USharedWorldSessionMenuButton` |
| Pause fallback | Labels **"Manage Players"** / **"Session Settings"** | Runtime inject under that list when the pause UI is open |

### Why dual path (hook + runtime inject)

`WidgetBlueprintHook` parent widget names drift across Satisfactory updates.
SML's own Mods button historically targeted `mMainMenuList`; that name is the
primary hook target here. Runtime inject walks the live widget tree for exact
English labels (`Join Game`, `Manage Players`) so a renamed internal slot does
not silently leave Shared Worlds missing.

### What is still unverified in-game

* Exact cooked parent of Manage Session entries (`mManageSessionList` vs a
  nested list). Documented as best-effort; runtime label inject covers the gap.
* Whether main-menu list slots are `UVerticalBox` children or custom FG list
  widgets wrapping buttons — inject searches both.
* Controller focus hand-off when opening C++ UMG browsers on top of FG menus
  (IsFocusable / navigation wrapping needs a live pad test).
* Continue / Load Game Shared-World awareness: API ready via
  `GetMostRecentlyPlayedWorldId` / `TryContinueLastSharedWorld`; not hooked into
  FG Continue/Load until last-played vs FG save order can be compared in game.

## UI layout (code)

```
UI/
  SharedWorldGameInstanceModule   — hooks + inject
  MainMenu/
    SharedWorldMainMenuButton     — "Shared Worlds" list entry
    SharedWorldBrowserWidget      — world list / create / refresh
    SharedWorldDetailsWidget      — PLAY / players / history / restore
    SharedWorldWorldCard          — per-world card + PLAY / VIEW
  SessionMenu/
    SharedWorldSessionMenuButton  — "Shared World" under Manage Session
    SharedWorldSessionWidget      — overview / host / players / history / backups
  Migration/
    SharedWorldMigrationOverlay   — HOST MIGRATION / HOST CONNECTION LOST
  Components/
    SharedWorldUiStyle            — colours / badge mapping / button helpers
```

All screens call `USharedWorldSubsystem` only. Host ranking text comes from
`GetHostMigrationDiagnostics` → `sw::FormatDiagnosticsText` (same ranking the
migration engine uses). PLAY calls `Play()`. MIGRATE HOST calls
`RequestMigrationTo(..., "auto")` which uses `SelectSuccessor` / HostMigrationEngine.

## Migration / recovery overlay

`USharedWorldSubsystem::UpdateMigrationOverlay` runs each tick for the active
world. Prefer `HostMigrationEngine::Diagnostics().OverlayMessage`; fall back to
`SessionState::Migrating` / `Reconnecting` messages. On network failure while
joined, the recovery overlay is shown before `OnHostConnectionLost`.

## Manual verification checklist

**Main menu**

1. After Join Game, Shared Worlds appears as a list entry (not Mods, not overlay).
2. Selecting it opens the browser; world cards show status from core.
3. PLAY invokes `PlaySharedWorld` path (`Subsystem::Play`).
4. CREATE SHARED WORLD / SETTINGS expands create + GitHub sign-in.
5. VIEW opens details; BACK returns to the menu.
6. Keyboard/controller can move to Shared Worlds and activate it.

**In game**

1. Pause → Manage Session → Shared World.
2. Overview shows active session + preferred successor from diagnostics.
3. Players / History tabs load via `FetchPlayers` / `FetchHistory`.
4. MIGRATE HOST / SAVE & SYNC / STOP HOSTING call real subsystem actions.
5. Kill host or `/sharedworld` crash inject → recovery overlay, not only FG disconnect.

## Continue / Load Game

* `TryContinueLastSharedWorld()` / `GetMostRecentlyPlayedWorldId()` are ready for a
  Continue hook: prefer Play on the latest Shared World over a stale local save.
* `IsSharedWorldSaveName` / `WorldIdFromSaveName` identify `SharedWorld_<id>` saves
  so a future Load-menu patch can offer **PLAY SHARED WORLD** instead of hosting
  an independent authoritative copy. Not hooked yet — FG Load list widgets need
  a verified inject point in game.

If a hook cannot be registered (`UWidgetBlueprintHookManager` missing), log
`menu_hook_manager_missing` and rely on runtime inject from
`OnMenuWorldReady` / periodic in-game `TryEnsureMenuEntries`.

Do **not** re-enable the corner overlay as the primary UX without updating this
document and STATUS.md.
