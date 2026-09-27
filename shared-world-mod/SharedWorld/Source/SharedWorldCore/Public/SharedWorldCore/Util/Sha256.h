#pragma once
// SHA-256 (FIPS 180-4). Streaming; no allocation.

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace sw
{
	class Sha256
	{
	public:
		using Digest = std::array<uint8_t, 32>;

		Sha256();
		void Update(const void* Data, size_t Size);
		void Update(std::string_view Data) { Update(Data.data(), Data.size()); }
		Digest Finish();

		static Digest Hash(std::string_view Data);
		/** Lowercase hex. */
		static std::string ToHex(const Digest& D);
		static std::string HexOf(std::string_view Data) { return ToHex(Hash(Data)); }
		/** True for a 64-char lowercase hex string. */
		static bool IsValidHex(std::string_view Hex);

	private:
		void Block(const uint8_t* P);

		uint32_t State[8];
		uint8_t Buffer[64];
		size_t BufferLen = 0;
		uint64_t TotalLen = 0;
	};
}
