using System.IO;
using EpicGames.Core;
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
		// Core sources live next door and use anonymous-namespace helpers with shared names.
		bUseUnity = false;

		// Compile SharedWorldCore as part of this module (static). A separate Core DLL
		// would need SHAREDWORLDCORE_API on every symbol; Core stays UE-header-free for CMake.
		string CoreDir = Path.Combine(ModuleDirectory, "..", "SharedWorldCore");
		ConditionalAddModuleDirectory(new DirectoryReference(CoreDir));
		PublicIncludePaths.Add(Path.Combine(CoreDir, "Public"));
		PrivateIncludePaths.Add(Path.Combine(CoreDir, "Private"));
		// Vendored zstd headers (Compress.cpp); .c sources under ThirdParty/zstd/lib are
		// picked up via ConditionalAddModuleDirectory.
		PublicSystemIncludePaths.Add(Path.Combine(CoreDir, "ThirdParty", "zstd", "lib"));
		PublicDefinitions.Add("ZSTD_DISABLE_ASM=1");
		PublicDefinitions.Add("XXH_NAMESPACE=ZSTD_");
		PublicDefinitions.Add("ZSTD_MULTITHREAD=0");

		PublicDependencyModuleNames.AddRange(new[] {
			"Core", "CoreUObject", "Engine", "InputCore",
			"SlateCore", "Slate", "UMG",
			"ApplicationCore",
			"FactoryGame", "SML",
		});

		PrivateDependencyModuleNames.AddRange(new[] {
			"HTTP", "Json", "JsonUtilities", "Projects",
			// Game session APIs (UCommonSessionSubsystem, USessionInformation, ...)
			"OnlineIntegration", "OnlineServicesInterface", "CoreOnline",
			"OnlineSubsystem", "OnlineSubsystemUtils",
			"ModelViewViewModel", "FieldNotification",
		});

		AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");

		// rclone engine (librclone.dll). Built from the rclone source by tools/rclone/build-librclone.ps1; loaded at run
		// time with LoadLibrary, never linked. Staged with the plugin only when it has been built, so the mod still
		// compiles and packages without it (the UI then reports "not installed").
		string RcloneDir = Path.Combine(PluginDirectory, "Binaries", "ThirdParty", "rclone");
		if (Target.Platform == UnrealTargetPlatform.Win64 && Directory.Exists(RcloneDir))
		{
			foreach (string RcloneFile in Directory.GetFiles(RcloneDir))
			{
				RuntimeDependencies.Add(RcloneFile);
			}
		}

		if (Target.Platform == UnrealTargetPlatform.Win64)
		{
			// Windows Credential Manager (CredWriteW / CredReadW / CredDeleteW):
			// where signed-in GitHub tokens are stored. Never in the save, never
			// in SharedWorldCore (which has no OS or UE dependency at all).
			PublicSystemLibraries.Add("Advapi32.lib");
		}

		// Optional packaged OAuth client id (public). Prefer baking this for
		// shipping builds so players never set SHAREDWORLD_GITHUB_CLIENT_ID.
		// Example (CI): -DSHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED=Iv1.xxxxxxxx
		string EmbeddedClientId = System.Environment.GetEnvironmentVariable("SHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED");
		if (!string.IsNullOrWhiteSpace(EmbeddedClientId))
		{
			PrivateDefinitions.Add("SHAREDWORLD_GITHUB_CLIENT_ID_EMBEDDED=\"" + EmbeddedClientId.Trim() + "\"");
		}
	}
}
