#include "SharedWorldUEHostVerifier.h"

#include <chrono>
#include <future>

#include "Async/Async.h"
#include "CommonSessionSubsystem.h"
#include "Engine/GameInstance.h"
#include "LocalUserInfo.h"
#include "OnlineIntegrationState.h"
#include "OnlineIntegrationSubsystem.h"
#include "SessionInformation.h"
#include "SharedWorldHostResponder.h"
#include "SharedWorldNetworkQuality.h"
#include "SharedWorldTypes.h"
#include "SharedWorldUeConvert.h"

namespace SharedWorldUe
{
	FSharedWorldUEHostVerifier::FSharedWorldUEHostVerifier(TWeakObjectPtr<UGameInstance> InGI,
		TWeakObjectPtr<USharedWorldHostResponder> InResponder, TWeakObjectPtr<USharedWorldNetworkQuality> InQuality,
		FString InExpectedHostPlayerId)
		: GameInstance(MoveTemp(InGI))
		, Responder(MoveTemp(InResponder))
		, Quality(MoveTemp(InQuality))
		, ExpectedHostPlayerId(MoveTemp(InExpectedHostPlayerId))
	{
	}

	void FSharedWorldUEHostVerifier::SetExpectedHostPlayerId(FString Id)
	{
		std::lock_guard<std::mutex> Lock(ExpectedHostMutex);
		ExpectedHostPlayerId = MoveTemp(Id);
	}

	sw::HostVerifyResult FSharedWorldUEHostVerifier::Probe(const sw::SharedWorldHello& Hello, const sw::JoinInfo& Join,
		sw::TimeMs Timeout)
	{
		using clock = std::chrono::steady_clock;
		const auto Started = clock::now();
		sw::HostVerifyResult Result;
		const FString WorldId = UTF8_TO_TCHAR(Hello.WorldId.c_str());
		FString ExpectedHost;
		{
			std::lock_guard<std::mutex> Lock(ExpectedHostMutex);
			ExpectedHost = ExpectedHostPlayerId;
		}

		auto ElapsedMs = [&]() -> int32
		{
			return static_cast<int32>(std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - Started).count());
		};

		auto LogResult = [&](const TCHAR* Outcome)
		{
			UE_LOG(LogSharedWorld, Log,
				TEXT("[SharedWorld/Verify] world=%s host=%s generation=%lld attempt_rtt_ms=%d transport=OnlineIntegrationResolve result=%s session_id_hash=%u detail=%s"),
				*WorldId, *ExpectedHost, Hello.ExpectedLeaseGeneration, ElapsedMs(), Outcome,
				Join.Data.empty() ? 0u : GetTypeHash(FString(UTF8_TO_TCHAR(Join.Data.c_str()))),
				UTF8_TO_TCHAR(Result.Detail.c_str()));
		};

		// Same-process host responder (host verifying itself / local tools).
		if (USharedWorldHostResponder* Host = Responder.Get())
		{
			if (TOptional<sw::SharedWorldHelloAck> Ack = Host->TryBuildAck(Hello))
			{
				Result.Outcome = sw::HostVerifyOutcome::Verified;
				Result.Ack = *Ack;
				Result.Detail = "local_host_responder";
				Result.RttMs = ElapsedMs();
				LogResult(TEXT("Verified"));
				return Result;
			}
			if (Host->IsReadyHostFor(WorldId) && Hello.ExpectedLeaseGeneration != Host->GetGeneration())
			{
				Result.Outcome = sw::HostVerifyOutcome::Mismatch;
				Result.Detail = "lease_mismatch";
				LogResult(TEXT("LeaseMismatch"));
				return Result;
			}
		}

		if (Join.Kind != "online-session-id")
		{
			Result.Outcome = sw::HostVerifyOutcome::Unsupported;
			Result.Detail = "unsupported_join_kind";
			LogResult(TEXT("Unsupported"));
			return Result;
		}

