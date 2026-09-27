#include "SharedWorldCore/Util/Random.h"

namespace sw
{
	namespace
	{
		template <typename Gen>
		std::string HexFrom(Gen& G, size_t Bytes)
		{
			static const char* Digits = "0123456789abcdef";
			std::string Out;
			Out.reserve(Bytes * 2);
			for (size_t i = 0; i < Bytes; ++i)
			{
				const unsigned B = static_cast<unsigned>(G()) & 0xFF;
				Out += Digits[B >> 4];
				Out += Digits[B & 15];
			}
			return Out;
		}
	}

	std::string SystemRandom::Hex(size_t Bytes)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return HexFrom(Device, Bytes);
	}

	std::string SeededRandom::Hex(size_t Bytes)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		return HexFrom(Engine, Bytes);
	}

	std::string NewUuid(IRandom& Rng)
	{
		std::string H = Rng.Hex(16);
		H[12] = '4';                                          // version 4
		H[16] = "89ab"[(H[16] >= 'a' ? H[16] - 'a' + 10 : H[16] - '0') & 3]; // variant 10xx
		return H.substr(0, 8) + "-" + H.substr(8, 4) + "-" + H.substr(12, 4) + "-" + H.substr(16, 4) + "-" + H.substr(20, 12);
	}
}
