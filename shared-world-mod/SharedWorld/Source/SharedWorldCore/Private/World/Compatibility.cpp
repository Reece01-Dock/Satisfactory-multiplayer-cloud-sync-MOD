#include "SharedWorldCore/World/Compatibility.h"

#include <algorithm>
#include <cstdlib>
#include <vector>

namespace sw
{
	static std::vector<long long> Parts(const std::string& V)
	{
		std::vector<long long> Out;
		size_t Start = 0;
		while (Start <= V.size())
		{
			const size_t Dot = V.find('.', Start);
			const std::string P = V.substr(Start, Dot == std::string::npos ? std::string::npos : Dot - Start);
			Out.push_back(std::strtoll(P.c_str(), nullptr, 10));
			if (Dot == std::string::npos) break;
			Start = Dot + 1;
		}
		return Out;
	}

	int CompareVersions(const std::string& A, const std::string& B)
	{
		const auto PA = Parts(A), PB = Parts(B);
		for (size_t i = 0; i < (std::max)(PA.size(), PB.size()); ++i)
		{
			const long long X = i < PA.size() ? PA[i] : 0, Y = i < PB.size() ? PB[i] : 0;
			if (X != Y) return X < Y ? -1 : 1;
		}
		return 0;
	}

	static bool SameCompatLine(const std::string& A, const std::string& B)
	{
		const auto PA = Parts(A), PB = Parts(B);
		const long long MajA = PA.empty() ? 0 : PA[0], MajB = PB.empty() ? 0 : PB[0];
		if (MajA != MajB) return false;
		if (MajA == 0) // 0.x: every minor is a breaking line
		{
			return (PA.size() > 1 ? PA[1] : 0) == (PB.size() > 1 ? PB[1] : 0);
		}
		return true;
	}

	Status CheckCompatibility(const LocalVersions& Local, const WorldInfo& Info, const std::optional<RevisionMeta>& Head)
	{
		if (Head && !Head->GameBuild.empty() && !Local.GameBuild.empty() && CompareVersions(Head->GameBuild, Local.GameBuild) > 0)
		{
			return MakeError(ErrorCode::Unsupported, "This Shared World was last saved by a newer Satisfactory version (" + Head->GameBuild +
				"). Update the game (yours is " + Local.GameBuild + ").");
		}
		if (!Info.ModVersion.empty() && !Local.ModVersion.empty() && !SameCompatLine(Info.ModVersion, Local.ModVersion))
		{
			return MakeError(ErrorCode::Unsupported, "This Shared World requires Shared World mod " + Info.ModVersion + " (you have " + Local.ModVersion + ").");
		}
		for (const RequiredMod& Req : Info.RequiredMods)
		{
			const RequiredMod* Have = nullptr;
			for (const RequiredMod& M : Local.InstalledMods)
			{
				if (M.ModReference == Req.ModReference) Have = &M;
			}
			if (!Have)
			{
				return MakeError(ErrorCode::Unsupported, "This Shared World requires the mod " + Req.ModReference + " " + Req.Version + ".");
			}
			if (!SameCompatLine(Have->Version, Req.Version))
			{
				return MakeError(ErrorCode::Unsupported, "This Shared World requires " + Req.ModReference + " " + Req.Version + " (you have " + Have->Version + ").");
			}
		}
		return {};
	}
}
