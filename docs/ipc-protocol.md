# Helper IPC protocol (API version 1)

Transport: HTTP/1.1 on `127.0.0.1:<port>`, JSON bodies.

Discovery: the helper writes `<data>/discovery.json` (Windows:
`%LOCALAPPDATA%\SatisfactorySharedWorld\discovery.json`):

```json
{"schemaVersion": 1, "apiVersion": 1, "port": 51234, "token": "<64 hex>", "pid": 4321, "version": "0.1.0-dev", "startedAt": "…"}
```

Every endpoint except `/v1/health` requires `Authorization: Bearer <token>`.
Requests with an `Origin` header or a `Host` other than
`127.0.0.1`/`localhost` are rejected with 403. Unknown JSON fields → 400.

Errors: `{"error": {"code": "…", "message": "…"}}` with 400 (bad request),
401 (token), 403 (transport), 404 (`UNKNOWN_WORLD`, `WORLD_NOT_FOUND`),
409 (`BAD_STATE`, the body also carries the current `session`).

## Endpoints

| Method & path | Body | Returns |
|---|---|---|
| `GET /v1/health` | – | `{"ok": true, "apiVersion": 1, "version": "…"}` |
| `GET /v1/worlds` | – | `{"worlds": [WorldStatus…]}` |
| `POST /v1/worlds` | CreateRequest | `201 {"revision": Revision}` — creates the world from a local save |
| `GET /v1/worlds/{id}` | – | WorldStatus |
| `GET /v1/worlds/{id}/history` | – | `{"revisions": [Revision…]}` |
| `POST /v1/worlds/{id}/play` | PlayRequest | SessionView — starts Play Shared World (async) |
| `GET /v1/worlds/{id}/session` | – | SessionView |
| `POST /v1/worlds/{id}/session/started` | `{"join": JoinInfo \| null}` | SessionView — host's game session exists |
| `POST /v1/worlds/{id}/session/keepalive` | `{"players": [{"displayName": "…"}]}` | SessionView — send every 10 s while a session is active |
| `POST /v1/worlds/{id}/session/saved` | `{"saveName": "SharedWorld_<id>", "final": bool}` | SessionView — the game finished writing the save |
| `POST /v1/worlds/{id}/session/abort` | – | cancel before the world loaded / stop waiting for a host |
| `POST /v1/worlds/{id}/session/ack` | – | dismiss ERROR / LEASE_LOST / JOIN_READY |
| `POST /v1/worlds/{id}/session/attach` | PlayRequest | resume hosting after a helper restart (generation re-checked) |

### PlayRequest

```json
{"playerId": "…", "displayName": "Reece", "platform": "EOS",
 "saveDirectory": "C:\\Users\\…\\SaveGames\\<account>", "gamePid": 1234}
```

`saveDirectory` comes from `UFGSaveSystem::GetSaveDirectoryPath()`; it must
be an existing absolute directory. `gamePid` lets the helper stop
heartbeating the moment the game process exits.

### SessionView

```json
{
  "worldId": "our-factory",
  "state": "JOIN_READY",
  "decision": "JOIN",
  "message": "Reece is already hosting. Joining session...",
  "steps": [{"at": "…", "message": "Checking shared world..."}, …],
  "error": null,
  "hostName": "Reece",
  "join": {"kind": "online-session-id", "value": "…", "backend": ""},
  "saveName": "SharedWorld_our-factory",
  "savePath": "…",
  "generation": 0,
  "revision": 184,
  "updatedAt": "…"
}
```

`error` when present:

```json
{"code": "NEWER_SAVE_EXISTS",
 "message": "A newer shared save exists.\n\nYour local save has been preserved as a backup.\n\nCloud revision: 185\nLocal revision: 184",
 "detail": "…", "localSaveUnchanged": true, "backupPath": "…",
 "cloudRevision": 185, "localRevision": 184, "retryable": false}
```

Error codes: `WORLD_NOT_FOUND`, `CLOUD_UNREACHABLE`, `LEASE_FAILED`,
`ALREADY_HOSTING_ELSEWHERE`, `HOST_NOT_READY`, `NO_SAVE`,
`CORRUPT_DOWNLOAD`, `DOWNLOAD_FAILED`, `LOCAL_SAVE_UNREADABLE`,
`BACKUP_FAILED`, `LOCAL_STATE`, `NEWER_SAVE_EXISTS`, `UPLOAD_FAILED`
(non-terminal; hosting continues), `LEASE_LOST`, `GAME_EXITED`, `CONTENTION`.

### WorldStatus

```json
{"worldId": "our-factory", "worldName": "Our Factory",
 "status": "ONLINE", "statusText": "Online", "hostName": "Reece",
 "playerCount": 2, "players": [...], "revision": 184, "generation": 591,
 "lastPlayedAt": "…", "lastHostName": "Reece", "leaseExpiresAt": "…",
 "error": "", "local": SessionView}
```

Status is cached for 5 s per helper; `play` always reads fresh.

### Mod reaction table

| `local.state` | Mod does |
|---|---|
| `READY_TO_HOST` | `UFGSaveSystem::LoadSaveGameHeaderSync(saveName)` → `LoadSaveFile` → `Start()`; on failure `session/abort` |
| `HOSTING` (after world load) | publish session id via `session/started`; `SaveGame` + `session/saved` for checkpoints / stop |
| `JOIN_READY` with `join` | resolve + join via OnlineIntegration, then `session/ack` |
| `JOIN_READY` without `join` | show "join from the friends list"; Dismiss → `session/ack` |
| `ERROR`, `LEASE_LOST` | show `error.message`; Retry → `play`, Dismiss → `ack` |
