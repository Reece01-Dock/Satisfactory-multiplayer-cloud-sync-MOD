#include "SharedWorldRootModules.h"

#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "SharedWorldChatCommand.h"
#include "SharedWorldSubsystem.h"
#include "SharedWorldTypes.h"

namespace
{
	USharedWorldSubsystem* GetSharedWorld(const UWorld* World)
	{
		const UGameInstance* GI = World ? World->GetGameInstance() : nullptr;
		return GI ? GI->GetSubsystem<USharedWorldSubsystem>() : nullptr;
	}
}

URootMenuWorld_SharedWorld::URootMenuWorld_SharedWorld()
{
	bRootModule = true;
}

void URootMenuWorld_SharedWorld::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	Super::DispatchLifecycleEvent(Phase);
	if (Phase == ELifecyclePhase::POST_INITIALIZATION)
	{
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=root_menu_post_init"));
		if (USharedWorldSubsystem* SW = GetSharedWorld(GetWorld()))
		{
			SW->OnMenuWorldReady(GetWorld());
		}
		else
		{
			UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=root_menu_no_subsystem"));
		}
	}
}

URootGameWorld_SharedWorld::URootGameWorld_SharedWorld()
{
	bRootModule = true;
	mChatCommands.Add(ASharedWorldChatCommand::StaticClass());
}

void URootGameWorld_SharedWorld::DispatchLifecycleEvent(ELifecyclePhase Phase)
{
	Super::DispatchLifecycleEvent(Phase);
	if (Phase == ELifecyclePhase::POST_INITIALIZATION)
	{
		if (USharedWorldSubsystem* SW = GetSharedWorld(GetWorld()))
		{
			SW->OnGameWorldReady(GetWorld());
		}
	}
}
