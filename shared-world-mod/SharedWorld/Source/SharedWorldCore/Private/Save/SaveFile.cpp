#include "SharedWorldCore/Save/SaveFile.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>
#include <zlib.h>

#include "SharedWorldCore/Util/FileUtil.h"

namespace sw::save
{
	namespace
	{
		// FSaveHeader::Type values that change the layout.
		constexpr int32_t HdrUE425EngineUpdate = 7;
		constexpr int32_t HdrAddedModdingParams = 8;
		constexpr int32_t HdrAddedSaveIdentifier = 10;
		constexpr int32_t HdrAddedWorldPartitionSupport = 11;
		constexpr int32_t HdrAddedSaveModificationChecksum = 12;
		constexpr int32_t HdrAddedIsCreativeModeEnabled = 13;
		constexpr int32_t HdrAddedSaveName = 14;
		constexpr int32_t MinHeaderVersion = HdrUE425EngineUpdate;
		constexpr int32_t SaveVersionUE5 = 37; // 8-byte body prefix from here on

		constexpr uint32_t PackageFileTag = 0x9E2A83C1u;
		constexpr uint32_t ChunkHeaderV1 = 0x00000000u;
		constexpr uint32_t ChunkHeaderV2 = 0x22222222u;
		constexpr uint8_t CompressionZlib = 3;
		constexpr int64_t MaxChunkBytes = int64_t(256) << 20;

		Error Bad(const std::string& Why) { return MakeError(ErrorCode::Corrupt, Why); }

		/** Bounds-checked little-endian reader over the whole file. */
		struct Reader
		{
			const std::string& Data;
			size_t Pos = 0;

			bool Has(size_t N) const { return Pos + N <= Data.size(); }
			bool I32(int32_t& Out)
			{
				uint32_t U;
				if (!U32(U)) return false;
				Out = static_cast<int32_t>(U);
				return true;
			}
			bool U32(uint32_t& Out)
			{
				if (!Has(4)) return false;
				const unsigned char* P = reinterpret_cast<const unsigned char*>(Data.data() + Pos);
				Out = uint32_t(P[0]) | uint32_t(P[1]) << 8 | uint32_t(P[2]) << 16 | uint32_t(P[3]) << 24;
				Pos += 4;
				return true;
			}
			bool I64(int64_t& Out)
			{
				uint32_t Lo, Hi;
				if (!U32(Lo) || !U32(Hi)) return false;
				Out = static_cast<int64_t>(uint64_t(Hi) << 32 | Lo);
				return true;
			}
			bool Skip(size_t N)
			{
				if (!Has(N)) return false;
				Pos += N;
				return true;
			}
			/** Unreal FString: int32 length (negative = UTF-16), null-terminated. */
			bool FString(std::string& Out)
			{
				int32_t N;
				if (!I32(N)) return false;
				if (N == 0)
				{
					Out.clear();
					return true;
				}
				if (N > 0 && N <= (1 << 20))
				{
					if (!Has(static_cast<size_t>(N)) || Data[Pos + N - 1] != '\0') return false;
					Out.assign(Data, Pos, static_cast<size_t>(N - 1));
					Pos += static_cast<size_t>(N);
					return true;
				}
				if (N < 0 && N >= -(1 << 20))
				{
					const size_t Units = static_cast<size_t>(-static_cast<int64_t>(N));
					if (!Has(Units * 2)) return false;
					const unsigned char* P = reinterpret_cast<const unsigned char*>(Data.data() + Pos);
					if (P[Units * 2 - 2] != 0 || P[Units * 2 - 1] != 0) return false;
					// Decode UTF-16LE to UTF-8 (unpaired surrogates rejected).
					Out.clear();
					for (size_t i = 0; i + 1 < Units; ++i)
					{
						uint32_t Cp = uint32_t(P[i * 2]) | uint32_t(P[i * 2 + 1]) << 8;
						if (Cp >= 0xD800 && Cp <= 0xDBFF)
						{
							if (i + 2 >= Units) return false;
							const uint32_t Lo = uint32_t(P[i * 2 + 2]) | uint32_t(P[i * 2 + 3]) << 8;
							if (Lo < 0xDC00 || Lo > 0xDFFF) return false;
							Cp = 0x10000 + ((Cp - 0xD800) << 10) + (Lo - 0xDC00);
							++i;
						}
						else if (Cp >= 0xDC00 && Cp <= 0xDFFF)
						{
							return false;
						}
						if (Cp < 0x80) Out += static_cast<char>(Cp);
						else if (Cp < 0x800) { Out += static_cast<char>(0xC0 | (Cp >> 6)); Out += static_cast<char>(0x80 | (Cp & 0x3F)); }
						else if (Cp < 0x10000) { Out += static_cast<char>(0xE0 | (Cp >> 12)); Out += static_cast<char>(0x80 | ((Cp >> 6) & 0x3F)); Out += static_cast<char>(0x80 | (Cp & 0x3F)); }
						else { Out += static_cast<char>(0xF0 | (Cp >> 18)); Out += static_cast<char>(0x80 | ((Cp >> 12) & 0x3F)); Out += static_cast<char>(0x80 | ((Cp >> 6) & 0x3F)); Out += static_cast<char>(0x80 | (Cp & 0x3F)); }
					}
					Pos += Units * 2;
					return true;
				}
				return false;
			}
		};

