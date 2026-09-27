#include "SharedWorldCore/Providers/Http.h"

#include <cctype>

namespace sw
{
	std::string HttpResponse::Header(const std::string& Name) const
	{
		for (const auto& [K, V] : Headers)
		{
			if (K.size() != Name.size()) continue;
			bool bSame = true;
			for (size_t i = 0; i < K.size() && bSame; ++i)
			{
				bSame = std::tolower(static_cast<unsigned char>(K[i])) == std::tolower(static_cast<unsigned char>(Name[i]));
			}
			if (bSame) return V;
		}
		return std::string();
	}
}
