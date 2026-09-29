#include "SharedWorldCore/Storage/SaveObject.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <vector>

#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Sha256.h"

namespace sw
{
	namespace
	{
		constexpr char Magic[4] = {'S', 'W', 'O', 'B'};
		constexpr uint16_t PackageVersion = 1;

		void WriteU16(std::ostream& Out, uint16_t V)
		{
			const uint8_t B[2] = {static_cast<uint8_t>(V & 0xff), static_cast<uint8_t>((V >> 8) & 0xff)};
			Out.write(reinterpret_cast<const char*>(B), 2);
		}
		void WriteU64(std::ostream& Out, uint64_t V)
		{
			uint8_t B[8];
			for (int i = 0; i < 8; ++i) B[i] = static_cast<uint8_t>((V >> (8 * i)) & 0xff);
			Out.write(reinterpret_cast<const char*>(B), 8);
		}
		bool ReadExact(std::istream& In, void* Buf, size_t N)
		{
			In.read(reinterpret_cast<char*>(Buf), static_cast<std::streamsize>(N));
			return static_cast<size_t>(In.gcount()) == N;
		}
		uint16_t ReadU16(std::istream& In, bool& Ok)
		{
			uint8_t B[2];
			Ok = ReadExact(In, B, 2);
			return Ok ? uint16_t(B[0] | (uint16_t(B[1]) << 8)) : 0;
		}
		uint64_t ReadU64(std::istream& In, bool& Ok)
		{
			uint8_t B[8];
			Ok = ReadExact(In, B, 8);
			uint64_t V = 0;
			if (Ok) for (int i = 0; i < 8; ++i) V |= uint64_t(B[i]) << (8 * i);
			return V;
		}
		Result<Sha256::Digest> ParseHexDigest(const std::string& Hex)
		{
			if (!Sha256::IsValidHex(Hex)) return MakeError(ErrorCode::Invalid, "invalid sha256 hex");
			Sha256::Digest D{};
			auto Nibble = [](char C) -> int {
				if (C >= '0' && C <= '9') return C - '0';
				if (C >= 'a' && C <= 'f') return C - 'a' + 10;
				return C - 'A' + 10;
			};
			for (size_t i = 0; i < 32; ++i)
			{
				D[i] = static_cast<uint8_t>((Nibble(Hex[i * 2]) << 4) | Nibble(Hex[i * 2 + 1]));
			}
			return D;
		}
	}

	json::Value SaveObjectEncoding::ToJson() const
	{
		json::Value V;
		V.Set("compression", ToString(Compression));
		V.Set("compressionVersion", CompressionVersion);
		V.Set("uncompressedSize", UncompressedSize);
		V.Set("compressedSize", CompressedSize);
		if (!CompressedSha256.empty()) V.Set("compressedSha256", CompressedSha256);
		if (!Provider.empty()) V.Set("provider", Provider);
		if (!Bucket.empty()) V.Set("bucket", Bucket);
		if (!Key.empty()) V.Set("key", Key);
		return V;
	}

	Result<SaveObjectEncoding> SaveObjectEncoding::FromJson(const json::Value& V)
	{
		SaveObjectEncoding E;
		if (!V.IsObject()) return MakeError(ErrorCode::Invalid, "saveObject must be an object");
		if (auto C = json::GetOptionalString(V, "compression", 16); C.Ok() && C->has_value())
		{
			SW_ASSIGN(E.Compression, ParseCompressionKind(**C));
		}
		if (auto Ver = json::GetOptionalInt(V, "compressionVersion"); Ver.Ok() && Ver->has_value()) E.CompressionVersion = static_cast<int>(**Ver);
		if (auto U = json::GetOptionalInt(V, "uncompressedSize"); U.Ok() && U->has_value()) E.UncompressedSize = **U;
		if (auto Csz = json::GetOptionalInt(V, "compressedSize"); Csz.Ok() && Csz->has_value()) E.CompressedSize = **Csz;
		if (auto H = json::GetOptionalString(V, "compressedSha256", 64); H.Ok() && H->has_value()) E.CompressedSha256 = **H;
		if (auto P = json::GetOptionalString(V, "provider", 64); P.Ok() && P->has_value()) E.Provider = **P;
		if (auto B = json::GetOptionalString(V, "bucket", 256); B.Ok() && B->has_value()) E.Bucket = **B;
		if (auto K = json::GetOptionalString(V, "key", 512); K.Ok() && K->has_value()) E.Key = **K;
		return E;
	}

