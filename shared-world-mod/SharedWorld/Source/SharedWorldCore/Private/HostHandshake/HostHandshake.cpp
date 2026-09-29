#include "SharedWorldCore/HostHandshake/HostHandshake.h"

namespace sw
{
	const char* ToString(HostVerifyOutcome O)
	{
		switch (O)
		{
		case HostVerifyOutcome::Verified: return "VERIFIED";
		case HostVerifyOutcome::Unreachable: return "UNREACHABLE";
		case HostVerifyOutcome::Mismatch: return "MISMATCH";
		case HostVerifyOutcome::Timeout: return "TIMEOUT";
		case HostVerifyOutcome::Unsupported: return "UNSUPPORTED";
		}
		return "?";
	}

	SharedWorldHello MakeHello(const Identity& Me, const std::string& WorldId, const Lease& Lease, int64_t HeadRevision,
		const std::string& Nonce, TimeMs Now)
	{
		SharedWorldHello H;
		H.ProtocolVersion = HostHandshakeProtocolVersion;
		H.WorldId = WorldId;
		H.PlayerId = Me.PlayerId;
		H.ExpectedRevision = HeadRevision;
		H.ExpectedLeaseGeneration = Lease.Generation;
		H.Nonce = Nonce;
		H.Timestamp = Now;
		return H;
	}

	Status ValidateHelloAck(const SharedWorldHello& Hello, const SharedWorldHelloAck& Ack, const Lease& ExpectedLease,
		int64_t ExpectedHeadRevision, const std::optional<JoinInfo>& ExpectedJoin)
	{
		if (Ack.ProtocolVersion != Hello.ProtocolVersion || Ack.ProtocolVersion != HostHandshakeProtocolVersion)
		{
			return MakeError(ErrorCode::Unsupported, "host handshake protocol mismatch");
		}
		if (Ack.Nonce != Hello.Nonce)
		{
			return MakeError(ErrorCode::Invalid, "host handshake nonce mismatch");
		}
		if (Ack.WorldId != Hello.WorldId)
		{
			return MakeError(ErrorCode::Invalid, "host handshake world id mismatch");
		}
		if (Ack.LeaseGeneration != Hello.ExpectedLeaseGeneration || Ack.LeaseGeneration != ExpectedLease.Generation)
		{
			return MakeError(ErrorCode::Fenced, "host handshake lease generation mismatch");
		}
		if (Ack.HostPlayerId != ExpectedLease.Holder.PlayerId)
		{
			return MakeError(ErrorCode::Invalid, "host handshake identity mismatch");
		}
		if (Ack.CurrentRevision != Hello.ExpectedRevision || Ack.CurrentRevision != ExpectedHeadRevision)
		{
			return MakeError(ErrorCode::StaleRevision, "host handshake revision mismatch");
		}
		if (ExpectedJoin && ExpectedJoin->Kind == "online-session-id" && !ExpectedJoin->Data.empty())
		{
			if (Ack.SessionId.empty())
			{
				return MakeError(ErrorCode::Invalid, "host handshake missing session id");
			}
			if (Ack.SessionId != ExpectedJoin->Data)
			{
				return MakeError(ErrorCode::Invalid, "host handshake session id mismatch");
			}
		}
		return {};
	}

	HostVerifyResult TrustLeaseHostVerifier::Probe(const SharedWorldHello& Hello, const JoinInfo& Join, TimeMs)
	{
		HostVerifyResult R;
		R.Outcome = HostVerifyOutcome::Verified;
		SharedWorldHelloAck Ack;
		Ack.ProtocolVersion = Hello.ProtocolVersion;
		Ack.WorldId = Hello.WorldId;
		Ack.HostPlayerId.clear(); // caller must fill via Validate with ExpectedLease; Trust path synthesises below
		Ack.CurrentRevision = Hello.ExpectedRevision;
		Ack.LeaseGeneration = Hello.ExpectedLeaseGeneration;
		Ack.SessionId = Join.Kind == "online-session-id" ? Join.Data : std::string();
		Ack.Nonce = Hello.Nonce;
		Ack.Timestamp = Hello.Timestamp;
		// HostPlayerId is filled by the session using the lease before Validate;
		// store Join data only here.
		R.Ack = std::move(Ack);
		R.Detail = "trust-lease";
		return R;
	}