		Result<SaveHeader> ReadHeader(Reader& R)
		{
			SaveHeader H;
			const Error E = Bad("not a valid Satisfactory save header");
			if (!R.I32(H.HeaderVersion) || !R.I32(H.SaveVersion) || !R.I32(H.BuildVersion)) return E;
			if (H.HeaderVersion < MinHeaderVersion || H.HeaderVersion > 64 || H.SaveVersion < 0 || H.BuildVersion < 0) return E;
			if (H.HeaderVersion >= HdrAddedSaveName && !R.FString(H.SaveName)) return E;
			if (!R.FString(H.MapName) || !R.FString(H.MapOptions) || !R.FString(H.SessionName)) return E;
			if (H.MapName.empty()) return E;
			if (!R.I32(H.PlayDurationSeconds) || !R.I64(H.SaveTicks)) return E;
			std::string Ignored;
			int32_t IgnoredInt;
			if (!R.Skip(1) || !R.I32(IgnoredInt)) return E; // session visibility, editor object version
			if (H.HeaderVersion >= HdrAddedModdingParams && (!R.FString(Ignored) || !R.I32(IgnoredInt))) return E;
			if (H.HeaderVersion >= HdrAddedSaveIdentifier && !R.FString(Ignored)) return E;
			if (H.HeaderVersion >= HdrAddedWorldPartitionSupport && !R.I32(IgnoredInt)) return E;
			if (H.HeaderVersion >= HdrAddedSaveModificationChecksum)
			{
				int32_t Valid;
				if (!R.I32(Valid)) return E;
				if (Valid == 1 && !R.Skip(16)) return E;
			}
			if (H.HeaderVersion >= HdrAddedIsCreativeModeEnabled && !R.I32(IgnoredInt)) return E;
			return H;
		}