	bool IsSaveObjectPackage(const std::string& Path)
	{
		std::ifstream In(Path, std::ios::binary);
		if (!In) return false;
		char M[4]{};
		if (!ReadExact(In, M, 4)) return false;
		return std::memcmp(M, Magic, 4) == 0;
	}

	Result<CompressStats> PackSaveObject(const std::string& UncompressedSavPath, const std::string& PackageOutPath,
		const CompressOptions& Opt)
	{
		const std::string PayloadTmp = file::TempSibling(PackageOutPath, "payload");
		struct Clean { std::string P; ~Clean() { (void)file::Remove(P); } } Guard{PayloadTmp};

		CompressOptions Use = Opt;
		auto Comp = CompressFile(UncompressedSavPath, PayloadTmp, Use);
		if (!Comp)
		{
			// Compression library failure → store raw package with compression=none.
			Use.Kind = CompressionKind::None;
			Use.MinRatioGain = -1.0;
			SW_ASSIGN(Comp, CompressFile(UncompressedSavPath, PayloadTmp, Use));
		}

		auto Digest = ParseHexDigest(Comp->UncompressedSha256);
		if (!Digest) return Digest.Err();
		Sha256::Digest CompDig{};
		if (Comp->Kind != CompressionKind::None)
		{
			SW_ASSIGN(CompDig, ParseHexDigest(Comp->CompressedSha256));
		}
		else
		{
			CompDig = *Digest;
		}

		const std::string PkgTmp = file::TempSibling(PackageOutPath, "swob");
		{
			std::ofstream Out(PkgTmp, std::ios::binary | std::ios::trunc);
			if (!Out) return MakeError(ErrorCode::Io, "cannot write package temp");
			Out.write(Magic, 4);
			WriteU16(Out, PackageVersion);
			WriteU16(Out, 0); // flags
			const uint8_t KindByte = static_cast<uint8_t>(Comp->Kind);
			const uint8_t LevelByte = static_cast<uint8_t>(std::clamp(Comp->Level, 0, 255));
			Out.put(static_cast<char>(KindByte));
			Out.put(static_cast<char>(LevelByte));
			WriteU16(Out, 0); // reserved
			WriteU64(Out, static_cast<uint64_t>(Comp->UncompressedSize));
			WriteU64(Out, static_cast<uint64_t>(Comp->CompressedSize));
			Out.write(reinterpret_cast<const char*>(Digest->data()), 32);
			Out.write(reinterpret_cast<const char*>(CompDig.data()), 32);

			std::ifstream Pay(PayloadTmp, std::ios::binary);
			if (!Pay) return MakeError(ErrorCode::Io, "missing payload temp");
			std::vector<char> Buf(1 << 20);
			while (Pay)
			{
				Pay.read(Buf.data(), static_cast<std::streamsize>(Buf.size()));
				const auto Got = Pay.gcount();
				if (Got > 0) Out.write(Buf.data(), Got);
			}
			if (!Out) return MakeError(ErrorCode::Io, "package write failed");
		}
		SW_TRY(file::ReplaceWith(PkgTmp, PackageOutPath));
		return *Comp;
	}