	HostVerifyResult FakeHostVerifier::Probe(const SharedWorldHello& Hello, const JoinInfo& Join, TimeMs)
	{
		++CallCount;
		HostVerifyResult R;
		if (FailCount > 0)
		{
			--FailCount;
			R.Outcome = FailOutcome;
			R.Detail = NextDetail.empty() ? "injected_fail" : NextDetail;
			return R;
		}
		R.Outcome = NextOutcome;
		R.Detail = NextDetail;
		R.RttMs = NextRttMs;
		if (NextAck)
		{
			R.Ack = NextAck;
		}
		else if (NextOutcome == HostVerifyOutcome::Verified)
		{
			SharedWorldHelloAck Ack;
			Ack.ProtocolVersion = Hello.ProtocolVersion;
			Ack.WorldId = Hello.WorldId;
			Ack.CurrentRevision = Hello.ExpectedRevision;
			Ack.LeaseGeneration = Hello.ExpectedLeaseGeneration;
			Ack.SessionId = Join.Kind == "online-session-id" ? Join.Data : std::string();
			Ack.Nonce = Hello.Nonce;
			Ack.Timestamp = Hello.Timestamp;
			R.Ack = std::move(Ack);
		}
		return R;
	}

	HostVerifyResult SessionResolveHostVerifier::Probe(const SharedWorldHello& Hello, const JoinInfo& Join, TimeMs Timeout)
	{
		++CallCount;
		HostVerifyResult R;
		if (bResolveTimeout || (ResolveDelayMs > 0 && Timeout > 0 && ResolveDelayMs > Timeout))
		{
			R.Outcome = HostVerifyOutcome::Timeout;
			R.Detail = "timed_out";
			return R;
		}
		if (Join.Kind == "online-session-id" && Join.Data.empty())
		{
			R.Outcome = HostVerifyOutcome::Verified;
			R.Detail = "friends_list_fallback";
			SharedWorldHelloAck Ack;
			Ack.ProtocolVersion = Hello.ProtocolVersion;
			Ack.WorldId = Hello.WorldId;
			Ack.HostPlayerId = HostPlayerId;
			Ack.CurrentRevision = HostRevision >= 0 ? HostRevision : Hello.ExpectedRevision;
			Ack.LeaseGeneration = HostLeaseGeneration >= 0 ? HostLeaseGeneration : Hello.ExpectedLeaseGeneration;
			Ack.Nonce = Hello.Nonce;
			Ack.Timestamp = Hello.Timestamp;
			R.Ack = std::move(Ack);
			return R;
		}
		if (Join.Kind != "online-session-id" || Join.Data != PresentSessionId)
		{
			R.Outcome = HostVerifyOutcome::Unreachable;
			R.Detail = "session_not_found";
			return R;
		}
		R.Outcome = HostVerifyOutcome::Verified;
		R.Detail = "session_resolved";
		R.RttMs = ResolveDelayMs > 0 ? ResolveDelayMs : 12;
		SharedWorldHelloAck Ack;
		Ack.ProtocolVersion = Hello.ProtocolVersion;
		Ack.WorldId = Hello.WorldId;
		Ack.HostPlayerId = HostPlayerId;
		Ack.CurrentRevision = HostRevision >= 0 ? HostRevision : Hello.ExpectedRevision;
		Ack.LeaseGeneration = HostLeaseGeneration >= 0 ? HostLeaseGeneration : Hello.ExpectedLeaseGeneration;
		Ack.SessionId = Join.Data;
		Ack.CurrentPlayers = 1;
		Ack.MaxPlayers = 4;
		Ack.Nonce = Hello.Nonce;
		Ack.Timestamp = Hello.Timestamp;
		R.Ack = std::move(Ack);
		return R;
	}
}
