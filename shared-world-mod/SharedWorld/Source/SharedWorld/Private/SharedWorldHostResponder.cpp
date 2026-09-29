#include "SharedWorldHostResponder.h"

#include "SharedWorldSubsystem.h"
#include "SharedWorldUeConvert.h"

void USharedWorldHostResponder::Init(USharedWorldSubsystem* InOwner)
{
	Owner = InOwner;
}

void USharedWorldHostResponder::Clear()
{
	WorldId.Reset();
	HostPlayerId.Reset();
	SessionId.Reset();
	Generation = Revision = 0;
	CurrentPlayers = 0;
	MaxPlayers = 4;
	bReady = false;
}

void USharedWorldHostResponder::OnHostingReady(const FString& InWorldId, const FString& InHostPlayerId, int64 InGeneration,
	int64 InRevision, const FString& InSessionId, int32 InCurrentPlayers, int32 InMaxPlayers)
{
	WorldId = InWorldId;
	HostPlayerId = InHostPlayerId;
	Generation = InGeneration;
	Revision = InRevision;
	SessionId = InSessionId;
	CurrentPlayers = InCurrentPlayers;
	MaxPlayers = InMaxPlayers > 0 ? InMaxPlayers : 4;
	bReady = true;
	UE_LOG(LogSharedWorld, Log,
		TEXT("[SharedWorld/Verify] event=host_responder_ready world=%s generation=%lld revision=%lld session_id_hash=%u players=%d"),
		*WorldId, Generation, Revision, GetTypeHash(SessionId), CurrentPlayers);
}

bool USharedWorldHostResponder::IsReadyHostFor(const FString& InWorldId) const
{
	return bReady && !WorldId.IsEmpty() && WorldId == InWorldId;
}

TOptional<sw::SharedWorldHelloAck> USharedWorldHostResponder::TryBuildAck(const sw::SharedWorldHello& Hello) const
{
	if (!bReady) return {};
	if (Hello.WorldId != SharedWorldUe::Std(WorldId)) return {};
	// Stale Hello for a previous generation: refuse so ValidateHelloAck fails closed.
	if (Hello.ExpectedLeaseGeneration != Generation) return {};

	sw::SharedWorldHelloAck Ack;
	Ack.ProtocolVersion = sw::HostHandshakeProtocolVersion;
	Ack.WorldId = SharedWorldUe::Std(WorldId);
	Ack.HostPlayerId = SharedWorldUe::Std(HostPlayerId);
	Ack.CurrentRevision = Revision;
	Ack.LeaseGeneration = Generation;
	Ack.SessionId = SharedWorldUe::Std(SessionId);
	Ack.CurrentPlayers = CurrentPlayers;
	Ack.MaxPlayers = MaxPlayers;
	Ack.Nonce = Hello.Nonce;
	Ack.Timestamp = Hello.Timestamp;
	return Ack;
}

FString USharedWorldHostResponder::Describe() const
{
	if (!bReady)
	{
		return TEXT("Host responder: not ready");
	}
	return FString::Printf(
		TEXT("Host: %s\nLease generation: %lld\nRevision: %lld\nSession published: %s\nHostReady: yes\nPlayers: %d/%d"),
		*HostPlayerId, Generation, Revision, SessionId.IsEmpty() ? TEXT("no (friends list)") : TEXT("yes"),
		CurrentPlayers, MaxPlayers);
}
