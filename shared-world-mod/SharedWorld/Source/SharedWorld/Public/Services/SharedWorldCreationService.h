#pragma once
// Simple creation: existing save / current world. Auto default storage.

#include "CoreMinimal.h"
#include "Services/SharedWorldDiscoveryService.h"
#include "SharedWorldCore/App/LocalSettings.h"

class USharedWorldSubsystem;

class FSharedWorldCreationService
{
public:
	using FDone = TFunction<void(bool bOk, const FString& Message)>;

	explicit FSharedWorldCreationService(USharedWorldSubsystem& InSW) : SW(InSW) {}

	TArray<FSharedWorldSaveInfo> ListLocalSaves() const;
	void CreateFromExistingSave(const FString& DisplayName, const FString& SaveName, FDone OnDone);
	void CreateFromCurrentWorld(const FString& DisplayName, FDone OnDone);
	sw::ProviderConfig ResolveDefaultProvider(FString& OutNote) const;
	bool NeedsStorageConnect() const;

private:
	USharedWorldSubsystem& SW;
};
