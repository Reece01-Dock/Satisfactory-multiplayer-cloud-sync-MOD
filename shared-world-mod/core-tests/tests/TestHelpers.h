#pragma once
// Shared fixtures for core tests.

#include <memory>
#include <string>

#include "SharedWorldCore/Lease/Lease.h"
#include "SharedWorldCore/Util/Log.h"

namespace swtest
{
	inline constexpr sw::TimeMs StartTime = 1790510400000; // 2026-09-27T12:00:00Z
	inline constexpr const char* TestWorldId = "our-factory";

	inline sw::Identity Player(int N)
	{
		return sw::Identity{"player-" + std::to_string(N), "P" + std::to_string(N), "EOS", "install-" + std::to_string(N)};
	}

	inline sw::NewWorld TestWorld(const std::string& Id = TestWorldId)
	{
		sw::NewWorld W;
		W.Info.WorldId = Id;
		W.Info.Name = "Our Factory";
		W.Info.CreatedBy = Player(1);
		W.Info.CreatedAt = StartTime;
		// Open membership: any player with storage access may play (membership has its own tests).
		W.Settings.Name = "Our Factory";
		return W;
	}

	inline sw::Status CreateTestWorld(std::shared_ptr<sw::IWorldRepository> Repo, std::shared_ptr<sw::IClock> Clock)
	{
		sw::WorldStore Store(std::move(Repo), TestWorldId, std::move(Clock), sw::Logger());
		auto R = Store.Create(TestWorld());
		if (!R) return R.Err();
		return {};
	}

	inline std::shared_ptr<sw::LeaseManager> MakeLeases(std::shared_ptr<sw::IWorldRepository> Repo, std::shared_ptr<sw::IClock> Clock,
		std::shared_ptr<sw::ILogSink> Sink = nullptr)
	{
		auto Store = std::make_shared<sw::WorldStore>(std::move(Repo), TestWorldId, std::move(Clock), sw::Logger(std::move(Sink)), 200);
		return std::make_shared<sw::LeaseManager>(Store, sw::LeaseConfig{});
	}
}
