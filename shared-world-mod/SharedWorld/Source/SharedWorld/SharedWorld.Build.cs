using System.IO;
using UnrealBuildTool;

public class SharedWorld : ModuleRules
{
	public SharedWorld(ReadOnlyTargetRules Target) : base(Target)
	{
		CppStandard = CppStandardVersion.Cpp20;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		PCHUsage = PCHUsageMode.UseExplicitOrSharedPCHs;
		bLegacyPublicIncludePaths = false;

		PublicDependencyModuleNames.AddRange(new[] {
			"Core", "CoreUObject", "Engine", "InputCore",
			"SlateCore", "Slate", "UMG",
			"FactoryGame", "SML",
		});

		PrivateDependencyModuleNames.AddRange(new[] {
			"HTTP", "Json", "JsonUtilities", "Projects",
			// Game session APIs (UCommonSessionSubsystem, USessionInformation, ...)
			"OnlineIntegration", "OnlineServicesInterface", "CoreOnline",
			"ModelViewViewModel", "FieldNotification",
		});

		// Ship the helper executable with the mod. It is started automatically
		// by USharedWorldIPCClient when no helper is running.
		string HelperExe = Path.Combine(PluginDirectory, "ThirdParty", "SharedWorldHelper", "Win64", "shared-world-helper.exe");
		if (Target.Platform == UnrealTargetPlatform.Win64 && File.Exists(HelperExe))
		{
			RuntimeDependencies.Add(HelperExe);
		}
	}
}
