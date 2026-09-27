#include "SharedWorldSubsystem.h"

#include "Blueprint/UserWidget.h"
#include "Dom/JsonObject.h"
#include "Engine/GameInstance.h"
#include "Engine/LocalPlayer.h"
#include "Engine/World.h"
#include "FGSaveSystem.h"
#include "GameFramework/OnlineReplStructs.h"
#include "GameFramework/PlayerController.h"
#include "HAL/PlatformMisc.h"
#include "HAL/PlatformProcess.h"
#include "JsonObjectConverter.h"
#include "Misc/Paths.h"
#include "SharedWorldHostController.h"
#include "SharedWorldIPCClient.h"
#include "SharedWorldJoinManager.h"
#include "SharedWorldPanel.h"

namespace
{
	constexpr double ActivePollSeconds = 1.0;
	constexpr double IdlePollSeconds = 5.0;
	constexpr double KeepaliveSeconds = 10.0;
}

void USharedWorldSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	IPC = NewObject<USharedWorldIPCClient>(this);
	Host = NewObject<USharedWorldHostController>(this);
	Host->Init(this);
	Joiner = NewObject<USharedWorldJoinManager>(this);
	Joiner->Init(this);
	IPC->Connect();
	TickHandle = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateUObject(this, &USharedWorldSubsystem::Tick), 0.25f);
	TearDownHandle = FWorldDelegates::OnWorldBeginTearDown.AddUObject(this, &USharedWorldSubsystem::OnWorldBeginTearDown);
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=subsystem_initialized"));
}

void USharedWorldSubsystem::Deinitialize()
{
	FTSTicker::GetCoreTicker().RemoveTicker(TickHandle);
	FWorldDelegates::OnWorldBeginTearDown.Remove(TearDownHandle);
	Super::Deinitialize();
}

FString USharedWorldSubsystem::GetConnectionProblem() const
{
	if (IPC && IPC->IsConnected())
	{
		return FString();
	}
	return IPC && !IPC->GetLastError().IsEmpty() ? IPC->GetLastError() : TEXT("Connecting to the Shared World helper...");
}

bool USharedWorldSubsystem::Tick(float)
{
	if (!IPC->IsConnected())
	{
		IPC->Connect();
		return true;
	}
	const double Now = FPlatformTime::Seconds();
	bool bActive = Host->IsBusy();
	for (const FSharedWorldStatus& W : Worlds)
	{
		bActive |= !W.Local.IsIdle();
	}
	if (!bPollInFlight && Now - LastPoll >= (bActive ? ActivePollSeconds : IdlePollSeconds))
	{
		PollWorlds();
	}
	if (bActive && Now - LastKeepalive >= KeepaliveSeconds)
	{
		SendKeepalives();
	}
	return true;
}

void USharedWorldSubsystem::PollWorlds()
{
	bPollInFlight = true;
	LastPoll = FPlatformTime::Seconds();
	TWeakObjectPtr<USharedWorldSubsystem> WeakThis(this);
	IPC->Request(TEXT("GET"), TEXT("/v1/worlds"), nullptr, FSharedWorldIPCResponse::CreateLambda(
		[WeakThis](bool bOk, int32, TSharedPtr<FJsonObject> Json)
	{
		USharedWorldSubsystem* Self = WeakThis.Get();
		if (!Self)
		{
			return;
		}
		Self->bPollInFlight = false;
		const TArray<TSharedPtr<FJsonValue>>* List = nullptr;
		if (!bOk || !Json.IsValid() || !Json->TryGetArrayField(TEXT("worlds"), List))
		{
			Self->OnChanged.Broadcast();
			return;
		}
		TArray<FSharedWorldStatus> NewWorlds;
		for (const TSharedPtr<FJsonValue>& V : *List)
		{
			FSharedWorldStatus S;
			if (V.IsValid() && ParseStatus(V->AsObject(), S))
			{
				NewWorlds.Add(MoveTemp(S));
			}
		}
		Self->Worlds = MoveTemp(NewWorlds);
		for (const FSharedWorldStatus& S : Self->Worlds)
		{
			Self->HandleSession(S.Local);
		}
		Self->OnChanged.Broadcast();
	}));
}

