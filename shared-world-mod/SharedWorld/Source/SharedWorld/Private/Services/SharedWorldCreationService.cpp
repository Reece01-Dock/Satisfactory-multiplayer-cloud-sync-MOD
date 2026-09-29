#include "Services/SharedWorldCreationService.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FGSaveManagerInterface.h"
#include "FGSaveSession.h"
#include "FGSaveSystem.h"
#include "SharedWorldSubsystem.h"
#include "SharedWorldUeConvert.h"

TArray<FSharedWorldSaveInfo> FSharedWorldCreationService::ListLocalSaves() const
{
	TArray<FSharedWorldSaveInfo> Out;
	UWorld* World = SW.GetGameInstance() ? SW.GetGameInstance()->GetWorld() : nullptr;
	UFGSaveSystem* Saves = World ? UFGSaveSystem::Get(World) : nullptr;
	if (!Saves) return Out;

	TArray<FSaveHeader> Headers = Saves->NativeEnumerateSaveGamesSync();
	UFGSaveSystem::SortSaves(Headers, ESaveSortMode::SSM_Time, ESaveSortDirection::SSD_Descending);
	const FDateTime Today = FDateTime::Now().GetDate();
	for (const FSaveHeader& H : Headers)
	{
		const FString Name = UFGSaveSession::GetName(H);
		if (Name.IsEmpty()) continue;
		if (USharedWorldSubsystem::IsSharedWorldSaveName(Name)) continue;
		FSharedWorldSaveInfo Info;
		Info.SaveName = Name;
		Info.SessionName = H.SessionName;
		Info.SaveDate = H.SaveDateTime;
		const int32 Days = (Today - H.SaveDateTime.GetDate()).GetDays();
		if (Days <= 0) Info.LastPlayedText = TEXT("Today");
		else if (Days == 1) Info.LastPlayedText = TEXT("Yesterday");
		else Info.LastPlayedText = FString::Printf(TEXT("%d days ago"), Days);
		Out.Add(MoveTemp(Info));
	}
	return Out;
}

sw::ProviderConfig FSharedWorldCreationService::ResolveDefaultProvider(FString& OutNote) const
{
	return SW.ResolveDefaultStorage(OutNote);
}

bool FSharedWorldCreationService::NeedsStorageConnect() const
{
	return SW.NeedsWelcomeStorageConnect();
}

void FSharedWorldCreationService::CreateFromExistingSave(const FString& DisplayName, const FString& SaveName, FDone OnDone)
{
	FString Note;
	const sw::ProviderConfig Provider = ResolveDefaultProvider(Note);
	if (!SW.IsWelcomeDone())
	{
		SW.MarkWelcomeDone();
	}
	SW.CreateWorldFromSave(DisplayName, SaveName, Provider, /*bRestrictToMembers=*/true, OnDone);
}

void FSharedWorldCreationService::CreateFromCurrentWorld(const FString& DisplayName, FDone OnDone)
{
	SW.CreateWorldFromCurrentSession(DisplayName, OnDone);
}
