# Shared Worlds — two-machine host verification test

Transport under test: **OnlineIntegration session resolve**
(`MakeOnlineSessionId` → `ResolveOnlineSession`), plus local `USharedWorldHostResponder`
for same-process Ack fields. Custom pre-join packets are **not** used (Satisfactory
does not expose a supported connectionless Shared Worlds channel).

Lease/CAS remains authoritative. Verification never grants hosting.

## Setup

- Two Steam accounts / PCs with the Shared Worlds mod.
- Same Shared World in both clients (owner creates; friend added / open membership).
- Storage reachable on both (folder or GitHub).

## Happy path

**Machine A (host)**

1. Main menu → Shared Worlds → Play the world.
2. Wait until loaded as listen-server host.
3. Confirm logs: `[SharedWorld/Host] event=session_published` and
   `[SharedWorld/Verify] event=host_responder_ready`.
4. In-game: `/sharedworld verify` → expect HostReady yes, Verification PASS (local responder).

**Machine B (client)**

1. Shared Worlds → select same world → Play.
2. Expect UI: Checking host… → Connecting to \<A\>…
3. Logs: `[SharedWorld/Verify] ... transport=OnlineIntegrationResolve result=Verified`
4. JoinManager proceeds (existing path). Land in A's session.

## Failure cases

| Scenario | Expected verification | Expected lease | Expected UI |
|---|---|---|---|
| A still loading (Preparing, HostReady=false) | not probed yet | A holds lease | Waiting for A to finish starting… |
| A published then Steam session killed but lease renews | SessionNotFound / retry | A still holds until TTL | Having trouble reaching A. Retrying… → Host connection lost… |
| Kill Satisfactory on A | retries then recover | expires after TTL+grace | then B Play can Acquire |
| A network disconnect | same as unreachable | conservative | retry/recover, no immediate host steal |
| A returns after B took over | A's renew/commit Fenced | B generation | A must not overwrite cloud Head |
| `/sharedworld verify` on B while A hosts | resolve + ValidateHelloAck | unchanged | PASS/FAIL text only; no ownership change |

## Log greps

```
[SharedWorld/Verify]
[SharedWorld/Join]
[SharedWorld/Host]
[SharedWorld/Lease]
```

Confirm session ids appear only as `session_id_hash=...`, never raw tokens.
