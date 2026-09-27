#pragma once

#include "Command/ChatCommandInstance.h"
#include "CoreMinimal.h"
#include "SharedWorldChatCommand.generated.h"

/**
 * In-game host controls until pause-menu buttons exist:
 *   /sharedworld status   show the shared session state
 *   /sharedworld save     save and upload a checkpoint now
 *   /sharedworld stop     save, upload and release the world
 * Only the hosting player may use save/stop.
 */
UCLASS()
class SHAREDWORLD_API ASharedWorldChatCommand : public AChatCommandInstance
{
	GENERATED_BODY()

public:
	ASharedWorldChatCommand();
	virtual EExecutionStatus ExecuteCommand_Implementation(UCommandSender* Sender, const TArray<FString>& Arguments, const FString& Label) override;
};
