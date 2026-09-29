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

See **[github-oauth-setup.md](github-oauth-setup.md)** for OAuth App creation,
Client ID configuration, and device-login testing.

1. Create a GitHub OAuth App with **Device Flow** enabled; ship only the public
   Client ID (`SHAREDWORLD_GITHUB_CLIENT_ID` for local dev, or
   `SHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED` baked into packaged builds). No
   client secret is used or shipped.
2. Link GitHub in Shared Worlds. On first Shared World create/upload the mod
   **automatically creates** a private repo `satisfactory-shared-worlds` under
   your account (no manual repo setup).
3. Each world lives on branch `shared-world/<world-id>`; save bytes are release
   assets. Your original personal save is not changed.
4. Enter a world name / pick a save and **Create Shared World**, then Play.

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
