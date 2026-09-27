#pragma once
// Portable file helpers. Paths are UTF-8 std::strings (wide APIs on Windows).
// Durable writes always flush through a *writable* handle: on Windows,
// FlushFileBuffers on a read-only handle fails with "Access is denied"
// (this broke the Go helper on Windows CI).

#include <cstdint>
#include <string>
#include <vector>

#include "SharedWorldCore/Util/Result.h"

namespace sw::file
{
	struct HashResult
	{
		std::string Sha256;
		int64_t Size = 0;
	};

	Result<std::string> ReadAll(const std::string& Path, int64_t MaxBytes = int64_t(64) << 20);
	/** Write to a temp file in the same directory, flush to disk, rename over Path. */
	Status WriteAtomic(const std::string& Path, const std::string& Data);
	/** Create Path with Data; AlreadyExists if it exists (never overwrites). */
	Status CreateExclusive(const std::string& Path, const std::string& Data);
	/** Copy Src to Dst (Dst must not exist), flushed to disk. */
	Status CopyExclusive(const std::string& Src, const std::string& Dst);
	/** Streams the file through SHA-256 without loading it into memory. */
	Result<HashResult> Hash(const std::string& Path, int64_t MaxBytes = int64_t(4) << 30);
	/** Renames a temp file over Target (atomic replace on NTFS and POSIX). */
	Status ReplaceWith(const std::string& Temp, const std::string& Target);
	/** Hard-links Existing to NewPath; AlreadyExists if NewPath exists (atomic create-only publish). */
	Status LinkExclusive(const std::string& Existing, const std::string& NewPath);
	/** fsync an existing file through a writable handle. */
	Status FlushExisting(const std::string& Path);

	bool Exists(const std::string& Path);
	Result<int64_t> Size(const std::string& Path);
	/** Seconds since last modification (for stale-lock detection). */
	Result<double> AgeSeconds(const std::string& Path);
	Status Remove(const std::string& Path); // missing file is not an error
	Status RemoveAll(const std::string& Path);
	Status CreateDirectories(const std::string& Path);
	Result<std::vector<std::string>> ListNames(const std::string& Dir); // missing dir -> empty
	std::string Join(const std::string& A, const std::string& B);
	std::string Parent(const std::string& Path);
	std::string FileName(const std::string& Path);
	/** Unique sibling temp path; the extension is not ".sav" so the game never lists it. */
	std::string TempSibling(const std::string& Path, const std::string& Tag);
}
