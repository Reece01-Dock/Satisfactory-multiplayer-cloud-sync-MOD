# Shared Worlds P2P (SWP/1)

Status: **design** (Phase 1). Part of the [v2 architecture](shared-worlds-architecture.md).

P2P is an accelerator: faster presence, heartbeats, migration preparation and save transfer between friends.
It is **never** the authority (who hosts) and **never** the only copy of a save.

## 1. Scope

- Friends only: peers are found from the platform friends list (Steam today via UE OnlineSubsystem, see
  `SharedWorldDiscoveryService`). No global discovery, DHT or anonymous routing.
- After HELLO, peers only exchange information about worlds **both** are members of (membership checked against the
  authority's member list, cached).
- Works in the main menu (before any game session exists) and alongside a running session.

## 2. Transport options

| Transport | Reaches | Status |
|---|---|---|
| Steam Networking Messages (`ISteamNetworkingMessages`, Steamworks 1.57 ships with the game) | Steam ↔ Steam | ASSUMED callable from the mod under app 526870 (game already initialises SteamAPI). **PoC first** (Phase 6) |
| EOS P2P (`SocketSubsystemEOS` ships with the game) | Steam ↔ Epic ↔ Epic (crossplay) | ASSUMED technically; **policy risk**: it would use Coffee Stain's EOS deployment for our traffic (see `native-architecture.md` §1). Needs a decision before use |
| Unreal replication inside a running game session (`SharedWorldInviteBridge` today) | everyone in the session | TESTED; only while a session exists; good for in-session messages, not for menu-time discovery |
| Direct UDP/TCP | anyone | Rejected: NAT traversal, firewall prompts, exposes IPs |

Plan: `ISharedWorldPeerNet` interface; Steam implementation first; in-session channel as a second implementation for
crossplay groups while playing; Epic menu-time P2P only after the policy decision (D4). Players without a P2P path
simply use the authority + durable storage (everything still works, just slower).

Transport identity (Steam ID of the sender) is authenticated by the transport, but **is not world authorisation**:
every world-scoped message is checked against membership.

## 3. Messages

Binary framed or compact JSON; explicit schemas; no serialised Unreal objects.

```
frame = { magic "SWP1", version u8, type u8, flags u8, length u32, payload[length] }
max frame 64 KiB (control) / 256 KiB (SAVE_CHUNK); unknown type → ignore + count; malformed → drop peer for 60 s
```

| Type | Direction | Payload (key fields) | Limits |
|---|---|---|---|
| HELLO / HELLO_ACK | both | protocol range, mod version, platform id, nonce, list of world ids (hashed with a per-peer salt so non-members learn nothing) | 1 per peer per 10 s |
| WORLD_QUERY / WORLD_STATUS | both | world id → {live, host, generation, revision, head hash, players, hostReady} | member-only |
| HOST_HEARTBEAT | host → members | world id, generation, revision, players, monotonic seq | 1 per 2 s |
| PING / PONG | both | nonce, send time | 2 per s |
| REVISION_ANNOUNCE | host → members | world id, generation, revision, head hash, size | per commit |
| SAVE_HAVE_QUERY / SAVE_HAVE | both | hash → have, size | member-only |
| SAVE_REQUEST / SAVE_CHUNK | both | hash, offset, length / bytes | bandwidth cap, max 4 concurrent |
| MIGRATION_PROPOSAL / MIGRATION_STATUS | members | generation, ranked candidates, chosen, stage | member-only |
| GOODBYE | both | reason | — |

Replay protection: HELLO nonces; heartbeats carry `generation` + strictly increasing `seq`; any message with a lower
generation than the authority's current one is ignored. P2P never changes authoritative state by itself: a
heartbeat claiming to be host for generation 83 is only believed after the authority shows generation 83 held by
that player.

## 4. Save transfer

```
need X → SAVE_HAVE_QUERY to members → pick best SAVE_HAVE (RTT, direct) → SAVE_REQUEST chunks
       → write temp → full SHA-256 == X ? move into cache : delete, mark peer bad, try next → durable fallback
```

Prefetch: after REVISION_ANNOUNCE, idle peers (setting on) fetch the new object from the host or durable storage in
the background, under the bandwidth cap, never during loading screens.

## 5. Host selection with P2P inputs

`HostElection` already ranks candidates deterministically from RTT, jitter, loss, reachability, upload, save cached,
storage ready. P2P supplies those inputs before a session exists (PING/PONG between peers, SAVE_HAVE for "cached").
The ranking only decides **who tries first**; the authority's `AcquireLease` decides who hosts.

## 6. Threat model

| Threat | Mitigation |
|---|---|
| Malicious peer sends a fake save | Full SHA-256 check against the authority's HEAD hash; reject and blacklist |
| Spoofed world id / fake host heartbeat | Membership check; heartbeat only believed if it matches the authority's lease holder + generation |
| Replayed messages | nonces, generation + seq ordering |
| Oversized packets / spam | frame size limits, per-type rate limits, peer back-off/drop |
| Invalid migration packets | migration is advisory; only `AcquireLease` at the authority is binding |
| Stolen storage URL | signed URLs short-lived, per object; content still hash-verified |
| Unauthorised friend | not a member → only HELLO answered; no world data |
| Stale host | fenced at the authority (existing) |
| Compromised local cache | entries re-verified by hash before use |

## 7. Proof-of-concept list (do before building on it)

1. Steam Networking Messages send/receive between two Steam accounts from the main menu, from the mod's module.
2. Message size / throughput for SAVE_CHUNK (target ≥ 2 MB/s relayed).
3. Behaviour when one side is not running the mod (messages must be ignored safely).
4. EOS P2P feasibility and policy (decision D4).
