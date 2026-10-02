# Shared Worlds UI system

One visual language for every Shared Worlds surface. The **Shared Worlds Browser** is the reference.
All code lives in `Source/SharedWorld/{Public,Private}/UI`. Backend (`SharedWorldCore`, `USharedWorldSubsystem`
state machine, leases, CAS, sync) is **not** touched by UI work; screens only read it and call its existing entry points.

## Design system (single source of truth)

`UI/SharedWorldUiStyle.h` (namespace `SharedWorldUi`) holds tokens and component factories. No screen may
introduce its own colours, radii, font sizes or button looks.

| Concern | Where |
|---|---|
| Colours / spacing / radii / font sizes | tokens at top of `SharedWorldUiStyle.h` (`SpaceS/M/L`, `RadiusS/M/L`, `FontTitle…FontSmall`) |
| Button hierarchy | `MakeRoleButton(ESharedWorldButtonRole::Game/Config/Secondary/Danger)`. Legacy `MakePrimaryButton`/`MakeSecondaryButton`/`MakeButton` forward to it |
| Status system | `ESharedWorldTone` → `ToneColor`, `MakeToneIcon` (shape per tone), `MakeStatusBadge` |
| Layout atoms | `MakePanel`, `MakeSectionHeader`, `MakeSettingsRow`, `MakeEmptyState`, `MakeNoticePanel`, `MakeProgressRow`, `MakeProgressBar` (marquee when no real %), `MakeStepper`, `MakeTabButton`/`StyleTabButton`, `StyleTextField`, `MakePlayerAvatar`, `MakePlayerRow` |
| Stateful components | `USharedWorldWorldCard` (row/tile), `USharedWorldSavePickRow`, `USharedWorldModal` (confirm/error, focus trap, Back cancels), `USharedWorldNoticeOverlay` (toast), `USharedWorldMigrationOverlay` |
| View model | `UI/SharedWorldBrowserModel` (status, tone, section, primary action, permissions, search) |
| Sub-pages | `SharedWorldBrowserPages.cpp` (welcome, create wizard, join, settings, GitHub link), `SharedWorldBrowserDetails.cpp` (world details) |

Status tone mapping (always shown with a **shape** as well as colour: dot = healthy, ring = inactive,
diamond = warning, square = problem, ringed dot = working):

* Healthy (green): Hosting, N Online, Host ready, Connected
* Working (blue): Syncing, Downloading, Uploading, Joining, Starting, Creating
* Warning (orange): Migrating host, Recovering, Needs recovery
* Problem (red): Needs attention (session error), Can't reach cloud
* Inactive (grey): Offline, Nobody hosting, Not linked

## Screen inventory and migration status

Legend: ✅ migrated · 🔶 partly · ⬜ not started · ❌ blocked on backend

| # | Surface | Widget / code | Component(s) | Status |
|---|---|---|---|---|
| 1 | Browser (list/grid, search, sections, details panel) | `USharedWorldBrowserWidget` (`EPage::Main`) | WorldCard, panels, roles | ✅ |
| 2 | Context menu `...` | `AddContextActions` | role buttons | ✅ |
| 3 | Create Shared World | `EPage::CreatePickSave → CreateName → CreateReview → Creating` | stepper, SavePickRow, settings rows, marquee progress | ✅ (description / permissions / icon ❌ backend) |
| 4 | Join Friend | `EPage::JoinFriend` (tabs Invites / Friend Worlds / Invite Code) | WorldCard, player rows, empty states | ✅ |
| 5 | World Details | `EPage::Details` (Overview / Players / Sync / Storage / Advanced) | tabs, player rows, stat plates | ✅ (no Activity tab: no event log) |
| 6 | Invite / member management | in-game `USharedWorldSessionWidget` Players tab | shell + buttons restyled; rows still legacy layout | 🔶 |
| 7 | World settings | none | — | ❌ (no per-world settings API) |
| 8 | Global settings | `EPage::Settings` (Storage / Sync / Diagnostics) | tabs, settings rows | ✅ (sync values are read-only; nothing unfinished is exposed as a toggle) |
| 9 | Storage / GitHub linking | `EPage::Settings` Storage tab (provider browser, see below), `EPage::LinkGitHub`, `EPage::Welcome` | `SharedWorldBrowserStorage.cpp`, `USharedWorldStorageProviderCard`, `SharedWorldStorageModel` | ✅ (UI only; GitHub is the only live provider) |
| 10 | Host migration overlay | `USharedWorldMigrationOverlay` | tone icon/edge, rounded bar | 🔶 (no candidate ranking list: backend only exposes a diagnostics string) |
| 11 | Save sync / upload | `ShowUploading` corner card | same overlay, marquee when % unknown | 🔶 (no bytes / speed from backend) |
| 12 | Recovery | status "Needs recovery" + Play | — | ❌ (backend offers only "Play to recover") |
| 13 | Sync conflict | none | — | ❌ (no conflict state exposed) |
| 14 | Error dialogs | session failures -> `USharedWorldModal` via `ShowSessionErrorDialog` (plain-language title, backend message, Details dropdown, Try Again if retryable); notices (version bump, returning to menu) stay non-blocking toasts | modal, notice panels | ✅ |
| 15 | Confirmations | Remove, Disconnect GitHub, Restore revision | `USharedWorldModal` (red confirm, Cancel focused first) | ✅ |
| 16 | Toasts | `USharedWorldNoticeOverlay` restyled; page `FlashText` lines remain | toast | 🔶 |
| 17 | Loading states | browser "Refreshing…" line, marquee bars | progress bar | 🔶 |
| 18 | Empty states | browser, invites, friends, members, saves | `MakeEmptyState` | ✅ |
| 19 | First-time setup | `EPage::Welcome` | stepper | ✅ |
| 20 | In-game Manage Session → Shared World | `USharedWorldSessionWidget` | cards, avatar player rows, orange Add, working tab highlight, refresh driven by `OnChanged` (+ one 5 s re-read of the live host ranking on the Overview tab) | ✅ |
| 21 | Main-menu / pause entry | `USharedWorldNativeMenu`, `MainMenuButton`, `SessionMenuButton` | stays native FG style | ✅ (intentional) |
| 22 | Legacy | `USharedWorldPanel` and `USharedWorldDetailsWidget` | removed (were only referenced by the module keep-alive list) | ✅ |

