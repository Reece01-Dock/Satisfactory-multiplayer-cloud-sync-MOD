# Shared Worlds Authority

Status: **design** (Phase 1). Part of the [v2 architecture](shared-worlds-architecture.md).

The authority decides who may host and which revision is current. It replaces GitHub's role (the fast-forward-only
ref update) as the compare-and-swap. It stores **no save files**: only a few kilobytes of metadata per world.

## 1. Contract: implement what the core already needs

The core's lease, fencing, revision and handoff logic (`LeaseManager`, `WorldStore`) needs exactly one atomic
operation from storage (see `Core/Storage/Storage.h`):

```
Head() -> commit id
ReadFile(commit, path) / ListDirectory(commit, dir) / Log(from, n)
Commit(expectedHead, changes[], message) -> newHead | Conflict | Ambiguous
```

So the authority service **is** an `IWorldRepository` over HTTPS, plus authentication and membership. The client
side is a new `FAuthorityRepository : IWorldRepository` in the module, and `LeaseManager` runs on it unchanged.
Everything in the spec's `ISharedWorldAuthority` (AcquireLease, CommitRevision, SetHostReady, membership…) is a thin
façade over `LeaseManager`/`WorldStore` on top of that repository.

Why not move lease logic into the server: the rules are already implemented, tested (`LeaseTests`,
`WorldSessionTests`, `HostMigrationTests`) and shared by every backend. Duplicating them in TypeScript would create two
sources of truth. The server only has to guarantee atomicity, authentication, membership and limits.

Optional server-side hardening (Phase 5+): the service can additionally parse `state/current.json` in each commit and
reject commits that break invariants (generation decreasing, head going backwards, lease taken while live). This is a
defence in depth, not the primary logic.

## 2. Implementation candidate: Cloudflare Worker + Durable Object

- One Durable Object (DO) per world, addressed by world id. A DO processes requests for one id one at a time and
  has strongly consistent storage (ASSUMED from Cloudflare docs; confirm with a concurrent-acquire PoC).
- Commit = inside the DO: `if head != expected → 409 Conflict`; else write the new tree, append the commit, set head.
  No locks needed beyond the DO's single-threaded execution.
- Storage per world: current tree (map path → content, small JSON documents), a bounded commit log (e.g. last 500
  commits; history UI reads `revisions/`, not the log), membership, protocol version.
- Limits: max document 64 KiB, max commit 256 KiB, max files 4096, rate limit per member (e.g. 2 req/s burst 20).
- Endpoints (all JSON over HTTPS, `Authorization: Bearer <session token>`):

| Method | Path | Notes |
|---|---|---|
| `POST` | `/v1/worlds` | create world (creator becomes owner) |
| `GET` | `/v1/worlds/{id}/head` | head commit id |
| `GET` | `/v1/worlds/{id}/files/{commit}/{path}` | file at commit |
| `GET` | `/v1/worlds/{id}/list/{commit}/{dir}` | directory listing |
| `POST` | `/v1/worlds/{id}/commit` | `{expectedHead, changes[], message}` → `{head}` / 409 |
| `GET` | `/v1/worlds/{id}/log?from=&n=` | commit log |
| `POST` | `/v1/worlds/{id}/members` / `DELETE …/members/{player}` | owner/admin only |
| `POST` | `/v1/worlds/{id}/invites` | owner creates a single-use invite code |
| `POST` | `/v1/invites/{code}/accept` | authenticated player joins the world |
| `POST` | `/v1/worlds/{id}/objects/{sha}/upload-url` and `…/download-url` | signed R2 URLs (storage doc §4, option C) |

- **Ambiguous outcomes**: if the response to a commit is lost, the client re-reads `head` and checks whether its
  commit is there (the commit id is computed client-side as a hash of parent + changes, so it is recognisable).
  Same rule as today's GitHub provider.

### What it stores (example)

```json
{
  "worldId": "sw-7a9f…",
  "head": "c_8f3e…",
  "files": {
    "world.json": "{…}",
    "state/current.json": "{\"generation\":82,\"head\":{\"number\":184,\"object\":\"sha256:abc123…\"},\"lease\":{…,\"hostReady\":true}}",
    "state/players.json": "{…}",
    "revisions/00000184-g00000082-abc123.json": "{…}"
  },
  "members": { "steam:7656…A": "owner", "steam:7656…B": "member", "eosacct:abcd…": "member" },
  "protocol": 1
}
```

