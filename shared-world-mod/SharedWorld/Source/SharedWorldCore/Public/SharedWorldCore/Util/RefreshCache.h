#pragma once
// A value that one thread reads freely while another refills it.
//
// Used so latency-sensitive callers (the game thread, once a second) never do
// network I/O: they read Get(), and a background job started when
// TryBeginRefresh() returns true calls Set() then EndRefresh().
// Time is passed in (seconds, any monotonic base) so tests are deterministic.

#include <mutex>
#include <optional>
#include <utility>

namespace sw
{
	template <typename T>
	class RefreshCache
	{
	public:
		std::optional<T> Get() const
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			return Value;
		}
		void Set(std::optional<T> NewValue)
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			Value = std::move(NewValue);
		}
		/** True if the caller should start a refresh now: none in flight and MinInterval elapsed since the last start. */
		bool TryBeginRefresh(double NowSeconds, double MinIntervalSeconds)
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			if (bInFlight || (bStartedOnce && NowSeconds - LastStart < MinIntervalSeconds))
			{
				return false;
			}
			bInFlight = true;
			bStartedOnce = true;
			LastStart = NowSeconds;
			return true;
		}
		void EndRefresh()
		{
			std::lock_guard<std::mutex> Lock(Mutex);
			bInFlight = false;
		}

	private:
		mutable std::mutex Mutex;
		std::optional<T> Value;
		bool bInFlight = false;
		bool bStartedOnce = false;
		double LastStart = 0.0;
	};
}
