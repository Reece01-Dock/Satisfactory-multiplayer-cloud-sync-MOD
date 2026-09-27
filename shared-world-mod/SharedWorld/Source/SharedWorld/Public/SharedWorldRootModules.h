#pragma once

#include "CoreMinimal.h"
#include "Module/GameWorldModule.h"
#include "Module/MenuWorldModule.h"
#include "SharedWorldRootModules.generated.h"

/**
 * Root menu-world module: SML creates it whenever the main menu loads.
 * Shows the Shared Worlds panel.
 */
UCLASS()
class SHAREDWORLD_API URootMenuWorld_SharedWorld : public UMenuWorldModule
{
	GENERATED_BODY()

public:
	URootMenuWorld_SharedWorld();
	virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;
};

/**
 * Root game-world module: SML creates it whenever a save is loaded or a
 * session is joined. Registers the /sharedworld chat command and tells the
 * host controller the world is ready.
 */
UCLASS()
class SHAREDWORLD_API URootGameWorld_SharedWorld : public UGameWorldModule
{
	GENERATED_BODY()

public:
	URootGameWorld_SharedWorld();
	virtual void DispatchLifecycleEvent(ELifecyclePhase Phase) override;
};
