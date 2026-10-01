# Shared Worlds UI integration

_Last updated: 2026-09-30_

## Goal

Shared Worlds must feel like a first-party Satisfactory feature: entries inside
the existing main menu and Manage Session lists, using FrontEnd buttons +
`WidgetSwitcher` like Join Game / Mods.

**No viewport overlays.** Browser and session screens open only as menu switcher pages.

## How SML Mods works (what we copy)

SML does **not** ship edits to `BP_MainMenuWidget`. It registers `WidgetBlueprintHooks`
on its Game Instance Module:

| Hook | Parent | Inserts |
|---|---|---|
| Mods button | `mMainMenuList` (Direct, slot 8) | `Widget_MainMenuButtonExtensions` as `ModsButton_SML` |
| Mods page | `mSwitcher` (Direct, append) | `Widget_ModList` as `ModList_SML` |

The button’s Construct finds `mSwitcher` / `mMainMenuList` / ModList and sets
`mSwitcherWidget`, `mParentList`, `mTargetWidget`.

## Shared Worlds (same pattern)

| Hook | Parent | Inserts |
|---|---|---|
| Shared Worlds button | `mMainMenuList` (Direct, slot 2) | game `Widget_FrontEnd_Button` as `mButtonSharedWorlds` (Join Game size/style) |
| Browser page | `mSwitcher` (Direct, append) | game `Widget_SubMenuBackground` as `SharedWorldsBrowser` (Join Game panel chrome); C++ browser fills `mContent` |
| Manage Session button | `OptionsList` | FrontEnd as `mSharedWorld` |
| Session page | `mSwitcher` | C++ `USharedWorldSessionWidget` as `SharedWorldSession` |

At runtime C++ wires the FrontEnd button the same way Mods Construct does
(title, small button, switcher + target), then binds Open to activate the page.

Local starter bake of FactoryGame BPs is optional; if those names already exist
on the archetype the hooks are skipped.

## Manual check

1. Steam / Alpakit: Shared Worlds appears under Mods.
2. Click opens the Shared Worlds browser in the main-menu switcher.
3. Pause → Manage Session → Shared World.
