// Election performance: peer-matrix scoring must stay cheap at 2–50 players.

#include <chrono>
#include <cstdio>
#include <string>
#include <vector>

#include "SharedWorldCore/HostElection/HostElection.h"
#include "SharedWorldCore/NetworkQuality/NetworkQuality.h"
#include "TestFramework.h"

using namespace sw;
using namespace swtest;

namespace
{
	void BenchN(int N)
	{
		SimulatedNetworkQualityProvider Net;
		std::vector<std::string> Ids;
		Ids.reserve(static_cast<size_t>(N));
		for (int i = 0; i < N; ++i)
		{
			Ids.push_back("p" + std::to_string(i));
			Net.SetUploadKbps(Ids.back(), 5000 + (i % 7) * 100);
		}
		for (int i = 0; i < N; ++i)
		{
			for (int j = 0; j < N; ++j)
			{
				if (i == j) continue;
				LinkSample S;
				S.RttMs = 10 + ((i * 17 + j * 13) % 90);
				S.JitterMs = (i + j) % 11;
				S.LossBp = (i == j + 3) ? 50 : 0;
				S.bReachable = true;
				S.UploadKbps = 8000;
				Net.SetLink(Ids[static_cast<size_t>(i)], Ids[static_cast<size_t>(j)], S);
			}
		}
		PeerQualityMatrix M;
		M.SetPlayers(Ids);
		Net.ApplyTo(M, 1);
		std::vector<HostCandidate> Cs;
		Cs.reserve(static_cast<size_t>(N));
		for (int i = 0; i < N; ++i)
		{
			Cs.push_back(MakeCandidate(Identity{Ids[static_cast<size_t>(i)], Ids[static_cast<size_t>(i)], "EOS", "i" + std::to_string(i)},
				M, Ids, true, true, true, true));
		}
		Identity Host = Cs.front().Who;
		const auto T0 = std::chrono::steady_clock::now();
		constexpr int Iters = 200;
		std::vector<RankedHost> Last;
		for (int k = 0; k < Iters; ++k)
		{
			Last = RankHosts(Cs, Host, {}, false);
		}
		const auto Ms = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - T0).count();
		const double Per = static_cast<double>(Ms) / Iters;
		std::printf("    election N=%d: %.1f us/rank  ranked=%zu  measurements=%d\n", N, Per, Last.size(), N * (N - 1));
		ASSERT_TRUE(!Last.empty());
		// Determinism: second pass identical order.
		auto Again = RankHosts(Cs, Host, {}, false);
		ASSERT_EQ(Again.size(), Last.size());
		for (size_t i = 0; i < Last.size(); ++i)
		{
			EXPECT_EQ(Again[i].Who.PlayerId, Last[i].Who.PlayerId);
			EXPECT_EQ(Again[i].Score.Total, Last[i].Score.Total);
		}
		// Soft budget: even 50 players should rank well under 5ms on CI hardware.
		EXPECT_TRUE(Per < 5000.0);
		swtest::Record("bench");
		swtest::Record("election");
	}
}

SW_TEST(Benchmark_Election_4)
{
	BenchN(4);
}
SW_TEST(Benchmark_Election_8)
{
	BenchN(8);
}
SW_TEST(Benchmark_Election_16)
{
	BenchN(16);
}
SW_TEST(Benchmark_Election_32)
{
	BenchN(32);
}
SW_TEST(Benchmark_Election_50)
{
	BenchN(50);
}
