#pragma once
// Shared Worlds live-host verification protocol.
//
// The cloud lease remains authoritative for who SHOULD host. This handshake
// only answers whether that lease holder is reachable and consistent with the
// lease (world id, generation, identity, revision, session id).
//
// Transport is injected (UE net / OnlineIntegration / test fake). Packets are
// never used as ownership.

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "SharedWorldCore/Model/Model.h"
#include "SharedWorldCore/Util/Result.h"
#include "SharedWorldCore/Util/Time.h"

namespace sw
{
	constexpr int32_t HostHandshakeProtocolVersion = 1;

	struct SharedWorldHello
	{
		int32_t ProtocolVersion = HostHandshakeProtocolVersion;
		std::string WorldId;
		std::string PlayerId;
		int64_t ExpectedRevision = 0;
		int64_t ExpectedLeaseGeneration = 0;
		std::string Nonce;
		TimeMs Timestamp = 0;
	};

	struct SharedWorldHelloAck
	{
		int32_t ProtocolVersion = HostHandshakeProtocolVersion;
		std::string WorldId;
		std::string HostPlayerId;
		int64_t CurrentRevision = 0;
		int64_t LeaseGeneration = 0;
		std::string SessionId; // may be empty when friends-list-only
		int32_t CurrentPlayers = 0;
		int32_t MaxPlayers = 0;
		std::string Nonce;
		TimeMs Timestamp = 0;
	};

	enum class HostVerifyOutcome
	{
		Verified,
		Unreachable,
		Mismatch,
		Timeout,
		Unsupported,
	};
	const char* ToString(HostVerifyOutcome O);

	struct HostVerifyResult
	{
		HostVerifyOutcome Outcome = HostVerifyOutcome::Unreachable;
		std::optional<SharedWorldHelloAck> Ack;
		/** Diagnostics only. Prefer stable tokens: session_not_found, lease_mismatch, timed_out, ... */
		std::string Detail;
		/** Round-trip / resolve latency when measured; 0 if unknown. */
		int RttMs = 0;
	};

	/**
	 * Validates an Ack against the Hello request and the authoritative lease /
	 * head revision. Does not perform I/O.
	 */
	Status ValidateHelloAck(const SharedWorldHello& Hello, const SharedWorldHelloAck& Ack, const Lease& ExpectedLease,
		int64_t ExpectedHeadRevision, const std::optional<JoinInfo>& ExpectedJoin = std::nullopt);

	/** Builds a Hello for the local player against the current cloud lease. */
	SharedWorldHello MakeHello(const Identity& Me, const std::string& WorldId, const Lease& Lease, int64_t HeadRevision,
		const std::string& Nonce, TimeMs Now);

	/**
	 * Transport for reachability probes. Production: UE adapter. Tests: fake.
	 * Returning Network / NotFound maps to Unreachable; Invalid to Mismatch.
	 */
	class IHostVerifier
	{
	public:
		virtual ~IHostVerifier() = default;
		virtual HostVerifyResult Probe(const SharedWorldHello& Hello, const JoinInfo& Join, TimeMs Timeout) = 0;
	};

	/** Always Verified with an Ack synthesised from the lease (no network). */
	class TrustLeaseHostVerifier final : public IHostVerifier
	{
	public:
		HostVerifyResult Probe(const SharedWorldHello& Hello, const JoinInfo& Join, TimeMs Timeout) override;
	};

	/** Configurable fake for tests. */
	class FakeHostVerifier final : public IHostVerifier
	{
	public:
		HostVerifyOutcome NextOutcome = HostVerifyOutcome::Verified;
		std::optional<SharedWorldHelloAck> NextAck;
		std::string NextDetail;
		int NextRttMs = 0;
		int CallCount = 0;
		/** After FailCount failures, subsequent probes succeed (packet-drop then recover). */
		int FailCount = 0;
		HostVerifyOutcome FailOutcome = HostVerifyOutcome::Unreachable;

		HostVerifyResult Probe(const SharedWorldHello& Hello, const JoinInfo& Join, TimeMs Timeout) override;
	};

	/**
	 * Test double for OnlineIntegration-style session resolve without UE.
	 * Set PresentSessionId to the Join.Data that should resolve; HostPlayerId
	 * is stamped onto the Ack (must match the lease holder for Verify to pass).
	 */
	class SessionResolveHostVerifier final : public IHostVerifier
	{
	public:
		std::string PresentSessionId;
		std::string HostPlayerId;
		int64_t HostRevision = -1;      // <0 → echo Hello.ExpectedRevision
		int64_t HostLeaseGeneration = -1; // <0 → echo Hello.ExpectedLeaseGeneration
		bool bResolveTimeout = false;
		int ResolveDelayMs = 0;
		int CallCount = 0;

		HostVerifyResult Probe(const SharedWorldHello& Hello, const JoinInfo& Join, TimeMs Timeout) override;
	};
}
