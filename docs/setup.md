# Setup (development preview)

This is a development preview. The helper is tested; the mod compiles only
in an SML development environment and has not been run in game yet (see
`STATUS.md`). **Back up your saves before trying it.**

## 1. Build the helper

Requires Go 1.22+.

```sh
cd shared-world-helper
go test -race ./...                       # unit, race and end-to-end tests
./scripts/smoke-two-helpers.sh            # two real helper processes, HOST/JOIN over HTTP
GOOS=windows GOARCH=amd64 go build -o ../shared-world-mod/SharedWorld/ThirdParty/SharedWorldHelper/Win64/shared-world-helper.exe ./cmd/shared-world-helper
```

## 2. Configure the helper

Create `%LOCALAPPDATA%\SatisfactorySharedWorld\config.json`:

```json
{
  "schemaVersion": 1,
  "provider": { "type": "filesystem", "root": "D:\\SharedWorlds" },
  "worlds": [ { "id": "our-factory", "name": "Our Factory" } ]
}
```

Optional keys: `lease.ttlSeconds` (90), `lease.heartbeatSeconds` (20, at
most a third of the TTL), `lease.skewGraceSeconds` (30),
`keepLocalBackups` (20; conflict backups are never pruned), `ipcPort`
(0 = random). An `installId` is generated on first start.

> **Filesystem provider limits.** It is safe for several helpers on one
> machine and for a real network share with working exclusive-create
> semantics. It is **not** safe on a folder replicated by Google Drive /
> OneDrive / Dropbox desktop clients: those replicate eventually, so two
> PCs can both acquire the world. Cloud providers with native conditional
> writes are the next milestone (docs/research.md §6).

Other files in that directory: `discovery.json` (port + token for the mod),
`logs/helper.log` (JSON lines), `state/` (per-world sync state),
`backups/<world>/` (local backups and conflict copies), `staging/`.

## 3. Create the shared world (once)

With the helper running, from a save that already exists in your save directory:

```sh
curl -X POST -H "Authorization: Bearer <token from discovery.json>" \
  -d '{"worldId":"our-factory","worldName":"Our Factory","importSaveName":"MyExistingSave",
       "playerId":"me","displayName":"Me","platform":"steam",
       "saveDirectory":"C:\\Users\\me\\AppData\\Local\\FactoryGame\\Saved\\SaveGames\\<id>","gamePid":0}' \
  http://127.0.0.1:<port>/v1/worlds
```

(An in-game "Create shared world" flow is part of Milestone 10.)

## 4. Build the mod

1. Set up the SML starter project for SML 3.12 / the current game build
   (docs.ficsit.app → Development → Getting started).
2. Copy `shared-world-mod/SharedWorld` into the project's `Mods/` folder.
3. Build the helper into `SharedWorld/ThirdParty/SharedWorldHelper/Win64/` (step 1).
4. Package with Alpakit and install as usual.

In game: the **Shared Worlds** panel appears on the main menu. In a hosted
shared world, `/sharedworld save` uploads a checkpoint and
`/sharedworld stop` saves, uploads and releases the world.