## 3. Membership and roles

- Membership is held by the authority (not GitHub, not the storage provider). Roles reuse the core's `Role`
  (Owner/Admin/Member/Viewer) and `HasPermission` (Play, Invite, RemovePlayers, RestoreRevision, ModifyStorage…).
- Every request is checked: non-members get 404 (don't reveal that a world exists). Members may read; only roles
  with Play may commit; only Owner/Admin may change membership or storage.
- Joining: the owner creates an invite (single-use, expiring code), shares it through the existing in-game invite
  push or as text; the friend's authenticated identity is added on accept. This replaces GitHub collaborator invites.
- `state/players.json` remains the in-world view of members for the UI and is kept in sync by the authority.

## 4. Authentication (who is this player?)

Must not trust a client-supplied `steamId`.

| Option | Verifies | Status |
|---|---|---|
| Steam session / Web API auth ticket (`GetAuthTicketForWebApi` + `ISteamUserAuth/AuthenticateUserTicket`) | SteamID owning a ticket for app 526870 | **UNSUPPORTED for us** (as far as known): validating needs the app's publisher Web API key, which belongs to Coffee Stain |
| **Steam OpenID 2.0** browser sign-in | SteamID (any website can use it) | ASSUMED (Valve-documented); PoC. Same UX as the GitHub/Dropbox browser approval players already know |
| Epic Account Services OAuth with **our own** EOS product | Epic account id | ASSUMED; needs a free Epic dev portal product. Not the game's EOS ProductUserId |
| Using the game's EOS Connect tokens | EOS ProductUserId | Policy risk: tokens belong to Coffee Stain's deployment; validating them needs their credentials |
| Invite capability only (no platform identity) | "holds the invite secret" | Fallback; weakest (a shared code is a bearer secret) |

Proposed flow:
1. First use: player signs in once (Steam OpenID or Epic) in the browser; the authority returns a **device
   session**: long-lived refresh token bound to a device key pair generated by the mod (private key in Windows
   Credential Manager, like the GitHub token today), short-lived access tokens (15 min).
2. The mod verifies at the game level that the signed-in platform id equals the local player's id
   (`GetPreferredUniqueNetId`) and warns on mismatch.
3. Members are recorded by verified platform id (`steam:<id64>`, `epic:<accountId>`).

Open questions: A1 cost/ownership of the Epic dev product; A2 whether players accept a one-time browser sign-in
(they already do for GitHub/Dropbox today).

## 5. Leases, heartbeats and takeover timing

- Lease TTL stays 45 s + 15 s skew grace for observers (`LeaseConfig`). Renewals are commits (~1-2/min per host):
  far below any rate limit.
- **A3 (open)**: fast takeover. P2P heartbeat loss is detected in ~5 s but acquisition must still be legal at the
  authority. Options: (a) keep TTL (simple, ≤60 s takeover); (b) shorter TTL with more frequent renews; (c) a
  "host gone" commit allowed when ≥2 members report heartbeat loss *and* the host misses one renew (server-side rule;
  still a single CAS). Start with (a), measure, then decide.
- Handoff reservation (planned migration) works unchanged.

## 6. Failure modes

| Failure | Effect | Handling |
|---|---|---|
| Authority unreachable, host running | Renew fails; host keeps playing; lease may expire | Existing behaviour: host continues locally, uploads/commits retried; if another player acquired meanwhile, old host is `Fenced` and its progress is kept as a recovery backup |
| Authority unreachable, nobody hosting | Nobody can acquire | UI "Can't reach Shared Worlds service"; offline single-player continue is **not** offered as authoritative |
| Authority data loss | Catastrophic for coordination | DO storage is durable (assumed); plus periodic export of each world's tree to the world's durable storage (`authority-backup.json`) so it can be re-imported |
| Malicious member | Can commit garbage within their permissions | Server-side invariant checks (§1), revision history kept, owner can remove members and restore |

## 7. Testing

- Phase 3 fake authority implements exactly these semantics in-process (including 404 for non-members, 409 on stale
  head, size limits), so all core tests and the spec §26 matrix run without the service.
- Phase 5 PoC: two machines (or two processes) hammer `AcquireLease(expected gen N)`; assert exactly one success per
  generation over 1,000 rounds.
