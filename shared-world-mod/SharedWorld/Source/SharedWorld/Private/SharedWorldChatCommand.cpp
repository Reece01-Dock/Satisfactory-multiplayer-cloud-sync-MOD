#include "SharedWorldChatCommand.h"

#include "Command/CommandSender.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FGPlayerController.h"
#include "SharedWorldCore/Model/Model.h"
#include "SharedWorldSubsystem.h"

ASharedWorldChatCommand::ASharedWorldChatCommand()
{
	CommandName = TEXT("sharedworld");
	Aliases.Add(TEXT("sw"));
	Usage = NSLOCTEXT("SharedWorld", "ChatUsage",
		"/sharedworld status|verify|history|players|save|stop|migrate <player|auto>|allow <player> [role]|remove <player>|open|restrict|granthost <github-user>|log|diag|dev <action>");
	MinNumberOfArguments = 0;
	bOnlyUsableByPlayer = true;
}

EExecutionStatus ASharedWorldChatCommand::ExecuteCommand_Implementation(UCommandSender* Sender, const TArray<FString>& Arguments, const FString& Label)
{
	const UGameInstance* GI = GetWorld() ? GetWorld()->GetGameInstance() : nullptr;
	USharedWorldSubsystem* SW = GI ? GI->GetSubsystem<USharedWorldSubsystem>() : nullptr;
	if (!SW)
	{
		return EExecutionStatus::UNCOMPLETED;
	}
	const FString Verb = Arguments.Num() > 0 ? Arguments[0].ToLower() : TEXT("status");
	const FString Arg = Arguments.Num() > 1 ? Arguments[1] : FString();
	const FString WorldId = SW->GetActiveWorldId();

	// Async results go back to whoever asked, if they are still there.
	TWeakObjectPtr<UCommandSender> WeakSender(Sender);
	auto Reply = [WeakSender](bool bOk, const FString& Message)
	{
		if (UCommandSender* S = WeakSender.Get())
		{
			S->SendChatMessage(Message, bOk ? FLinearColor::White : FLinearColor::Red);
		}
	};

	if (Verb == TEXT("status"))
	{
		Sender->SendChatMessage(SW->DescribeActiveSession());
		return EExecutionStatus::COMPLETED;
	}
	if (Verb == TEXT("verify"))
	{
		Sender->SendChatMessage(SW->DebugVerifyHost(Arg.IsEmpty() ? SW->GetActiveWorldId() : Arg));
		return EExecutionStatus::COMPLETED;
	}
	if (WorldId.IsEmpty())
	{
		Sender->SendChatMessage(TEXT("This game is not a Shared World session."), FLinearColor::Red);
		return EExecutionStatus::UNCOMPLETED;
	}
	if (Verb == TEXT("history"))
	{
		SW->FetchHistory(WorldId, 10, Reply);
		return EExecutionStatus::COMPLETED;
	}
	if (Verb == TEXT("players"))
	{
		SW->FetchPlayers(WorldId, Reply);
		return EExecutionStatus::COMPLETED;
	}

	// Everything below changes the world: only the hosting player (whose game
	// runs this command locally) may use it. Roles in the world's member list
	// are checked again by SharedWorldCore.
	AFGPlayerController* PC = Sender->GetPlayer();
	if (!PC || !PC->IsLocalController())
	{
		Sender->SendChatMessage(TEXT("Only the host can do that."), FLinearColor::Red);
		return EExecutionStatus::INSUFFICIENT_PERMISSIONS;
	}
	if (Verb == TEXT("save"))
	{
		Sender->SendChatMessage(SW->RequestCheckpoint());
		return EExecutionStatus::COMPLETED;
	}
	if (Verb == TEXT("stop"))
	{
		Sender->SendChatMessage(SW->RequestStop());
		return EExecutionStatus::COMPLETED;
	}
	if (Verb == TEXT("log"))
	{
		Sender->SendChatMessage(SW->RecentLog(12));
		return EExecutionStatus::COMPLETED;
	}
	if (Verb == TEXT("diag"))
	{
		Sender->SendChatMessage(SW->GetHostMigrationDiagnostics(WorldId));
		return EExecutionStatus::COMPLETED;
	}
#if !UE_BUILD_SHIPPING
	if (Verb == TEXT("dev"))
	{
		Sender->SendChatMessage(SW->DevInject(WorldId, Arg.IsEmpty() ? TEXT("leave") : Arg));
		return EExecutionStatus::COMPLETED;
	}
#endif
	if (Verb == TEXT("open") || Verb == TEXT("restrict"))
	{
		SW->SetOpenMembership(WorldId, Verb == TEXT("open"), Reply);
		return EExecutionStatus::COMPLETED;
	}
	if (Arg.IsEmpty())
	{
		PrintCommandUsage(Sender);
		return EExecutionStatus::BAD_ARGUMENTS;
	}
	if (Verb == TEXT("migrate"))
	{
		Sender->SendChatMessage(SW->RequestMigrationTo(WorldId, Arg));
		return EExecutionStatus::COMPLETED;
	}
	if (Verb == TEXT("allow"))
	{
		sw::Role AssignedRole = sw::Role::Member;
		if (Arguments.Num() > 2)
		{
			auto Parsed = sw::ParseRole(TCHAR_TO_UTF8(*Arguments[2].ToLower()));
			if (!Parsed || *Parsed == sw::Role::Owner)
			{
				Sender->SendChatMessage(TEXT("Role must be member, admin or viewer."), FLinearColor::Red);
				return EExecutionStatus::BAD_ARGUMENTS;
			}
			AssignedRole = *Parsed;
		}
		SW->AllowPlayer(WorldId, Arg, AssignedRole, Reply);
		return EExecutionStatus::COMPLETED;
	}
	if (Verb == TEXT("remove"))
	{
		SW->RemovePlayer(WorldId, Arg, Reply);
		return EExecutionStatus::COMPLETED;
	}
	if (Verb == TEXT("granthost"))
	{
		SW->GrantHosting(WorldId, Arg, Reply);
		return EExecutionStatus::COMPLETED;
	}
	PrintCommandUsage(Sender);
	return EExecutionStatus::BAD_ARGUMENTS;
}
