#include <atomic>
#include <chrono>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <random>
#include <string>

#include "TestFramework.h"

namespace swtest
{
	namespace
	{
		bool GFailed = false;
		std::vector<std::filesystem::path> GTempDirs;
		uint64_t GChaosSeed = 1;
		SuiteCounters GCounters;
	}

	std::vector<TestCase>& Registry()
	{
		static std::vector<TestCase> R;
		return R;
	}

	void ReportFailure(const char* File, int Line, const std::string& Message)
	{
		GFailed = true;
		std::fprintf(stderr, "    %s:%d: %s\n", File, Line, Message.c_str());
	}

	bool CurrentFailed() { return GFailed; }

	uint64_t ChaosSeed() { return GChaosSeed; }
	void SetChaosSeed(uint64_t Seed) { GChaosSeed = Seed == 0 ? 1 : Seed; }
	SuiteCounters& Counters() { return GCounters; }

	void Record(const char* Bucket)
	{
		if (!Bucket) return;
		if (std::strcmp(Bucket, "election") == 0) ++GCounters.HostElections;
		else if (std::strcmp(Bucket, "acquire") == 0) ++GCounters.SimultaneousAcquisitions;
		else if (std::strcmp(Bucket, "crash") == 0) ++GCounters.HostCrashes;
		else if (std::strcmp(Bucket, "migration") == 0) ++GCounters.Migrations;
		else if (std::strcmp(Bucket, "storage") == 0) ++GCounters.StorageFailures;
		else if (std::strcmp(Bucket, "git") == 0) ++GCounters.GitConflicts;
		else if (std::strcmp(Bucket, "invariant") == 0) ++GCounters.InvariantChecks;
		else if (std::strcmp(Bucket, "bench") == 0) ++GCounters.Benchmarks;
	}

	std::string TempDir()
	{
		static std::atomic<int> Counter{0};
		std::random_device Rd;
		auto P = std::filesystem::temp_directory_path() / ("swtest-" + std::to_string(Rd()) + "-" + std::to_string(Counter++));
		std::error_code Ec;
		std::filesystem::create_directories(P, Ec);
		GTempDirs.push_back(P);
		return P.string();
	}
}

static void PrintUsage()
{
	std::printf("Usage: sw_tests [filter] [--seed N] [--list]\n");
	std::printf("  filter   substring match on test name (e.g. Migration, Scenario, Chaos)\n");
	std::printf("  --seed N chaos/fuzz seed (also SW_TEST_SEED). Default 1.\n");
	std::printf("  --list   print test names and exit\n");
}

int main(int argc, char** argv)
{
	const char* Filter = nullptr;
	bool bList = false;
	if (const char* Env = std::getenv("SW_TEST_SEED"))
	{
		swtest::SetChaosSeed(static_cast<uint64_t>(std::strtoull(Env, nullptr, 10)));
	}
	for (int i = 1; i < argc; ++i)
	{
		const char* A = argv[i];
		if (std::strcmp(A, "--list") == 0) bList = true;
		else if (std::strcmp(A, "--help") == 0 || std::strcmp(A, "-h") == 0)
		{
			PrintUsage();
			return 0;
		}
		else if (std::strncmp(A, "--seed=", 7) == 0)
		{
			swtest::SetChaosSeed(static_cast<uint64_t>(std::strtoull(A + 7, nullptr, 10)));
		}
		else if (std::strcmp(A, "--seed") == 0 && i + 1 < argc)
		{
			swtest::SetChaosSeed(static_cast<uint64_t>(std::strtoull(argv[++i], nullptr, 10)));
		}
		else if (A[0] != '-')
		{
			Filter = A;
		}
		else
		{
			std::fprintf(stderr, "unknown flag: %s\n", A);
			PrintUsage();
			return 2;
		}
	}

	if (bList)
	{
		for (auto& T : swtest::Registry()) std::printf("%s\n", T.Name);
		return 0;
	}

	std::printf("SharedWorldCore tests  seed=%llu\n", static_cast<unsigned long long>(swtest::ChaosSeed()));

	int Run = 0, Failed = 0;
	std::map<std::string, int> CatRun, CatFail;
	for (auto& T : swtest::Registry())
	{
		if (Filter && !std::strstr(T.Name, Filter)) continue;
		++Run;
		const char* Cat = swtest::CategoryOf(T.Name);
		++CatRun[Cat];
		swtest::GFailed = false;
		const auto Start = std::chrono::steady_clock::now();
		T.Fn();
		const auto Ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Start).count();
		if (swtest::GFailed)
		{
			++Failed;
			++CatFail[Cat];
			std::fprintf(stderr, "FAIL %s (%lld ms)\n", T.Name, static_cast<long long>(Ms));
		}
		else
		{
			std::printf("ok   %s (%lld ms)\n", T.Name, static_cast<long long>(Ms));
		}
	}
	for (auto& P : swtest::GTempDirs)
	{
		std::error_code Ec;
		std::filesystem::remove_all(P, Ec);
	}

	const swtest::SuiteCounters& C = swtest::Counters();
	std::printf("\n=== Suite summary ===\n");
	std::printf("Host elections recorded:          %d\n", C.HostElections);
	std::printf("Simultaneous acquisitions:        %d\n", C.SimultaneousAcquisitions);
	std::printf("Host crashes exercised:           %d\n", C.HostCrashes);
	std::printf("Migrations exercised:             %d\n", C.Migrations);
	std::printf("Storage failure paths:            %d\n", C.StorageFailures);
	std::printf("Git/CAS conflict paths:           %d\n", C.GitConflicts);
	std::printf("Invariant checks:                 %d\n", C.InvariantChecks);
	std::printf("Benchmarks:                       %d\n", C.Benchmarks);
	std::printf("Chaos seed:                       %llu\n", static_cast<unsigned long long>(swtest::ChaosSeed()));
	std::printf("\nBy category:\n");
	for (const auto& [Cat, N] : CatRun)
	{
		std::printf("  %-12s %d run, %d failed\n", Cat.c_str(), N, CatFail[Cat]);
	}
	std::printf("\n%d tests, %d failed\n", Run, Failed);
	if (Failed)
	{
		std::printf("Re-run failing chaos with: sw_tests Chaos --seed %llu\n", static_cast<unsigned long long>(swtest::ChaosSeed()));
	}
	return Failed ? 1 : 0;
}
