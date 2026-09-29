#include "Modules/ModuleManager.h"
#include "SharedWorldPanel.h"
#include "SharedWorldRootModules.h"
#include "SharedWorldSubsystem.h"
#include "SharedWorldTypes.h"
#include "UI/SharedWorldBrowserWidget.h"
#include "UI/SharedWorldDetailsWidget.h"
#include "UI/SharedWorldGameInstanceModule.h"
#include "UI/SharedWorldMainMenuButton.h"
#include "UI/SharedWorldMigrationOverlay.h"
#include "UI/SharedWorldSessionMenuButton.h"
#include "UI/SharedWorldSessionWidget.h"
#include "UI/SharedWorldWorldCard.h"

DEFINE_LOG_CATEGORY(LogSharedWorld);

/**
 * Shipping builds strip unreferenced UObject translation units. Without this
 * StartupModule forcing StaticClass(), SML never discovers our MenuWorldModule
 * and the SHARED WORLDS panel never spawns.
 */
class FSharedWorldModule : public IModuleInterface
{
public:
	virtual void StartupModule() override
	{
		(void)URootMenuWorld_SharedWorld::StaticClass();
		(void)URootGameWorld_SharedWorld::StaticClass();
		(void)USharedWorldPanel::StaticClass();
		(void)USharedWorldEntry::StaticClass();
		(void)USharedWorldSubsystem::StaticClass();
		(void)USharedWorldGameInstanceModule::StaticClass();
		(void)USharedWorldMainMenuButton::StaticClass();
		(void)USharedWorldSessionMenuButton::StaticClass();
		(void)USharedWorldBrowserWidget::StaticClass();
		(void)USharedWorldDetailsWidget::StaticClass();
		(void)USharedWorldSessionWidget::StaticClass();
		(void)USharedWorldMigrationOverlay::StaticClass();
		(void)USharedWorldWorldCard::StaticClass();
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=module_startup"));
	}
};

IMPLEMENT_MODULE(FSharedWorldModule, SharedWorld);