		if (Join.Data.empty())
		{
			sw::SharedWorldHelloAck Ack;
			Ack.ProtocolVersion = Hello.ProtocolVersion;
			Ack.WorldId = Hello.WorldId;
			Ack.CurrentRevision = Hello.ExpectedRevision;
			Ack.LeaseGeneration = Hello.ExpectedLeaseGeneration;
			Ack.Nonce = Hello.Nonce;
			Ack.Timestamp = Hello.Timestamp;
			Result.Outcome = sw::HostVerifyOutcome::Verified;
			Result.Ack = std::move(Ack);
			Result.Detail = "friends_list_fallback";
			Result.RttMs = ElapsedMs();
			LogResult(TEXT("Verified"));
			return Result;
		}

		UGameInstance* GI = GameInstance.Get();
		UCommonSessionSubsystem* Sessions = GI ? GI->GetSubsystem<UCommonSessionSubsystem>() : nullptr;
		UOnlineIntegrationSubsystem* Online = GI ? GI->GetSubsystem<UOnlineIntegrationSubsystem>() : nullptr;
		UOnlineIntegrationState* State = Online ? Online->GetOnlineIntegrationState() : nullptr;
		ULocalUserInfo* User = State ? State->GetFirstUserInfo() : nullptr;
		if (!Sessions || !User)
		{
			Result.Outcome = sw::HostVerifyOutcome::Unreachable;
			Result.Detail = "online_services_unavailable";
			LogResult(TEXT("TransportError"));
			return Result;
		}

		const FString SessionStr = UTF8_TO_TCHAR(Join.Data.c_str());
		const UE::Online::FOnlineSessionId SessionId = UCommonSessionSubsystem::MakeOnlineSessionId(SessionStr);

		std::promise<TWeakObjectPtr<USessionInformation>> Promise;
		std::future<TWeakObjectPtr<USessionInformation>> Future = Promise.get_future();
		Sessions->ResolveOnlineSession(User, SessionId).Next([&Promise](USessionInformation* Found)
		{
			Promise.set_value(Found);
		});

		const auto Wait = Timeout > 0 ? std::chrono::milliseconds(Timeout) : std::chrono::milliseconds(5000);
		if (Future.wait_for(Wait) != std::future_status::ready)
		{
			Result.Outcome = sw::HostVerifyOutcome::Timeout;
			Result.Detail = "timed_out";
			LogResult(TEXT("TimedOut"));
			return Result;
		}

		TWeakObjectPtr<USessionInformation> WeakFound = Future.get();
		USessionInformation* Found = WeakFound.Get();
		if (!Found)
		{
			Result.Outcome = sw::HostVerifyOutcome::Unreachable;
			Result.Detail = "session_not_found";
			LogResult(TEXT("SessionNotFound"));
			return Result;
		}

		// Confirm the resolved handle still round-trips to the published id.
		const FCommonSession Handle = Found->GetSessionHandle();
		if (Handle.IsValid())
		{
			const FString Resolved = UCommonSessionSubsystem::OnlineSessionIdToString(Handle.GetSessionId());
			if (!Resolved.IsEmpty() && !SessionStr.IsEmpty() && Resolved != SessionStr)
			{
				Result.Outcome = sw::HostVerifyOutcome::Mismatch;
				Result.Detail = "session_id_mismatch";
				LogResult(TEXT("InvalidResponse"));
				return Result;
			}
		}

		sw::SharedWorldHelloAck Ack;
		Ack.ProtocolVersion = Hello.ProtocolVersion;
		Ack.WorldId = Hello.WorldId;
		// Leave HostPlayerId empty for remote resolve: WorldSession fills the
		// authoritative lease holder before ValidateHelloAck. A local
		// HostResponder Ack (above) already carries the real host id.
		Ack.CurrentRevision = Hello.ExpectedRevision;
		Ack.LeaseGeneration = Hello.ExpectedLeaseGeneration;
		Ack.SessionId = Join.Data;
		Ack.Nonce = Hello.Nonce;
		Ack.Timestamp = Hello.Timestamp;
		Ack.CurrentPlayers = 0;
		Ack.MaxPlayers = 4;

		Result.Outcome = sw::HostVerifyOutcome::Verified;
		Result.Ack = std::move(Ack);
		Result.Detail = "session_resolved";
		Result.RttMs = ElapsedMs();
		LogResult(TEXT("Verified"));

		if (USharedWorldNetworkQuality* NQ = Quality.Get())
		{
			NQ->ObserveVerifySample(ExpectedHost, Result.RttMs);
		}
		return Result;
	}
}
