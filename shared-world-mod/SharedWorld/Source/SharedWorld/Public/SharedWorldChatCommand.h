#pragma once

#include "Command/ChatCommandInstance.h"
#include "CoreMinimal.h"
#include "SharedWorldChatCommand.generated.h"

/**
 * In-game Shared World controls (chat commands run on the host's game):
 *   /sharedworld status              session state, world id and storage
 *   /sharedworld history             recent revisions
 *   /sharedworld players             members and roles
 * Host only:
 *   /sharedworld save                save and upload a checkpoint now
 *   /sharedworld stop                save, upload and release the world
 *   /sharedworld migrate <player>    hand hosting to a connected player
 *   /sharedworld allow <player> [member|admin|viewer]
 *   /sharedworld remove <player>
 *   /sharedworld open | restrict     who may play (anyone with storage access / members)
 *   /sharedworld granthost <github-username>   storage access so they can host
 *   /sharedworld log                 recent Shared World log lines
 * Players are Steam/Epic accounts: <player> is a connected player's name.
 */
UCLASS()
class SHAREDWORLD_API ASharedWorldChatCommand : public AChatCommandInstance
{
	GENERATED_BODY()

public:
	ASharedWorldChatCommand();
	virtual EExecutionStatus ExecuteCommand_Implementation(UCommandSender* Sender, const TArray<FString>& Arguments, const FString& Label) override;
};
