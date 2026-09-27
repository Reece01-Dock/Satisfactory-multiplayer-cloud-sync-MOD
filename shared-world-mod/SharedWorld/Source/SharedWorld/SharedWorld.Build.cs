using UnrealBuildTool;

public class SharedWorld : ModuleRules
{
	public SharedWorld(ReadOnlyTargetRules Target) : base(Target)
	{
		CppStandard = CppStandardVersion.Cpp20;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bLegacyPublicIncludePaths = false;
		bEnableExceptions = false; // SharedWorldCore is exception-free; keep the boundary honest

		PublicDependencyModuleNames.AddRange(new[] {
			"Core", "CoreUObject", "Engine", "InputCore",
			"SlateCore", "Slate", "UMG",
			"FactoryGame", "SML",
			"SharedWorldCore",
		});

		PrivateDependencyModuleNames.AddRange(new[] {
			"HTTP", "Json", "JsonUtilities", "Projects",
			// Game session APIs (UCommonSessionSubsystem, USessionInformation, ...)
			"OnlineIntegration", "OnlineServicesInterface", "CoreOnline",
			"ModelViewViewModel", "FieldNotification",
		});

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Windows Credential Manager (CredWriteW / CredReadW / CredDeleteW):
			// where signed-in GitHub tokens are stored. Never in the save, never
			// in SharedWorldCore (which has no OS or UE dependency at all).
			PublicSystemLibraries.Add("Advapi32.lib");
		}
	}
}
