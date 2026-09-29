#include "SharedWorldCore/Storage/Compress.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include <zstd.h>
#include <zlib.h>

#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Sha256.h"

namespace sw
{
	namespace
	{
		TimeMs ElapsedMs(std::chrono::steady_clock::time_point T0)
		{
			return static_cast<TimeMs>(
				std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - T0).count());
		}

	}

	const char* ToString(CompressionKind K)
	{
		switch (K)
		{
		case CompressionKind::None: return "none";
		case CompressionKind::Zstd: return "zstd";
		case CompressionKind::Zlib: return "zlib";
		}
		return "none";
	}

	Result<CompressionKind> ParseCompressionKind(const std::string& S)
	{
		if (S.empty() || S == "none" || S == "raw") return CompressionKind::None;
		if (S == "zstd") return CompressionKind::Zstd;
		if (S == "zlib" || S == "gzip" || S == "deflate") return CompressionKind::Zlib;
		return MakeError(ErrorCode::Invalid, "unknown compression: " + S);
	}

	Result<CompressStats> CompressFile(const std::string& SrcFile, const std::string& DestFile, const CompressOptions& Opt)
	{
		CompressStats Stats;
		Stats.Kind = Opt.Kind;
		Stats.Level = Opt.Level;
		auto H = file::Hash(SrcFile);
		if (!H) return H.Err();
		Stats.UncompressedSize = H->Size;
		Stats.UncompressedSha256 = H->Sha256;

		if (Opt.Kind == CompressionKind::None)
		{
			SW_TRY(file::CopyExclusive(SrcFile, DestFile));
			Stats.CompressedSize = Stats.UncompressedSize;
			Stats.CompressedSha256 = Stats.UncompressedSha256;
			return Stats;
		}

		const auto T0 = std::chrono::steady_clock::now();
		std::ifstream In(SrcFile, std::ios::binary);
		if (!In) return MakeError(ErrorCode::Io, "cannot open " + SrcFile);

		const std::string Tmp = file::TempSibling(DestFile, "cmp");
		std::ofstream Out(Tmp, std::ios::binary | std::ios::trunc);
		if (!Out) return MakeError(ErrorCode::Io, "cannot write " + Tmp);

		Sha256 PayloadHash;
		std::vector<char> InBuf(Opt.ChunkBytes ? Opt.ChunkBytes : size_t(1 << 20));
		int64_t Written = 0;

		if (Opt.Kind == CompressionKind::Zstd)
		{
			const int Level = std::clamp(Opt.Level, 1, 22);
			Stats.Level = Level;
			ZSTD_CCtx* Ctx = ZSTD_createCCtx();
			if (!Ctx) return MakeError(ErrorCode::Io, "zstd createCCtx failed");
			(void)ZSTD_CCtx_setParameter(Ctx, ZSTD_c_compressionLevel, Level);
			(void)ZSTD_CCtx_setParameter(Ctx, ZSTD_c_checksumFlag, 1);
			std::vector<char> OutBuf(ZSTD_CStreamOutSize());
			bool bFailed = false;
			std::string Err;
			for (;;)
			{
				In.read(InBuf.data(), static_cast<std::streamsize>(InBuf.size()));
				const size_t Got = static_cast<size_t>(In.gcount());
				const ZSTD_EndDirective End = Got ? ZSTD_e_continue : ZSTD_e_end;
				ZSTD_inBuffer Zin{InBuf.data(), Got, 0};
				bool bDone = false;
				while (!bDone)
				{
					ZSTD_outBuffer Zout{OutBuf.data(), OutBuf.size(), 0};
					const size_t Ret = ZSTD_compressStream2(Ctx, &Zout, &Zin, End);
					if (ZSTD_isError(Ret))
					{
						bFailed = true;
						Err = ZSTD_getErrorName(Ret);
						break;
					}
					if (Zout.pos)
					{
						Out.write(OutBuf.data(), static_cast<std::streamsize>(Zout.pos));
						PayloadHash.Update(OutBuf.data(), Zout.pos);
						Written += static_cast<int64_t>(Zout.pos);
					}
					bDone = (End == ZSTD_e_end) ? (Ret == 0) : (Zin.pos == Zin.size);
				}
				if (bFailed || !Got) break;
			}
			ZSTD_freeCCtx(Ctx);
			Out.close();
			if (bFailed || !Out)
			{
				(void)file::Remove(Tmp);
				return MakeError(ErrorCode::Io, "zstd compress failed: " + Err);
			}
		}
		else // zlib / deflate raw stream with zlib wrapper (compress2-style via deflate)
		{
			const int Level = std::clamp(Opt.Level, 1, 9);
			Stats.Level = Level;
			z_stream Zs{};
			if (deflateInit(&Zs, Level) != Z_OK) return MakeError(ErrorCode::Io, "deflateInit failed");
			std::vector<unsigned char> OutBuf(Opt.ChunkBytes ? Opt.ChunkBytes : size_t(1 << 20));
			int Ret = Z_OK;
			do
			{
				In.read(InBuf.data(), static_cast<std::streamsize>(InBuf.size()));
				const int Got = static_cast<int>(In.gcount());
				Zs.next_in = reinterpret_cast<Bytef*>(InBuf.data());
				Zs.avail_in = static_cast<uInt>(Got);
				const int Flush = Got ? Z_NO_FLUSH : Z_FINISH;
				do
				{
					Zs.next_out = OutBuf.data();
					Zs.avail_out = static_cast<uInt>(OutBuf.size());
					Ret = deflate(&Zs, Flush);
					if (Ret == Z_STREAM_ERROR)
					{
						deflateEnd(&Zs);
						(void)file::Remove(Tmp);
						return MakeError(ErrorCode::Io, "deflate stream error");
					}
					const size_t Have = OutBuf.size() - Zs.avail_out;
					if (Have)
					{
						Out.write(reinterpret_cast<char*>(OutBuf.data()), static_cast<std::streamsize>(Have));
						PayloadHash.Update(OutBuf.data(), Have);
						Written += static_cast<int64_t>(Have);
					}
				} while (Zs.avail_out == 0);
			} while (Ret != Z_STREAM_END);
			deflateEnd(&Zs);
			Out.close();
			if (!Out)
			{
				(void)file::Remove(Tmp);
				return MakeError(ErrorCode::Io, "zlib write failed");
			}
		}

		Stats.CompressMs = ElapsedMs(T0);
		Stats.CompressedSize = Written;
		Stats.CompressedSha256 = Sha256::ToHex(PayloadHash.Finish());

		const double Gain = Stats.UncompressedSize > 0
			? 1.0 - (static_cast<double>(Stats.CompressedSize) / static_cast<double>(Stats.UncompressedSize))
			: 0.0;
		if (Gain < Opt.MinRatioGain)
		{
			(void)file::Remove(Tmp);
			SW_TRY(file::CopyExclusive(SrcFile, DestFile));
			Stats.Kind = CompressionKind::None;
			Stats.CompressedSize = Stats.UncompressedSize;
			Stats.CompressedSha256 = Stats.UncompressedSha256;
			Stats.bSkippedPoorRatio = true;
			return Stats;
		}

		SW_TRY(file::ReplaceWith(Tmp, DestFile));
		return Stats;
	}

	Result<DecompressStats> DecompressFile(const std::string& SrcFile, const std::string& DestFile,
		CompressionKind Kind, int64_t ExpectedUncompressedSize)
	{
		DecompressStats Stats;
		Stats.Kind = Kind;
		auto Sz = file::Size(SrcFile);
		if (!Sz) return Sz.Err();
		Stats.CompressedSize = *Sz;

		if (Kind == CompressionKind::None)
		{
			SW_TRY(file::CopyExclusive(SrcFile, DestFile));
			Stats.UncompressedSize = Stats.CompressedSize;
			return Stats;
		}

		const auto T0 = std::chrono::steady_clock::now();
		std::ifstream In(SrcFile, std::ios::binary);
		if (!In) return MakeError(ErrorCode::Io, "cannot open " + SrcFile);
		const std::string Tmp = file::TempSibling(DestFile, "dec");
		std::ofstream Out(Tmp, std::ios::binary | std::ios::trunc);
		if (!Out) return MakeError(ErrorCode::Io, "cannot write " + Tmp);

		std::vector<char> InBuf(1 << 20);
		int64_t Written = 0;

		if (Kind == CompressionKind::Zstd)
		{
			ZSTD_DCtx* Ctx = ZSTD_createDCtx();
			if (!Ctx) return MakeError(ErrorCode::Io, "zstd createDCtx failed");
			std::vector<char> OutBuf(ZSTD_DStreamOutSize());
			bool bFailed = false;
			std::string Err;
			while (In)
			{
				In.read(InBuf.data(), static_cast<std::streamsize>(InBuf.size()));
				const size_t Got = static_cast<size_t>(In.gcount());
				if (!Got) break;
				ZSTD_inBuffer Zin{InBuf.data(), Got, 0};
				while (Zin.pos < Zin.size)
				{
					ZSTD_outBuffer Zout{OutBuf.data(), OutBuf.size(), 0};
					const size_t Ret = ZSTD_decompressStream(Ctx, &Zout, &Zin);
					if (ZSTD_isError(Ret))
					{
						bFailed = true;
						Err = ZSTD_getErrorName(Ret);
						break;
					}
					if (Zout.pos)
					{
						Out.write(OutBuf.data(), static_cast<std::streamsize>(Zout.pos));
						Written += static_cast<int64_t>(Zout.pos);
					}
				}
				if (bFailed) break;
			}
			ZSTD_freeDCtx(Ctx);
			Out.close();
			if (bFailed || !Out)
			{
				(void)file::Remove(Tmp);
				return MakeError(ErrorCode::Corrupt, "zstd decompress failed: " + Err);
			}
		}
		else
		{
			z_stream Zs{};
			if (inflateInit(&Zs) != Z_OK) return MakeError(ErrorCode::Io, "inflateInit failed");
			std::vector<unsigned char> OutBuf(1 << 20);
			int Ret = Z_OK;
			do
			{
				In.read(InBuf.data(), static_cast<std::streamsize>(InBuf.size()));
				const int Got = static_cast<int>(In.gcount());
				if (Got == 0 && Ret == Z_OK) break;
				Zs.next_in = reinterpret_cast<Bytef*>(InBuf.data());
				Zs.avail_in = static_cast<uInt>(Got);
				do
				{
					Zs.next_out = OutBuf.data();
					Zs.avail_out = static_cast<uInt>(OutBuf.size());
					Ret = inflate(&Zs, Z_NO_FLUSH);
					if (Ret == Z_NEED_DICT || Ret == Z_DATA_ERROR || Ret == Z_MEM_ERROR)
					{
						inflateEnd(&Zs);
						(void)file::Remove(Tmp);
						return MakeError(ErrorCode::Corrupt, "zlib decompress failed");
					}
					const size_t Have = OutBuf.size() - Zs.avail_out;
					if (Have)
					{
						Out.write(reinterpret_cast<char*>(OutBuf.data()), static_cast<std::streamsize>(Have));
						Written += static_cast<int64_t>(Have);
					}
				} while (Zs.avail_out == 0);
			} while (Ret != Z_STREAM_END);
			inflateEnd(&Zs);
			Out.close();
			if (Ret != Z_STREAM_END || !Out)
			{
				(void)file::Remove(Tmp);
				return MakeError(ErrorCode::Corrupt, "zlib decompress incomplete");
			}
		}

		Stats.DecompressMs = ElapsedMs(T0);
		Stats.UncompressedSize = Written;
		if (ExpectedUncompressedSize > 0 && Written != ExpectedUncompressedSize)
		{
			(void)file::Remove(Tmp);
			return MakeError(ErrorCode::Corrupt, "decompressed size mismatch");
		}
		SW_TRY(file::ReplaceWith(Tmp, DestFile));
		return Stats;
	}

	Result<CompressStats> BenchmarkCompress(const std::string& SrcFile, CompressionKind Kind, int Level)
	{
		CompressOptions Opt;
		Opt.Kind = Kind;
		Opt.Level = Level;
		Opt.MinRatioGain = -1.0; // never skip for benchmarks
		const std::string Tmp = file::TempSibling(SrcFile, "bench");
		auto R = CompressFile(SrcFile, Tmp, Opt);
		(void)file::Remove(Tmp);
		return R;
	}
}
