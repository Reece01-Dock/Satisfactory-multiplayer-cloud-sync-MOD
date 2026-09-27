#include "SharedWorldCore/Util/Base64.h"

namespace sw
{
	std::string Base64Encode(std::string_view Data)
	{
		static const char* T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
		std::string Out;
		Out.reserve((Data.size() + 2) / 3 * 4);
		size_t i = 0;
		for (; i + 2 < Data.size(); i += 3)
		{
			const unsigned V = (unsigned(static_cast<unsigned char>(Data[i])) << 16) | (unsigned(static_cast<unsigned char>(Data[i + 1])) << 8) | static_cast<unsigned char>(Data[i + 2]);
			Out += T[V >> 18]; Out += T[(V >> 12) & 63]; Out += T[(V >> 6) & 63]; Out += T[V & 63];
		}
		if (i + 1 == Data.size())
		{
			const unsigned V = unsigned(static_cast<unsigned char>(Data[i])) << 16;
			Out += T[V >> 18]; Out += T[(V >> 12) & 63]; Out += "==";
		}
		else if (i + 2 == Data.size())
		{
			const unsigned V = (unsigned(static_cast<unsigned char>(Data[i])) << 16) | (unsigned(static_cast<unsigned char>(Data[i + 1])) << 8);
			Out += T[V >> 18]; Out += T[(V >> 12) & 63]; Out += T[(V >> 6) & 63]; Out += '=';
		}
		return Out;
	}
}
