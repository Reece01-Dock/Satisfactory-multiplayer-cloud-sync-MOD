#include "SharedWorldUEHostVerifier.h"

#include <chrono>
#include <future>
#include <memory>

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

		if (IsInGameThread())
		{
			// Probe blocks until the game thread has run the resolve below; calling it
			// from the game thread would wait on itself.
			Result.Outcome = sw::HostVerifyOutcome::Unsupported;
			Result.Detail = "probe_called_on_game_thread";
			LogResult(TEXT("Unsupported"));
			return Result;
		}

		// Every UObject / OnlineIntegration touch happens on the game thread. Only
		// plain data (no UObject pointers) crosses back, and the promise is shared
		// so a resolve that completes after a timeout cannot write to a dead stack frame.
		struct FStageOne
		{
			enum class EKind { LocalAck, LeaseMismatch, NothingToResolve, ServicesUnavailable, NotFound, Resolved };
			EKind Kind = EKind::ServicesUnavailable;
			TOptional<sw::SharedWorldHelloAck> Ack;
			FString ResolvedId;
		};
		auto Promise = std::make_shared<std::promise<FStageOne>>();
		std::future<FStageOne> Future = Promise->get_future();

		const FString SessionStr = UTF8_TO_TCHAR(Join.Data.c_str());
		const bool bResolvable = Join.Kind == "online-session-id" && !Join.Data.empty();
		TWeakObjectPtr<UGameInstance> WeakGI = GameInstance;
		TWeakObjectPtr<USharedWorldHostResponder> WeakResponder = Responder;
		AsyncTask(ENamedThreads::GameThread, [Promise, WeakGI, WeakResponder, Hello, WorldId, SessionStr, bResolvable]()
		{
			FStageOne Out;
			// Same-process host responder (host verifying itself / local tools).
			if (USharedWorldHostResponder* Host = WeakResponder.Get())
			{
				if (TOptional<sw::SharedWorldHelloAck> Ack = Host->TryBuildAck(Hello))
				{
					Out.Kind = FStageOne::EKind::LocalAck;
					Out.Ack = MoveTemp(Ack);
					Promise->set_value(MoveTemp(Out));
					return;
				}
				if (Host->IsReadyHostFor(WorldId) && Hello.ExpectedLeaseGeneration != Host->GetGeneration())
				{
					Out.Kind = FStageOne::EKind::LeaseMismatch;
					Promise->set_value(MoveTemp(Out));
					return;
				}
			}
			if (!bResolvable)
			{
				Out.Kind = FStageOne::EKind::NothingToResolve;
				Promise->set_value(MoveTemp(Out));
				return;
			}
			UGameInstance* GI = WeakGI.Get();
			UCommonSessionSubsystem* Sessions = GI ? GI->GetSubsystem<UCommonSessionSubsystem>() : nullptr;
			UOnlineIntegrationSubsystem* Online = GI ? GI->GetSubsystem<UOnlineIntegrationSubsystem>() : nullptr;
			UOnlineIntegrationState* State = Online ? Online->GetOnlineIntegrationState() : nullptr;
			ULocalUserInfo* User = State ? State->GetFirstUserInfo() : nullptr;
			if (!Sessions || !User || SessionStr.IsEmpty())
			{
				Out.Kind = FStageOne::EKind::ServicesUnavailable;
				Promise->set_value(MoveTemp(Out));
				return;
			}
			const UE::Online::FOnlineSessionId SessionId = UCommonSessionSubsystem::MakeOnlineSessionId(SessionStr);
			Sessions->ResolveOnlineSession(User, SessionId).Next([Promise](USessionInformation* Found)
			{
				// The future may complete on any thread; read the handle here and pass plain data.
				FStageOne Done;
				const FCommonSession Handle = Found ? Found->GetSessionHandle() : FCommonSession();
				if (!Found)
				{
					Done.Kind = FStageOne::EKind::NotFound;
				}
				else
				{
					Done.Kind = FStageOne::EKind::Resolved;
					if (Handle.IsValid())
					{
						Done.ResolvedId = UCommonSessionSubsystem::OnlineSessionIdToString(Handle.GetSessionId());
					}
				}
				Promise->set_value(MoveTemp(Done));
			});
		});

		const auto Wait = Timeout > 0 ? std::chrono::milliseconds(Timeout) : std::chrono::milliseconds(5000);
		if (Future.wait_for(Wait) != std::future_status::ready)
		{
			Result.Outcome = sw::HostVerifyOutcome::Timeout;
			Result.Detail = "timed_out";
			LogResult(TEXT("TimedOut"));
			return Result;
		}
		FStageOne Stage = Future.get();
		switch (Stage.Kind)
		{
		case FStageOne::EKind::LocalAck:
			Result.Outcome = sw::HostVerifyOutcome::Verified;
			Result.Ack = *Stage.Ack;
			Result.Detail = "local_host_responder";
			Result.RttMs = ElapsedMs();
			LogResult(TEXT("Verified"));
			return Result;
		case FStageOne::EKind::LeaseMismatch:
			Result.Outcome = sw::HostVerifyOutcome::Mismatch;
			Result.Detail = "lease_mismatch";
			LogResult(TEXT("LeaseMismatch"));
			return Result;
		case FStageOne::EKind::NothingToResolve:
		{
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
			Result.Outcome = sw::HostVerifyOutcome::Unreachable; // unreachable: bResolvable is exactly the two checks above
			Result.Detail = "nothing_to_resolve";
			LogResult(TEXT("TransportError"));
			return Result;
		}
		case FStageOne::EKind::ServicesUnavailable:
			Result.Outcome = sw::HostVerifyOutcome::Unreachable;
			Result.Detail = "online_services_unavailable";
			LogResult(TEXT("TransportError"));
			return Result;
		case FStageOne::EKind::NotFound:
			Result.Outcome = sw::HostVerifyOutcome::Unreachable;
			Result.Detail = "session_not_found";
			LogResult(TEXT("SessionNotFound"));
			return Result;
		case FStageOne::EKind::Resolved:
			break;
		}

		// Confirm the resolved handle still round-trips to the published id.
		if (!Stage.ResolvedId.IsEmpty() && Stage.ResolvedId != SessionStr)
		{
			Result.Outcome = sw::HostVerifyOutcome::Mismatch;
			Result.Detail = "session_id_mismatch";
			LogResult(TEXT("InvalidResponse"));
			return Result;
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

		// UObject: record on the game thread.
		AsyncTask(ENamedThreads::GameThread, [WeakQuality = Quality, ExpectedHost, Rtt = Result.RttMs]()
		{
			if (USharedWorldNetworkQuality* NQ = WeakQuality.Get())
			{
				NQ->ObserveVerifySample(ExpectedHost, Rtt);
			}
		});
		return Result;
	}
}