		Status VerifyBody(Reader& R, int32_t SaveVersion)
		{
			int64_t Total = 0;
			std::string FirstBytes;
			std::string Out(1 << 16, '\0');
			for (int Chunk = 0;; ++Chunk)
			{
				if (R.Pos == R.Data.size())
				{
					if (Chunk == 0) return Bad("save body is empty");
					break;
				}
				const std::string Where = "chunk " + std::to_string(Chunk) + ": ";
				uint32_t Tag, Version;
				if (!R.U32(Tag) || !R.U32(Version)) return Bad(Where + "truncated chunk header");
				if (Tag != PackageFileTag) return Bad(Where + "bad package file tag");
				size_t HeaderLen;
				if (Version == ChunkHeaderV2) HeaderLen = 49;
				else if (Version == ChunkHeaderV1) HeaderLen = 48;
				else return Bad(Where + "unknown chunk header version");
				const size_t Base = R.Pos - 8;
				if (!R.Has(HeaderLen - 8)) return Bad(Where + "truncated chunk header");
				const size_t Off = HeaderLen == 49 ? 1 : 0;
				if (HeaderLen == 49 && static_cast<uint8_t>(R.Data[Base + 16]) != CompressionZlib) return Bad(Where + "unsupported compression");
				Reader Fields{R.Data, Base + 32 + Off};
				int64_t Compressed, Uncompressed;
				Fields.I64(Compressed);
				Fields.I64(Uncompressed);
				R.Pos = Base + HeaderLen;
				if (Compressed <= 0 || Compressed > MaxChunkBytes || Uncompressed < 0 || Uncompressed > MaxChunkBytes) return Bad(Where + "implausible chunk sizes");
				if (!R.Has(static_cast<size_t>(Compressed))) return Bad(Where + "truncated compressed data");

				z_stream Z;
				std::memset(&Z, 0, sizeof(Z));
				if (inflateInit(&Z) != Z_OK) return MakeError(ErrorCode::Io, "zlib init failed");
				Z.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(R.Data.data() + R.Pos));
				Z.avail_in = static_cast<uInt>(Compressed);
				int64_t Got = 0;
				int Rc = Z_OK;
				while (Rc == Z_OK)
				{
					Z.next_out = reinterpret_cast<Bytef*>(Out.data());
					Z.avail_out = static_cast<uInt>(Out.size());
					Rc = inflate(&Z, Z_NO_FLUSH);
					const size_t Produced = Out.size() - Z.avail_out;
					if (Chunk == 0 && FirstBytes.size() < 8) FirstBytes.append(Out.data(), (std::min)(Produced, size_t(8) - FirstBytes.size()));
					Got += static_cast<int64_t>(Produced);
					if (Got > Uncompressed) break;
					if (Rc == Z_BUF_ERROR && Z.avail_in == 0) break;
				}
				const uInt Left = Z.avail_in;
				inflateEnd(&Z);
				if (Rc != Z_STREAM_END) return Bad(Where + "corrupt compressed data");
				if (Left != 0) return Bad(Where + "compressed length mismatch");
				if (Got != Uncompressed) return Bad(Where + "inflated " + std::to_string(Got) + " bytes, header says " + std::to_string(Uncompressed));
				R.Pos += static_cast<size_t>(Compressed);
				Total += Got;
			}
			if (FirstBytes.size() < 4) return Bad("save body too short");
			const unsigned char* P = reinterpret_cast<const unsigned char*>(FirstBytes.data());
			const int64_t Declared = static_cast<int32_t>(uint32_t(P[0]) | uint32_t(P[1]) << 8 | uint32_t(P[2]) << 16 | uint32_t(P[3]) << 24);
			const int64_t Extra = SaveVersion >= SaveVersionUE5 ? 8 : 4;
			if (Declared + Extra != Total)
			{
				return Bad("save body declares " + std::to_string(Declared + Extra) + " bytes but contains " + std::to_string(Total));
			}
			return {};
		}
	}

	Status ValidateSaveName(const std::string& Name)
	{
		if (Name.empty() || Name.size() > 64) return MakeError(ErrorCode::Invalid, "invalid save name");
		for (char C : Name)
		{
			if (!((C >= 'a' && C <= 'z') || (C >= 'A' && C <= 'Z') || (C >= '0' && C <= '9') || C == '_' || C == '-'))
			{
				return MakeError(ErrorCode::Invalid, "invalid save name '" + Name + "'");
			}
		}
		return {};
	}

	Result<SaveHeader> ValidateBytes(const std::string& Bytes)
	{
		if (Bytes.size() < 64 || static_cast<int64_t>(Bytes.size()) > MaxSaveSize)
		{
			return Bad("save file has implausible size " + std::to_string(Bytes.size()));
		}
		Reader R{Bytes};
		SaveHeader H;
		SW_ASSIGN(H, ReadHeader(R));
		Status B = VerifyBody(R, H.SaveVersion);
		if (!B) return B.Err().Wrap("save body check failed");
		return H;
	}

	Result<SaveHeader> ValidateFile(const std::string& Path)
	{
		// Saves are tens of MB; reading once keeps the check simple and exact.
		std::string Bytes;
		SW_ASSIGN(Bytes, file::ReadAll(Path, MaxSaveSize));
		return ValidateBytes(Bytes);
	}

	Status WaitStable(const std::string& Path, TimeMs Quiet, TimeMs Timeout)
	{
		using Clock = std::chrono::steady_clock;
		const auto Deadline = Clock::now() + std::chrono::milliseconds(Timeout);
		auto Sample = [&](int64_t& Size, double& Age) -> Status
		{
			SW_ASSIGN(Size, file::Size(Path));
			SW_ASSIGN(Age, file::AgeSeconds(Path));
			return {};
		};
		int64_t LastSize;
		double LastAge;
		SW_TRY(Sample(LastSize, LastAge));
		auto StableSince = Clock::now();
		while (true)
		{
			std::this_thread::sleep_for(std::chrono::milliseconds((std::max<TimeMs>)(Quiet / 4, 5)));
			int64_t Size;
			double Age;
			SW_TRY(Sample(Size, Age));
			// A rewrite shows up as a size change or the age going backwards.
			if (Size != LastSize || Age + 0.001 < LastAge)
			{
				StableSince = Clock::now();
			}
			else if (Clock::now() - StableSince >= std::chrono::milliseconds(Quiet))
			{
				return {};
			}
			LastSize = Size;
			LastAge = Age;
			if (Clock::now() > Deadline) return MakeError(ErrorCode::BadState, "save file kept changing; the game may still be writing it");
		}
	}

	std::string BuildSynthetic(const std::string& SessionName, const std::string& Body)
	{
		std::string B;
		auto W32 = [&](uint32_t V) { for (int i = 0; i < 4; ++i) B += static_cast<char>((V >> (8 * i)) & 0xFF); };
		auto W64 = [&](uint64_t V) { for (int i = 0; i < 8; ++i) B += static_cast<char>((V >> (8 * i)) & 0xFF); };
		auto WStr = [&](const std::string& S)
		{
			if (S.empty()) { W32(0); return; }
			W32(static_cast<uint32_t>(S.size() + 1));
			B += S;
			B += '\0';
		};
		W32(HdrAddedSaveName);
		W32(52);
		W32(400000);
		WStr(SessionName);
		WStr("Persistent_Level");
		WStr("?startloc=Grass Fields");
		WStr(SessionName);
		W32(3600);
		W64(638000000000000000ull);
		B += '\0';
		W32(0);
		WStr("{\"Version\":1,\"FullMods\":[]}");
		W32(1);
		WStr("00000000000000000000000000000000");
		W32(1);
		W32(0);
		W32(0);
		std::string Raw;
		const uint32_t Len = static_cast<uint32_t>(Body.size());
		for (int i = 0; i < 4; ++i) Raw += static_cast<char>((Len >> (8 * i)) & 0xFF);
		Raw.append(4, '\0');
		Raw += Body;
		constexpr size_t ChunkSize = 1 << 17;
		for (size_t Off = 0; Off < Raw.size(); Off += ChunkSize)
		{
			const size_t N = (std::min)(ChunkSize, Raw.size() - Off);
			uLongf DestLen = compressBound(static_cast<uLong>(N));
			std::string Z(DestLen, '\0');
			compress(reinterpret_cast<Bytef*>(Z.data()), &DestLen, reinterpret_cast<const Bytef*>(Raw.data() + Off), static_cast<uLong>(N));
			Z.resize(DestLen);
			W32(PackageFileTag);
			W32(ChunkHeaderV2);
			W64(ChunkSize);
			B += static_cast<char>(CompressionZlib);
			for (int i = 0; i < 2; ++i)
			{
				W64(Z.size());
				W64(N);
			}
			B += Z;
		}
		return B;
	}
}
