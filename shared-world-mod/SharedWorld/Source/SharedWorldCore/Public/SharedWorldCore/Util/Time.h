#pragma once
// Wall-clock time as Unix milliseconds (UTC). Injectable for tests.

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

#include "SharedWorldCore/Util/Result.h"

namespace sw
{
	using TimeMs = int64_t;

	constexpr TimeMs Seconds(int64_t S) { return S * 1000; }
	constexpr TimeMs Minutes(int64_t M) { return M * 60 * 1000; }

	class IClock
	{
	public:
		virtual ~IClock() = default;
		virtual TimeMs Now() const = 0;
	};

	class SystemClock final : public IClock
	{
	public:
		TimeMs Now() const override;
	};

	class FakeClock final : public IClock
	{
	public:
		explicit FakeClock(TimeMs Start) : Current(Start) {}
		TimeMs Now() const override { return Current.load(); }
		void Advance(TimeMs Delta) { Current += Delta; }

	private:
		std::atomic<TimeMs> Current;
	};

	/** "2026-09-27T12:00:00.000Z" */
	std::string FormatTime(TimeMs T);
	/** Accepts RFC 3339 with 'Z' or a numeric offset and optional fraction. */
	Result<TimeMs> ParseTime(std::string_view S);
}