## Things the UI cannot show yet (need backend support, not faked)

* Per-world description, icon, permissions at creation (`CreateWorldFromSave` is members-only, name + save only).
* Compressed size / transfer speed / per-step creation progress (creation shows an honest marquee).
* Sync conflict state and recovery choices ("safe revision vs recovered revision").
* Activity timeline (no event log), per-player ping/ranking as structured data (only a diagnostics string).
* Per-world settings (preferred host, sync interval overrides).
* Sync Now / Repair local copy / Delete / Leave (distinct from "Remove from list").
* Player avatars (a shared initial-disc fallback is used everywhere; no platform fetch).

## Behaviour rules

* **Back** closes, in order: modal → "…" menu → wizard step / sub-page → browser → main menu (`HandleBack`, Esc / gamepad B).
* Destructive actions always go through `USharedWorldModal`.
* Backend/technical strings go to the log; players see a title, a sentence, a suggested action.
* Screens follow `USharedWorldSubsystem::OnChanged`. The create wizard and invite-code form ignore it (it would wipe typing).

## Known gaps / next work

* Controller: every design-system button is a `USharedWorldButton` (focus outline + focus forwarding); the modal traps focus; cards draw their own focus outline. Not yet verified with a real gamepad.
* Not yet verified in-game: 1080p / 1440p / ultrawide scaling of the new pages, gamepad navigation.
* rclone-backed providers: blocked on a design decision about leases/CAS (see the rclone note below).

## Settings > Storage (provider browser)

Layout (wide screens): left column = header + tabs + **Active Storage Provider** card + **Choose a Storage Provider** browser
(search, grid/list, Recommended / Other / View All, "powered by rclone" footer); right column = **selected provider** panel.
Narrow screens stack the selected-provider panel under the browser.

* Data: `FSharedWorldStorageCatalog::Build` -> `FSharedWorldStorageProvider` (ProviderId, DisplayName, Description, icon monogram/colour,
  Difficulty, bAvailable/bConnected/bRuntimeBacked, Account/Repository/Location, capability flags, ProviderTier).
  rclone discovery only needs to emit more of these; cards, grid, search and details read nothing else.
* Live (runtime-backed): GitHub connected state, account, repository, Test Connection (real call), Disconnect (real flow, confirmed), Reconfigure (re-runs real sign-in).
  "Last Checked" is set only by a Test Connection run in the current session; before that it says "Not checked this session".
* Preview metadata: capability rows are labelled "What this provider supports. Not a live check." Display-only providers show no capability values.
* Display-only: Google Drive, OneDrive, Dropbox, Cloudflare R2, Amazon S3, WebDAV, SFTP. Connect shows "Provider setup is not implemented yet."
* Icons: `USharedWorldStorageProviderCard::BuildIcon` (monogram tile). Replace its body with a brush lookup to add real logos.
* Not implemented (by request): rclone, OAuth, remotes, provider discovery, migration. "Change Provider" only scrolls to / focuses the picker.

## rclone and leases (design note, not implemented)

`IWorldRepository` has ONE atomic primitive, `Commit(ExpectedHead, Changes)` (compare-and-swap); leases, fencing and
revisions are all built on it. `IObjectStore` (the .sav bytes) is immutable and content-addressed and needs no CAS.
rclone can implement `IObjectStore` for any provider. It cannot provide a trustworthy CAS on providers without
conditional writes (notably Google Drive), so moving the *repository* (leases) onto such a provider would weaken the
split-brain guarantees. The storage UI is ready for either approach; the backend choice is pending.