	Status UnpackSaveObject(const std::string& PackageOrRawPath, const std::string& DestSavPath,
		const std::string& ExpectedSha256, int64_t ExpectedSize, SaveObjectEncoding* OutEncoding)
	{
		if (!IsSaveObjectPackage(PackageOrRawPath))
		{
			// Legacy raw .sav object.
			SW_TRY(file::CopyExclusive(PackageOrRawPath, DestSavPath));
			auto H = file::Hash(DestSavPath);
			if (!H) return H.Err();
			if (!ExpectedSha256.empty() && H->Sha256 != ExpectedSha256)
				return MakeError(ErrorCode::Corrupt, "legacy object hash mismatch");
			if (ExpectedSize > 0 && H->Size != ExpectedSize)
				return MakeError(ErrorCode::Corrupt, "legacy object size mismatch");
			if (OutEncoding)
			{
				*OutEncoding = {};
				OutEncoding->Compression = CompressionKind::None;
				OutEncoding->UncompressedSize = H->Size;
				OutEncoding->CompressedSize = H->Size;
			}
			return {};
		}

		std::ifstream In(PackageOrRawPath, std::ios::binary);
		if (!In) return MakeError(ErrorCode::Io, "cannot open package");
		char M[4]{};
		if (!ReadExact(In, M, 4) || std::memcmp(M, Magic, 4) != 0)
			return MakeError(ErrorCode::Corrupt, "not a SWOB package");
		bool Ok = true;
		const uint16_t Ver = ReadU16(In, Ok);
		(void)ReadU16(In, Ok); // flags
		uint8_t KindByte = 0, LevelByte = 0;
		Ok = Ok && ReadExact(In, &KindByte, 1) && ReadExact(In, &LevelByte, 1);
		(void)ReadU16(In, Ok); // reserved
		const uint64_t UncSize = ReadU64(In, Ok);
		const uint64_t CompSize = ReadU64(In, Ok);
		Sha256::Digest UncDig{}, CompDig{};
		Ok = Ok && ReadExact(In, UncDig.data(), 32) && ReadExact(In, CompDig.data(), 32);
		if (!Ok || Ver != PackageVersion)
			return MakeError(ErrorCode::Corrupt, "invalid SWOB header");

		const CompressionKind Kind = static_cast<CompressionKind>(KindByte);
		const std::string PayloadTmp = file::TempSibling(DestSavPath, "pay");
		struct Clean { std::string P; ~Clean() { (void)file::Remove(P); } } Guard{PayloadTmp};
		{
			std::ofstream Pay(PayloadTmp, std::ios::binary | std::ios::trunc);
			if (!Pay) return MakeError(ErrorCode::Io, "cannot write payload temp");
			std::vector<char> Buf(1 << 20);
			uint64_t Left = CompSize;
			while (Left > 0)
			{
				const size_t N = static_cast<size_t>(std::min<uint64_t>(Left, Buf.size()));
				if (!ReadExact(In, Buf.data(), N)) return MakeError(ErrorCode::Corrupt, "truncated SWOB payload");
				Pay.write(Buf.data(), static_cast<std::streamsize>(N));
				Left -= N;
			}
		}

		// Optional compressed payload hash check.
		{
			auto PH = file::Hash(PayloadTmp);
			if (!PH) return PH.Err();
			if (PH->Sha256 != Sha256::ToHex(CompDig))
				return MakeError(ErrorCode::Corrupt, "compressed payload hash mismatch");
			if (PH->Size != static_cast<int64_t>(CompSize))
				return MakeError(ErrorCode::Corrupt, "compressed payload size mismatch");
		}

		const std::string SavTmp = file::TempSibling(DestSavPath, "sav");
		struct Clean2 { std::string P; ~Clean2() { if (!P.empty()) (void)file::Remove(P); } } Guard2{SavTmp};
		auto Dec = DecompressFile(PayloadTmp, SavTmp, Kind, static_cast<int64_t>(UncSize));
		if (!Dec) return Dec.Err();

		auto H = file::Hash(SavTmp);
		if (!H) return H.Err();
		const std::string ExpectHex = Sha256::ToHex(UncDig);
		if (H->Sha256 != ExpectHex)
			return MakeError(ErrorCode::Corrupt, "uncompressed save hash mismatch after decompress");
		if (!ExpectedSha256.empty() && H->Sha256 != ExpectedSha256)
			return MakeError(ErrorCode::Corrupt, "uncompressed save does not match revision object hash");
		if (ExpectedSize > 0 && H->Size != ExpectedSize)
			return MakeError(ErrorCode::Corrupt, "uncompressed save size mismatch");

		SW_TRY(file::ReplaceWith(SavTmp, DestSavPath));
		Guard2.P.clear(); // ownership moved via rename

		if (OutEncoding)
		{
			OutEncoding->Compression = Kind;
			OutEncoding->CompressionVersion = Ver;
			OutEncoding->UncompressedSize = static_cast<int64_t>(UncSize);
			OutEncoding->CompressedSize = static_cast<int64_t>(CompSize);
			OutEncoding->CompressedSha256 = Sha256::ToHex(CompDig);
		}
		return {};
	}
}
