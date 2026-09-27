#pragma once
// Satisfactory save files: structural validation, stable-write detection and
// a synthetic builder for tests. Port of shared-world-helper/internal/savefile.
//
// On-disk format (FSaveHeader history in FGSaveManagerInterface.h, cross-
// checked with @etothepii/satisfactory-file-parser 4.1.2): little-endian
// header, then zlib chunks, each starting with tag 0x9E2A83C1 and a v1/v2
// chunk header (48/49 bytes). The inflated body starts with its own size.
// A save truncated by a crash mid-write fails validation.

#include <cstdint>
#include <string>

#include "SharedWorldCore/Util/Result.h"
#include "SharedWorldCore/Util/Time.h"

namespace sw::save
{
	constexpr const char* Extension = ".sav";
	constexpr int64_t MaxSaveSize = int64_t(2) << 30;

	struct SaveHeader
	{
		int32_t HeaderVersion = 0;
		int32_t SaveVersion = 0;
		int32_t BuildVersion = 0;
		std::string SaveName;
		std::string MapName;
		std::string MapOptions;
		std::string SessionName;
		int32_t PlayDurationSeconds = 0;
		int64_t SaveTicks = 0;
	};

	/** Save stems: [A-Za-z0-9_-]{1,64}, so they can never traverse directories. */
	Status ValidateSaveName(const std::string& Name);

	/**
	 * Full structural check: header parses, every chunk inflates to its
	 * declared size, chunks end exactly at EOF, body size matches.
	 * Returns Corrupt for anything that is not a complete save.
	 */
	Result<SaveHeader> ValidateFile(const std::string& Path);
	/** Same check on bytes in memory. */
	Result<SaveHeader> ValidateBytes(const std::string& Bytes);

	/**
	 * Waits until size and modification time have not changed for Quiet,
	 * so a save the game is still writing is never picked up. The game's
	 * save-complete callback is the primary signal; this is the second guard.
	 */
	Status WaitStable(const std::string& Path, TimeMs Quiet, TimeMs Timeout);

	/** Structurally valid (not game-loadable) save with arbitrary body; tests only. */
	std::string BuildSynthetic(const std::string& SessionName, const std::string& Body);
}
