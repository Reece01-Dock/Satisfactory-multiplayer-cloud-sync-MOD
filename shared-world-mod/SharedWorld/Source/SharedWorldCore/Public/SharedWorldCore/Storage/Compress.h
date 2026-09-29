#pragma once
// Streaming save compression for Shared World object storage.
// Authoritative content hash is always of the uncompressed .sav bytes.

#include <cstdint>
#include <string>

#include "SharedWorldCore/Util/Result.h"
#include "SharedWorldCore/Util/Time.h"

namespace sw
{
	enum class CompressionKind : uint8_t
	{
		None = 0,
		Zstd = 1,
		Zlib = 2,
	};
	const char* ToString(CompressionKind K);
	Result<CompressionKind> ParseCompressionKind(const std::string& S);

	struct CompressOptions
	{
		CompressionKind Kind = CompressionKind::Zstd;
		/** Zstd: 1..22 (default 3). Zlib: 1..9 (mapped). */
		int Level = 3;
		/**
		 * If compressed size is not at least this fraction smaller than raw
		 * (e.g. 0.05 = 5%), store uncompressed instead.
		 */
		double MinRatioGain = 0.05;
		size_t ChunkBytes = 1 << 20; // 1 MiB streaming buffer
	};

	struct CompressStats
	{
		CompressionKind Kind = CompressionKind::None;
		int Level = 0;
		int64_t UncompressedSize = 0;
		int64_t CompressedSize = 0;
		std::string UncompressedSha256;
		std::string CompressedSha256; // hash of compressed payload bytes only
		TimeMs CompressMs = 0;
		bool bSkippedPoorRatio = false;
	};

	struct DecompressStats
	{
		CompressionKind Kind = CompressionKind::None;
		int64_t UncompressedSize = 0;
		int64_t CompressedSize = 0;
		TimeMs DecompressMs = 0;
	};

	/** Stream-compress SrcFile → DestFile (Dest overwritten). */
	Result<CompressStats> CompressFile(const std::string& SrcFile, const std::string& DestFile, const CompressOptions& Opt = {});
	/** Stream-decompress SrcFile → DestFile. */
	Result<DecompressStats> DecompressFile(const std::string& SrcFile, const std::string& DestFile,
		CompressionKind Kind, int64_t ExpectedUncompressedSize = 0);

	/** Benchmark helpers (tests / tools). */
	Result<CompressStats> BenchmarkCompress(const std::string& SrcFile, CompressionKind Kind, int Level);
}
