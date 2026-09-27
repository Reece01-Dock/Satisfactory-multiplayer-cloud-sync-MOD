#include "SharedWorldChatCommand.h"

#include "Command/CommandSender.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "FGPlayerController.h"
#include "SharedWorldSubsystem.h"

ASharedWorldChatCommand::ASharedWorldChatCommand()
{
	CommandName = TEXT("sharedworld");
	Aliases.Add(TEXT("sw"));
	Usage = NSLOCTEXT("SharedWorld", "ChatUsage", "/sharedworld [status|save|stop]");
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
	if (Verb == TEXT("status"))
	{
		Sender->SendChatMessage(SW->DescribeActiveSession());
		return EExecutionStatus::COMPLETED;
	}
	// Commands run on the server; only the local (hosting) player may save or stop.
	AFGPlayerController* PC = Sender->GetPlayer();
	if (!PC || !PC->IsLocalController())
	{
		Sender->SendChatMessage(TEXT("Only the host can save or stop the shared world."), FLinearColor::Red);
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
	PrintCommandUsage(Sender);
	return EExecutionStatus::BAD_ARGUMENTS;
}