bool USharedWorldSubsystem::ParseSession(const TSharedPtr<FJsonObject>& Json, FSharedWorldSession& Out)
{
	if (!Json.IsValid() || !FJsonObjectConverter::JsonObjectToUStruct(Json.ToSharedRef(), &Out, 0, 0))
	{
		return false;
	}
	Out.bHasError = Json->HasTypedField<EJson::Object>(TEXT("error"));
	Out.bHasJoin = Json->HasTypedField<EJson::Object>(TEXT("join"));
	return true;
}

bool USharedWorldSubsystem::ParseStatus(const TSharedPtr<FJsonObject>& Json, FSharedWorldStatus& Out)
{
	if (!Json.IsValid() || !FJsonObjectConverter::JsonObjectToUStruct(Json.ToSharedRef(), &Out, 0, 0))
	{
		return false;
	}
	const TSharedPtr<FJsonObject>* Local = nullptr;
	if (Json->TryGetObjectField(TEXT("local"), Local))
	{
		ParseSession(*Local, Out.Local);
	}
	return !Out.WorldId.IsEmpty();
}

const FSharedWorldStatus* USharedWorldSubsystem::FindWorld(const FString& WorldId) const
{
	return Worlds.FindByPredicate([&](const FSharedWorldStatus& S) { return S.WorldId == WorldId; });
}

void USharedWorldSubsystem::HandleSession(const FSharedWorldSession& Session)
{
	const FString Prev = LastStates.FindRef(Session.WorldId);
	LastStates.Add(Session.WorldId, Session.State);
	if (Prev != Session.State)
	{
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=session_state world=%s from=%s to=%s decision=%s generation=%lld revision=%lld"),
			*Session.WorldId, *Prev, *Session.State, *Session.Decision, Session.Generation, Session.Revision);
	}

	if (Session.State == TEXT("READY_TO_HOST") && !Host->IsBusy())
	{
		FString Error;
		if (!Host->BeginHosting(MenuWorld.Get(), Session.WorldId, Session.SaveName, Error))
		{
			UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=host_load_failed world=%s reason=\"%s\""), *Session.WorldId, *Error);
			SendSessionEvent(Session.WorldId, TEXT("abort"), nullptr);
		}
	}
	else if (Session.State == TEXT("HOSTING") && Host->NeedsFinalUpload(Session.WorldId))
	{
		// The world ended (e.g. exit to menu) while a checkpoint was uploading.
		Host->SendFinalUpload();
	}
	else if (Session.State == TEXT("JOIN_READY") && Session.bHasJoin)
	{
		const FString Key = FString::Printf(TEXT("%s#%s#%s"), *Session.WorldId, *Session.HostName, *Session.Join.Value);
		if (!JoinsStarted.Contains(Key))
		{
			JoinsStarted.Add(Key);
			FString Error;
			if (Joiner->Join(MenuWorld.Get(), Session.Join, Error))
			{
				SendSessionEvent(Session.WorldId, TEXT("ack"), nullptr);
			}
			else
			{
				UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=join_failed world=%s reason=\"%s\""), *Session.WorldId, *Error);
			}
		}
	}
}

TSharedPtr<FJsonObject> USharedWorldSubsystem::MakePlayRequest() const
{
	FString PlayerId, DisplayName, Platform = TEXT("unknown");
	if (const ULocalPlayer* LP = GetGameInstance()->GetFirstGamePlayer())
	{
		const FUniqueNetIdRepl NetId = LP->GetPreferredUniqueNetId();
		if (NetId.IsValid())
		{
			PlayerId = NetId.ToString();
			Platform = NetId.GetType().ToString();
		}
		DisplayName = LP->GetNickname();
	}
	if (PlayerId.IsEmpty())
	{
		// Not logged in to an online service: fall back to a stable per-machine id.
		PlayerId = TEXT("local:") + FPlatformMisc::GetLoginId();
	}
	if (DisplayName.IsEmpty())
	{
		DisplayName = FPlatformProcess::UserName();
	}
	TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
	Body->SetStringField(TEXT("playerId"), PlayerId.Left(128));
	Body->SetStringField(TEXT("displayName"), DisplayName.Left(64));
	Body->SetStringField(TEXT("platform"), Platform.Left(32));
	// Ask the game where saves live instead of guessing paths.
	Body->SetStringField(TEXT("saveDirectory"), FPaths::ConvertRelativePathToFull(UFGSaveSystem::GetSaveDirectoryPath()));
	Body->SetNumberField(TEXT("gamePid"), FPlatformProcess::GetCurrentProcessId());
	return Body;
}

