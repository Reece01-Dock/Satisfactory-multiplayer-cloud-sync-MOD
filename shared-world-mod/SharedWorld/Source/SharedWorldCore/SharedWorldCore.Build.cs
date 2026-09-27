using UnrealBuildTool;

// Pure C++20 core of Shared World (no UE headers except the module entry
// point). The same sources are built and unit-tested natively by
// shared-world-mod/core-tests (CMake), so keep them UE-independent.
public class SharedWorldCore : ModuleRules
{
	public SharedWorldCore(ReadOnlyTargetRules Target) : base(Target)
	{
		CppStandard = CppStandardVersion.Cpp20;
		DefaultBuildSettings = BuildSettingsVersion.Latest;
		PCHUsage = PCHUsageMode.NoPCHs;
		bLegacyPublicIncludePaths = false;
		// Anonymous-namespace helpers may share names across files.
		bUseUnity = false;
		bEnableExceptions = false;
		bUseRTTI = false;

		PublicDependencyModuleNames.Add("Core");
		AddEngineThirdPartyPrivateStaticDependencies(Target, "zlib");
	}
}
