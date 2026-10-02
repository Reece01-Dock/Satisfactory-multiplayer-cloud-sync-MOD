# Moving the whole world (record, state, locks) to rclone providers: the locking problem

Status: open design question. Nothing here is implemented yet. Today, saves can live on any rclone provider, but the
world record, state and the host lock stay on GitHub (or a local folder).

## What "the stuff that locks" actually is

A Shared World has two kinds of data:

| Data | Examples | Needs |
|---|---|---|
| Save files (objects) | each uploaded `.sav`, content-addressed by SHA-256 | nothing special: files never change once written |
| World repository | `world.json`, `state.json` (current revision, host lease, generation), players, settings | **compare-and-swap** |

Every change to the repository goes through one primitive:

```
Commit(ExpectedHead, changes)  ->  succeeds only if the repository is still at ExpectedHead,
                                   otherwise fails with Conflict and the caller re-reads and retries
```

Everything that keeps a world safe is built on that single guarantee:

- **The host lease.** "I am the host, generation 7" is written with Commit. If two players press Play at the same
  moment, both read the same head, both try to commit, and exactly one succeeds. The other gets Conflict, re-reads,
  sees a host, and joins instead.
- **Revision fencing.** A new save is only published if the publisher still holds the current generation. An old host
  that lost its connection, or a crashed game coming back, can never overwrite newer progress.
- **Host migration.** The hand-over to the next host is a sequence of commits, each one valid only on top of the last.

On GitHub, Commit is a git ref update that only succeeds if the branch still points where we expect. That is real
compare-and-swap, enforced by GitHub's servers.

## What rclone can and can't do

rclone was built to copy files, not to coordinate writers. Every upload is "overwrite whatever is there". There is no
"only if unchanged" option in its API. So an rclone-only world repository would mean two players can both "win" the
lease.

However, rclone's providers differ underneath. Many cloud APIs do support a conditional write natively; rclone just
doesn't expose it. Because we build librclone from source, we can add a small command (e.g. `sharedworld/putif`)
that calls each provider's own conditional write. That gives real compare-and-swap wherever the provider has one.

### Provider capabilities

Based on the providers' API documentation as I understand it. Each "Yes" must be confirmed with a real test before it
is trusted.

| Provider | Native "write only if unchanged" | How |
|---|---|---|
| Dropbox | Yes | upload mode `update` with the file's `rev`; `add` for create-only |
| Box | Yes | upload new version with `If-Match` |
| OneDrive / SharePoint | Yes | `If-Match` ETag on upload |
| S3 and S3-compatible (AWS, R2, Wasabi, MinIO, B2 S3 API...) | Yes (most, since 2024) | `If-Match` / `If-None-Match` on PUT; check each vendor |
| Azure Blob | Yes | ETag `If-Match` / `If-None-Match` |
| Google Cloud Storage | Yes | `ifGenerationMatch` precondition |
| WebDAV (Nextcloud, ownCloud...) | Usually | `If-Match` ETag; depends on the server |
| SFTP, SMB, local folder / NAS | Yes | create-exclusive file or atomic rename on the server |
| **Google Drive** | **No (as far as known)** | `files.update` documents no precondition header; to be re-checked against the current API |
| **pCloud, MEGA, Jottacloud, Yandex, Mail.ru, Koofr, pikpak...** | **No / undocumented** | plain overwrite only |
| **FTP** | **No** | protocol has no conditional write |
| **HTTP, read-only and "virtual" backends** | n/a | cannot store a world |

Note: Google Drive is the most popular provider players will ask for, and it is in the "No" group.

## The downside for "No" providers

Without a conditional write, the best possible is a **claim, wait, re-check** lock:

1. Read the current state (generation N).
2. Write a claim file `claims/N+1/<my install id>`.
3. Wait a settle time (for example 5 seconds) so any competing claim has time to land.
4. List `claims/N+1/`. If I am the only claim (or the lowest id), I am the host. Otherwise delete my claim and join.

### When it fails

It fails if two claims are written but **one of the players lists before the other's claim is visible**:

- **Simultaneous start.** Two friends press Play within about the settle time, and the provider's listing lags (Google
  Drive listings can take seconds to show a new file). Both see only their own claim and both become host. Each
  plays their own copy. Whoever uploads last overwrites the other's progress for that session.
- **Slow or flaky connection.** A player whose upload of the claim is slow can land after the other already decided.
  The same split happens.
- **Clock / pause issues.** A laptop that sleeps mid-claim and wakes after the settle time breaks the "wait" step.
- **Revision fencing gets weaker.** A host that lost the lease (e.g. went offline) can still overwrite `state.json`,
  because nothing on the server rejects a stale write. The generation check becomes "read, compare, write", and two
  writers can interleave between the read and the write.

### How bad is it?

- The window is small: a few seconds around a simultaneous start or a host hand-over.
- The result is not file corruption: save files are immutable and content-addressed, so every save stays intact.
  What can go wrong is **which revision is "current"**. One player's session progress can be dropped, and state can
  point at the wrong revision until someone restores from history (all revisions are kept).
- Host migration (a host leaves and another takes over automatically) is the riskiest case, because it is exactly a
  moment when several clients act at once.

## Ways out (ideas to evaluate)

1. **Hybrid (what exists today, extended).** Native-CAS providers hold everything. "No" providers hold the saves,
   while the record and lock stay on GitHub. Always safe, but those players also need GitHub.
2. **Tiny free lock service.** A small coordinator that only stores the lock and head pointer with real
   compare-and-swap, for example a Cloudflare Worker with a Durable Object, or a free Firebase/Supabase row with a
   conditional update. Every provider then becomes safe. Costs: something to host and keep running, and a privacy
   note (it only stores world ids and lease data, never saves).
3. **Use the game session as the lock.** Before taking the lease, check Steam/EOS presence for anyone already hosting
   that world (the mod already discovers friends' sessions). This shrinks the window to "two people start in the same
   second while nobody hosts". It can be combined with the claim-wait-recheck lock. Still not a guarantee.
4. **Claim-wait-recheck with a warning.** Accept the risk for "No" providers, and show "Locking: best effort" on the
   provider card and world details so players know to coordinate who starts the world.
5. **Restrict "No" providers to save files only** and say so clearly in the Storage UI ("Google Drive can store saves.
   Choose a provider with safe locking to move the whole world").
6. **Per-provider special tricks.** Some "No" providers may still have a usable primitive (for example, an API that
   fails when a file with a given name already exists in a folder). Each would need investigating and testing one by
   one, and may change without notice.

## What is safe to build now regardless

- The rclone patch adding `sharedworld/read` + `sharedworld/putif`, implemented first for local folders (testable
  here without any account) and Dropbox, then the other native-CAS providers.
- A git-like repository on top of it: immutable, content-addressed commit and file blobs, plus one `HEAD` pointer
  moved forward with `putif`. This plugs into the existing `IWorldRepository`, so leases, fencing and migration logic
  stay exactly as they are.
- "Set as Active" moving record, state, locks and saves for native-CAS providers, with the old GitHub copy marked
  "moved", so nobody keeps playing a stale copy.

The decision above only affects the "No" providers.
