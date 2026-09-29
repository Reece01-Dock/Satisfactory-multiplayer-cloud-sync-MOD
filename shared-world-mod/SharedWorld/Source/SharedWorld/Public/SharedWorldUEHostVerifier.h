#pragma once
// UE transport for sw::IHostVerifier.
//
// Transport selected: OnlineIntegration session resolve
//   MakeOnlineSessionId → UCommonSessionSubsystem::ResolveOnlineSession
//
// Why: Satisfactory exposes no connectionless Shared Worlds packet channel
// before join. JoinManager already uses ResolveOnlineSession; reusing it
// verifies the published Steam/EOS session is discoverable without inventing
// a second networking stack. Packets are never authoritative — the cloud
// lease still decides who may host. ValidateHelloAck remains the validator.
//
// Layered flow:
//   1. If Join is empty → friends-list fallback (Verified, no session id).
//   2. Resolve the published online-session-id with a bounded wait.
//   3. Prefer an Ack from the local USharedWorldHostResponder when this
//      process is the host (same-machine / post-ready probes).
//   4. Otherwise build Ack from Hello + resolved session id + expected host
//      identity from the lease (filled by WorldSession before ValidateHelloAck
//      if empty), then WorldSession always runs ValidateHelloAck.

#include <memory>
#include <mutex>

#include "CoreMinimal.h"
#include "SharedWorldCore/HostHandshake/HostHandshake.h"

class UGameInstance;
class USharedWorldHostResponder;
class USharedWorldNetworkQuality;

namespace SharedWorldUe
{
	class FSharedWorldUEHostVerifier final : public sw::IHostVerifier
	{
	public:
		FSharedWorldUEHostVerifier(TWeakObjectPtr<UGameInstance> InGI,
			TWeakObjectPtr<USharedWorldHostResponder> InResponder,
			TWeakObjectPtr<USharedWorldNetworkQuality> InQuality,
			FString InExpectedHostPlayerId);

		void SetExpectedHostPlayerId(FString Id);
		sw::HostVerifyResult Probe(const sw::SharedWorldHello& Hello, const sw::JoinInfo& Join, sw::TimeMs Timeout) override;

	private:
		TWeakObjectPtr<UGameInstance> GameInstance;
		TWeakObjectPtr<USharedWorldHostResponder> Responder;
		TWeakObjectPtr<USharedWorldNetworkQuality> Quality;
		std::mutex ExpectedHostMutex;
		FString ExpectedHostPlayerId;
	};
}
