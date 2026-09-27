#include "SharedWorldCore/Storage/Storage.h"

namespace sw
{
	Status ValidateRepoPath(const std::string& Path)
	{
		if (Path.empty() || Path.size() > 256)
		{
			return MakeError(ErrorCode::Invalid, "invalid repository path (length)");
		}
		size_t Start = 0;
		while (Start <= Path.size())
		{
			const size_t Slash = Path.find('/', Start);
			const std::string Seg = Path.substr(Start, Slash == std::string::npos ? std::string::npos : Slash - Start);
			if (Seg.empty() || Seg == "." || Seg == "..")
			{
				return MakeError(ErrorCode::Invalid, "invalid repository path '" + Path + "'");
			}
			for (char C : Seg)
			{
				if (!((C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '.' || C == '_' || C == '-'))
				{
					return MakeError(ErrorCode::Invalid, "invalid repository path '" + Path + "'");
				}
			}
			if (Slash == std::string::npos)
			{
				break;
			}
			Start = Slash + 1;
		}
		return {};
	}
}