void USharedWorldSubsystem::Play(const FString& WorldId)
{
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=play_pressed world=%s"), *WorldId);
	TWeakObjectPtr<USharedWorldSubsystem> WeakThis(this);
	IPC->Request(TEXT("POST"), FString::Printf(TEXT("/v1/worlds/%s/play"), *WorldId), MakePlayRequest(),
		FSharedWorldIPCResponse::CreateLambda([WeakThis](bool, int32, TSharedPtr<FJsonObject>)
	{
		if (USharedWorldSubsystem* Self = WeakThis.Get())
		{
			Self->PollWorlds();
		}
	}));
}

void USharedWorldSubsystem::Dismiss(const FString& WorldId)
{
	SendSessionEvent(WorldId, TEXT("ack"), nullptr);
}

void USharedWorldSubsystem::Cancel(const FString& WorldId)
{
	SendSessionEvent(WorldId, TEXT("abort"), nullptr);
}

void USharedWorldSubsystem::SendSessionEvent(const FString& WorldId, const FString& Event, const TSharedPtr<FJsonObject>& Body,
	TFunction<void(bool, int32)> OnDone)
{
	TWeakObjectPtr<USharedWorldSubsystem> WeakThis(this);
	IPC->Request(TEXT("POST"), FString::Printf(TEXT("/v1/worlds/%s/session/%s"), *WorldId, *Event), Body,
		FSharedWorldIPCResponse::CreateLambda([WeakThis, WorldId, Event, OnDone](bool bOk, int32 Code, TSharedPtr<FJsonObject>)
	{
		if (!bOk)
		{
			UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=session_event_failed world=%s event=%s http=%d"), *WorldId, *Event, Code);
		}
		if (OnDone)
		{
			OnDone(bOk, Code);
		}
		if (USharedWorldSubsystem* Self = WeakThis.Get())
		{
			Self->PollWorlds();
		}
	}));
}

void USharedWorldSubsystem::SendKeepalives()
{
	LastKeepalive = FPlatformTime::Seconds();
	for (const FSharedWorldStatus& W : Worlds)
	{
		if (W.Local.IsIdle())
		{
			continue;
		}
		TSharedPtr<FJsonObject> Body = MakeShared<FJsonObject>();
		TArray<TSharedPtr<FJsonValue>> Players;
		if (Host->IsHostingWorld(W.WorldId))
		{
			for (const FSharedWorldPlayer& P : Host->GetPlayers())
			{
				TSharedPtr<FJsonObject> PO = MakeShared<FJsonObject>();
				PO->SetStringField(TEXT("displayName"), P.DisplayName.Left(64));
				Players.Add(MakeShared<FJsonValueObject>(PO));
			}
			Body->SetArrayField(TEXT("players"), Players);
		}
		IPC->Request(TEXT("POST"), FString::Printf(TEXT("/v1/worlds/%s/session/keepalive"), *W.WorldId), Body, FSharedWorldIPCResponse());
	}
}

void USharedWorldSubsystem::OnMenuWorldReady(UWorld* World)
{
	MenuWorld = World;
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	if (!PC)
	{
		return;
	}
	if (USharedWorldPanel* Panel = CreateWidget<USharedWorldPanel>(PC, USharedWorldPanel::StaticClass()))
	{
		Panel->AddToViewport(10);
	}
	PollWorlds();
}

void USharedWorldSubsystem::OnGameWorldReady(UWorld* World)
{
	MenuWorld.Reset();
	Host->OnGameWorldReady(World);
}

void USharedWorldSubsystem::OnWorldBeginTearDown(UWorld* World)
{
	Host->OnWorldTearDown(World);
}

FString USharedWorldSubsystem::RequestCheckpoint()
{
	return Host->SaveAndUpload(false);
}

FString USharedWorldSubsystem::RequestStop()
{
	return Host->SaveAndUpload(true);
}

FString USharedWorldSubsystem::DescribeActiveSession() const
{
	for (const FSharedWorldStatus& W : Worlds)
	{
		if (!W.Local.IsIdle())
		{
			return FString::Printf(TEXT("%s: %s (revision %lld). %s"), *W.WorldName, *W.Local.State, W.Local.Revision, *W.Local.Message);
		}
	}
	return TEXT("No shared world session is active.");
}
