#include "SharedWorldGameShims.h"

#include "CommonSessionTypes.h"
#include "Engine/GameInstance.h"
#include "Engine/World.h"
#include "Misc/EngineVersion.h"
#include "Online/FGSessionSettings.h"
#include "OnlineIntegrationState.h"
#include "OnlineIntegrationSubsystem.h"
#include "Sessions/SessionDefinition.h"
#include "SharedWorldTypes.h"
#include "UObject/UnrealType.h"

namespace
{
	/** Game build the reflected names below were last checked against (matches .uplugin GameVersion). */
	constexpr int32 AuditedGameChangelist = 491125;

	/** Property names on UFGSessionSettings (private members, hence reflection). */
	const FName CurrentDefinitionProperty(TEXT("mCurrentSessionDefinition"));
	const FName DefinitionNameProperty(TEXT("mSessionDefinitionName"));

	/**
	 * LoadSaveFile hosts with whatever UFGSessionSettings currently has selected.
	 * The front-end often leaves that on SessionDef_SinglePlayer (no ?listen).
	 * Preference: the current definition if it already creates an online session,
	 * then well-known names, then the first definition that creates one (logged as a
	 * fallback because the choice is then arbitrary).
	 */
	USessionDefinition* PickHostingSessionDefinition(UOnlineIntegrationState* State, UFGSessionSettings* Settings, bool& bOutFallback)
	{
		bOutFallback = false;
		if (!State)
		{
			return nullptr;
		}
		if (Settings)
		{
			if (USessionDefinition* Current = Settings->GetCurrentSessionDefinition())
			{
				if (Current->bCreateOnlineSession)
				{
					return Current;
				}
			}
		}
		static const FName Preferred[] = {
			FName(TEXT("SessionDef_Steam")),
			FName(TEXT("SessionDef_EOS")),
			FName(TEXT("SessionDef_Epic")),
			FName(TEXT("SessionDef_EOSPlus")),
			FName(TEXT("SessionDef_CrossPlay")),
			FName(TEXT("SessionDef_Friends")),
			FName(TEXT("SessionDef_Private")),
			FName(TEXT("SessionDef_IP")),
		};
		for (const FName& Name : Preferred)
		{
			if (USessionDefinition* Def = State->GetSessionDefinitionByName(Name))
			{
				if (Def->bCreateOnlineSession)
				{
					return Def;
				}
			}
		}
		for (USessionDefinition* Def : State->GetSessionDefinitions())
		{
			if (Def && Def->bCreateOnlineSession)
			{
				bOutFallback = true;
				return Def;
			}
		}
		return nullptr;
	}

	bool Fail(FString* OutError, const FString& Reason)
	{
		UE_LOG(LogSharedWorld, Error, TEXT("[SharedWorld] event=host_session_def_missing reason=\"%s\""), *Reason);
		if (OutError)
		{
			*OutError = Reason;
		}
		return false;
	}
}

namespace SharedWorldShim
{
	void WarnIfUnauditedGameBuild()
	{
		static bool bWarned = false;
		const int32 Build = static_cast<int32>(FEngineVersion::Current().GetChangelist());
		if (!bWarned && Build != AuditedGameChangelist)
		{
			bWarned = true;
			UE_LOG(LogSharedWorld, Warning,
				TEXT("[SharedWorld] event=unaudited_game_build build=%d audited=%d note=\"reflected game members are re-verified on every use; see docs/ficsit-compatibility.md\""),
				Build, AuditedGameChangelist);
		}
	}

	bool EnsureHostingSessionDefinition(UWorld* MenuWorld, FString* OutError)
	{
		WarnIfUnauditedGameBuild();
		UGameInstance* GI = MenuWorld ? MenuWorld->GetGameInstance() : nullptr;
		UOnlineIntegrationSubsystem* Online = GI ? GI->GetSubsystem<UOnlineIntegrationSubsystem>() : nullptr;
		UOnlineIntegrationState* State = Online ? Online->GetOnlineIntegrationState() : nullptr;
		UFGSessionSettings* Settings = GI ? GI->GetSubsystem<UFGSessionSettings>() : nullptr;
		if (!Settings)
		{
			return Fail(OutError, TEXT("UFGSessionSettings subsystem not available"));
		}
		bool bFallback = false;
		USessionDefinition* Def = PickHostingSessionDefinition(State, Settings, bFallback);
		if (!Def)
		{
			return Fail(OutError, TEXT("no multiplayer SessionDefinition available"));
		}
		USessionDefinition* Previous = Settings->GetCurrentSessionDefinition();
		if (Previous == Def)
		{
			UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=host_session_def name=%s already_current=1"), *Def->GetName());
			return true;
		}
		if (bFallback)
		{
			UE_LOG(LogSharedWorld, Warning,
				TEXT("[SharedWorld] event=host_session_def_fallback name=%s note=\"no preferred definition found; first online-capable one used\""),
				*Def->GetName());
		}

		// Validate the reflected members before writing anything.
		FObjectProperty* CurrentProp = FindFProperty<FObjectProperty>(UFGSessionSettings::StaticClass(), CurrentDefinitionProperty);
		if (!CurrentProp)
		{
			return Fail(OutError, TEXT("UFGSessionSettings::mCurrentSessionDefinition not found (game update?)"));
		}
		if (!CurrentProp->PropertyClass || !Def->IsA(CurrentProp->PropertyClass))
		{
			return Fail(OutError, TEXT("UFGSessionSettings::mCurrentSessionDefinition changed type (game update?)"));
		}
		CurrentProp->SetObjectPropertyValue(CurrentProp->ContainerPtrToValuePtr<void>(Settings), Def);
		FNameProperty* NameProp = FindFProperty<FNameProperty>(UFGSessionSettings::StaticClass(), DefinitionNameProperty);
		FName PreviousName;
		if (NameProp)
		{
			PreviousName = NameProp->GetPropertyValue_InContainer(Settings);
			NameProp->SetPropertyValue_InContainer(Settings, Def->GetFName());
		}
		else
		{
			UE_LOG(LogSharedWorld, Warning, TEXT("[SharedWorld] event=host_session_def_name_property_missing note=\"mSessionDefinitionName not found; only the definition object was set\""));
		}

		// The public getter is the source of truth for whether the write took.
		if (Settings->GetCurrentSessionDefinition() != Def)
		{
			CurrentProp->SetObjectPropertyValue(CurrentProp->ContainerPtrToValuePtr<void>(Settings), Previous);
			if (NameProp)
			{
				NameProp->SetPropertyValue_InContainer(Settings, PreviousName);
			}
			return Fail(OutError, TEXT("session definition write did not take effect; restored previous selection"));
		}
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld] event=host_session_def name=%s applied=1"), *Def->GetName());
		return true;
	}
}
