# Setup (development preview)

The core is tested; the Unreal module has not been compiled or run in game
yet (see `STATUS.md`). **Back up your saves before trying it.**

## Build

1. Set up the SML 3.12 starter project (Satisfactory Modding docs).
2. Copy `shared-world-mod/SharedWorld/` into the project's `Mods/` folder.
   It contains two modules: `SharedWorldCore` (pure C++, zlib) and
   `SharedWorld` (the SML mod).
3. Build and package with Alpakit as usual.

Core tests (no Unreal needed):

```
cmake -S shared-world-mod/core-tests -B build -G Ninja
cmake --build build && ./build/sw_tests
```

## GitHub storage

1. The mod's release build needs a GitHub OAuth App with **device flow
   enabled**; put its client id in `GitHubClientId`
   (`SharedWorldSubsystem.cpp`). No client secret is used or shipped.
2. Create a (private) repository for your worlds, e.g. `you/our-saves`.
   It may be empty; the mod initialises it. Each world lives on its own
   branch `shared-world/<world-id>`; saves are release assets.
3. In the main menu: **+ Add / Account → Sign in with GitHub**, open the
   shown address and enter the code. The token goes to Windows Credential
   Manager, never into settings, saves or the repository.
4. Enter the storage (`you/our-saves`), your save's name and a world name,
   then **Create Shared World**. Your original save is not changed.

## Friends

* To **play**, friends join your game through the Satisfactory friends
  list (Steam or Epic) — or, if they have added the world, with **Play
  Shared World**.
* To be able to **host** (and take over if you crash), they need write
  access to the storage: in game, `/sharedworld granthost <their-github-name>`,
  then they accept GitHub's invitation, sign in in the mod and add the world
  with its id (`/sharedworld status` shows it).
* Members-only worlds: `/sharedworld restrict`, then
  `/sharedworld allow <player> [member|admin|viewer]` while they are connected.

## Folder storage

Instead of `owner/repo`, enter an absolute folder path: a local folder or a
real network share (SMB/NFS). **Never** use a Google Drive / OneDrive /
Dropbox sync folder: those replicate files without atomic rename or
locking, which breaks the host lock.

## Files on your PC

`%LOCALAPPDATA%\SatisfactorySharedWorld\`: `settings.json` (world list, no
secrets), `install-id`, and `worlds/<id>/` (sync state, verified object
cache, backups — conflict and recovery backups are never pruned).
