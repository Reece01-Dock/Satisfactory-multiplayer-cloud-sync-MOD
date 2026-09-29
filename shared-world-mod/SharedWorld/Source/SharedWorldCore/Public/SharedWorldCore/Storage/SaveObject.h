#pragma once
// On-wire / on-disk package for a Shared World save object.
// Logical object id = SHA-256 of the uncompressed .sav (RevisionMeta.object).
// Stored bytes may be a SWOB package (compressed) or a legacy raw .sav.

#include <cstdint>
#include <string>

#include "SharedWorldCore/Storage/Compress.h"
#include "SharedWorldCore/Util/Json.h"
#include "SharedWorldCore/Util/Result.h"

namespace sw
{
	/** Optional encoding metadata stored on RevisionMeta (backward compatible). */
	struct SaveObjectEncoding
	{
		CompressionKind Compression = CompressionKind::None;
		int CompressionVersion = 1;
		int64_t UncompressedSize = 0;
		int64_t CompressedSize = 0;
		std::string CompressedSha256; // optional; empty if unknown / raw
		/** Future object-store provider hint (empty = default / GitHub releases / folder). */
		std::string Provider;
		std::string Bucket;
		std::string Key;

		bool IsCompressed() const { return Compression != CompressionKind::None; }
		json::Value ToJson() const;
		static Result<SaveObjectEncoding> FromJson(const json::Value& V);
	};

	/**
	 * Build a SWOB package from a verified uncompressed .sav.
	 * If compression gain is below threshold, package still wraps raw bytes
	 * with Compression=None (explicit encoding, never inferred from extension).
	 */
	Result<CompressStats> PackSaveObject(const std::string& UncompressedSavPath, const std::string& PackageOutPath,
		const CompressOptions& Opt = {});

	/**
	 * Unpack SWOB or legacy raw .sav into DestSavPath.
	 * Verifies uncompressed SHA-256 when ExpectedSha256 is non-empty.
	 */
	Status UnpackSaveObject(const std::string& PackageOrRawPath, const std::string& DestSavPath,
		const std::string& ExpectedSha256, int64_t ExpectedSize, SaveObjectEncoding* OutEncoding = nullptr);

	bool IsSaveObjectPackage(const std::string& Path);
}
