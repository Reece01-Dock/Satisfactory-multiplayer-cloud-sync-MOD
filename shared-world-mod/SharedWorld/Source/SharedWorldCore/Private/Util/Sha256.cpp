#include "SharedWorldCore/Util/Sha256.h"

#include <cstring>

namespace sw
{
	namespace
	{
		constexpr uint32_t K[64] = {
			0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
			0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
			0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
			0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
			0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
			0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
			0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
			0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

		inline uint32_t Rotr(uint32_t X, int N) { return (X >> N) | (X << (32 - N)); }
	}

	Sha256::Sha256()
		: State{0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19}
	{
	}

	void Sha256::Block(const uint8_t* P)
	{
		uint32_t W[64];
		for (int i = 0; i < 16; ++i)
		{
			W[i] = (uint32_t(P[i * 4]) << 24) | (uint32_t(P[i * 4 + 1]) << 16) | (uint32_t(P[i * 4 + 2]) << 8) | uint32_t(P[i * 4 + 3]);
		}
		for (int i = 16; i < 64; ++i)
		{
			const uint32_t S0 = Rotr(W[i - 15], 7) ^ Rotr(W[i - 15], 18) ^ (W[i - 15] >> 3);
			const uint32_t S1 = Rotr(W[i - 2], 17) ^ Rotr(W[i - 2], 19) ^ (W[i - 2] >> 10);
			W[i] = W[i - 16] + S0 + W[i - 7] + S1;
		}
		uint32_t A = State[0], B = State[1], C = State[2], D = State[3], E = State[4], F = State[5], G = State[6], H = State[7];
		for (int i = 0; i < 64; ++i)
		{
			const uint32_t S1 = Rotr(E, 6) ^ Rotr(E, 11) ^ Rotr(E, 25);
			const uint32_t Ch = (E & F) ^ (~E & G);
			const uint32_t T1 = H + S1 + Ch + K[i] + W[i];
			const uint32_t S0 = Rotr(A, 2) ^ Rotr(A, 13) ^ Rotr(A, 22);
			const uint32_t Maj = (A & B) ^ (A & C) ^ (B & C);
			const uint32_t T2 = S0 + Maj;
			H = G; G = F; F = E; E = D + T1; D = C; C = B; B = A; A = T1 + T2;
		}
		State[0] += A; State[1] += B; State[2] += C; State[3] += D;
		State[4] += E; State[5] += F; State[6] += G; State[7] += H;
	}

	void Sha256::Update(const void* Data, size_t Size)
	{
		const uint8_t* P = static_cast<const uint8_t*>(Data);
		TotalLen += Size;
		if (BufferLen)
		{
			const size_t Take = (Size < 64 - BufferLen) ? Size : 64 - BufferLen;
			std::memcpy(Buffer + BufferLen, P, Take);
			BufferLen += Take;
			P += Take;
			Size -= Take;
			if (BufferLen == 64)
			{
				Block(Buffer);
				BufferLen = 0;
			}
		}
		while (Size >= 64)
		{
			Block(P);
			P += 64;
			Size -= 64;
		}
		if (Size)
		{
			std::memcpy(Buffer, P, Size);
			BufferLen = Size;
		}
	}

	Sha256::Digest Sha256::Finish()
	{
		const uint64_t BitLen = TotalLen * 8;
		const uint8_t Pad = 0x80;
		Update(&Pad, 1);
		const uint8_t Zero = 0;
		while (BufferLen != 56)
		{
			Update(&Zero, 1);
		}
		uint8_t Len[8];
		for (int i = 0; i < 8; ++i)
		{
			Len[i] = static_cast<uint8_t>(BitLen >> (56 - 8 * i));
		}
		Update(Len, 8);
		Digest Out;
		for (int i = 0; i < 8; ++i)
		{
			Out[i * 4] = static_cast<uint8_t>(State[i] >> 24);
			Out[i * 4 + 1] = static_cast<uint8_t>(State[i] >> 16);
			Out[i * 4 + 2] = static_cast<uint8_t>(State[i] >> 8);
			Out[i * 4 + 3] = static_cast<uint8_t>(State[i]);
		}
		return Out;
	}

	Sha256::Digest Sha256::Hash(std::string_view Data)
	{
		Sha256 H;
		H.Update(Data);
		return H.Finish();
	}

	std::string Sha256::ToHex(const Digest& D)
	{
		static const char* Hex = "0123456789abcdef";
		std::string Out;
		Out.reserve(64);
		for (uint8_t B : D)
		{
			Out += Hex[B >> 4];
			Out += Hex[B & 15];
		}
		return Out;
	}

	bool Sha256::IsValidHex(std::string_view Hex)
	{
		if (Hex.size() != 64)
		{
			return false;
		}
		for (char C : Hex)
		{
			if (!((C >= '0' && C <= '9') || (C >= 'a' && C <= 'f')))
			{
				return false;
			}
		}
		return true;
	}
}
