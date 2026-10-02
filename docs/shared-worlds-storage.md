# Shared Worlds durable storage

Status: **design** (Phase 1). Part of the [v2 architecture](shared-worlds-architecture.md).

## 1. Invariant

> A revision cannot become authoritative HEAD until its save object is confirmed durable.

Today: `SyncEngine::Upload` (`Core/Sync/Sync.cpp`) does snapshot → SHA-256 → `Has` → `Put` → optional read-back
verify → `LeaseManager::CommitRevision`. HEAD only moves through `CommitRevision`, so this already holds for every
backend. Phase 4 makes it explicit and observable (storage health) and adds mirror targets.

## 2. Object model (unchanged)

- Object id = lowercase hex SHA-256 of the exact `.sav` bytes.
- Stored packaged (`EncodingObjectStore`: zstd, SWOB header); the logical hash is always the raw save's.
- Immutable, never overwritten; `Put` is idempotent and never exposes a partial object (temp name + rename, or
  provider-atomic upload, then size/hash check).
- Layout: `objects/<aa>/<sha256>` (rclone), `objects/sha256/<aa>/<sha256>` (folder), release assets (GitHub).
- Revision metadata (`revisions/…json`, in the authority) references the object; `state/current.json.head`
  references the revision. Restore = new revision pointing at an old object (no rewrite).
- Retention: keeps the last N revisions (default 5) and deletes unreferenced objects. **v2 change:** retention runs
  only from the current lease holder and never deletes an object referenced by HEAD, a pending upload, or the
  recent-revision window (also enforced in the local cache).

`ISharedWorldObjectStore` in the spec = the existing `IObjectStore` (`Has / Put / Get / Remove / List`) plus a
`Verify(hash)` helper (Get to temp + hash) that the fetch chain and storage-health checks use.

## 3. Lookup order and the local cache

```
local cache (verified) → connected peers (SAVE_HAVE → SAVE_REQUEST, full hash check) → durable targets in order
```

- Cache: `%LOCALAPPDATA%/SatisfactorySharedWorld/cache/objects/…` (exists today in `Sync`). Settings: cache latest
  save (default on), latest N (default 3), max size (default 4 GB), P2P prefetch (default on), max background
  bandwidth.
- Cleanup never removes objects referenced by an active world's HEAD, a pending upload or the recent window.
- A cache hit is re-hashed before use if the file's size/mtime changed since it was verified; a damaged entry is
  deleted (existing behaviour).

## 4. The storage access problem (who can read the bytes?)

A world created by Reece on his Google Drive is not readable by Tom. Every member who might host must be able to read
the latest object (offline scenario) and write new ones (when hosting). Options evaluated:

### A. Shared provider credentials
The creator's rclone token (Drive/Dropbox…) is given to members.
- Exposes the creator's **whole** cloud account (provider OAuth scopes are account-wide; Drive's `drive.file` scope
  is per-app-per-user and does not help sharing).
- Revocation means revoking the token for the creator too.
- **Rejected.** Same reason the GitHub design refused token sharing.

### B. World-scoped storage with scoped credentials
A dedicated bucket/prefix per world (R2, S3, B2) with credentials limited to that prefix.
- R2/S3 support prefix-scoped keys/policies; B2 supports bucket-restricted application keys with name prefix.
- Members still receive long-lived secrets; revocation = rotate the key for everyone.
- Viable for technical groups who bring their own bucket; not "zero setup".

### C. Authority-issued signed URLs (recommended default)
The authority holds the bucket credential (e.g. Cloudflare R2 binding) and hands members short-lived presigned
GET/PUT URLs for one object at a time, after checking membership and (for PUT) the lease.
- Players need no cloud account and see no credentials.
- Per-object, time-limited, revocable by removing the member.
- PUT URLs can be restricted to the exact object key (`objects/<aa>/<sha>`); the client still verifies by hash.
- Cost sits with whoever runs the authority (D1). R2 has no egress fees (ASSUMED from Cloudflare docs; verify
  pricing). Storage cost ≈ saves × retention × worlds.
- Recommended as the default for v2.

### D. Provider-native sharing (built, optional)
The world's saves live in a folder on the creator's provider; the creator shares the folder using the provider's own
sharing; each member connects the same provider and links the shared folder (World Details > Storage). Implemented
today (`SaveStorageRouter`, link UI, "Set as Active", move existing worlds).
- Works where providers support folder sharing with write access (Dropbox, OneDrive, Google Drive, Box, Nextcloud…).
- Friction: every host needs an account on that provider and must accept the share; shared-folder paths differ per
  user (handled by the link step).
- Keep as an option for groups that prefer their own cloud; also usable as a **mirror** (§5).

## 5. Multiple durable targets

Policy object per world:

```json
{ "targets": [ {"id":"primary","kind":"authority-r2","required":true},
               {"id":"mirror","kind":"rclone","remote":"sw-drive-1:SharedWorlds","required":false,"async":true} ] }
```

- HEAD may advance once **all required** targets confirm (default: exactly one required).
- Async mirrors are filled in the background; failures show in storage health but do not block.
- Fetch order: cache → peers → required targets → mirrors.

## 6. Storage health

Tracked per world and shown in World Details (plain language):

```
Revision 184 · Latest save: Synced · Storage: Cloud (✓) · Mirror: Google Drive (✓) · Cached by: 2 players
```

- If the required target is unavailable: autosaves continue **locally** as unsynced progress, the world shows
  "World storage unavailable — progress can't be safely saved until storage is back", the host is not offered for
  takeover-with-local-save, and nothing is announced to peers as current. When storage returns, the existing
  unsynced-progress rule uploads it only if HEAD is still its base.
- Open question S1: whether to block new sessions entirely while storage is down (recommended: allow joining a live
  host, block *starting* a host).

## 7. Tests (spec §26 subset owned by storage)

| Test | Expected |
|---|---|
| Durable Put fails | HEAD unchanged; local save kept as unsynced progress |
| Host crash during upload (partial object) | Partial never visible (atomic Put); HEAD unchanged |
| Crash after upload, before commit | Object remains; HEAD unchanged; next host ignores it; retention later removes it |
| Crash after commit | New host fetches the committed hash (cache/peer/cloud) and continues |
| Corrupt object in a target | Hash check fails → next source; target marked unhealthy |
| Mirror down | HEAD advances (mirror not required); health shows mirror offline |
