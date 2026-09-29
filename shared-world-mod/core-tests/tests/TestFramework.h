#pragma once
// Minimal test framework (no exceptions, no dependencies).

#include <cstdio>
#include <cstring>
#include <functional>
#include <sstream>
#include <string>
#include <vector>

namespace swtest
{
	struct TestCase
	{
		const char* Name;
		std::function<void()> Fn;
	};

	std::vector<TestCase>& Registry();
	void ReportFailure(const char* File, int Line, const std::string& Message);
	bool CurrentFailed();
	std::string TempDir(); // unique, created, removed at exit

	/** Seed from --seed / SW_TEST_SEED; default 1. Chaos tests must honour this. */
	uint64_t ChaosSeed();
	void SetChaosSeed(uint64_t Seed);

	/** Tallies for the end-of-suite report (tests call RecordPass on success paths). */
	struct SuiteCounters
	{
		int HostElections = 0;
		int SimultaneousAcquisitions = 0;
		int HostCrashes = 0;
		int Migrations = 0;
		int StorageFailures = 0;
		int GitConflicts = 0;
		int InvariantChecks = 0;
		int Benchmarks = 0;
	};
	SuiteCounters& Counters();
	void Record(const char* Bucket); // "election"|"acquire"|"crash"|"migration"|"storage"|"git"|"invariant"|"bench"

	struct Registrar
	{
		Registrar(const char* Name, std::function<void()> Fn) { Registry().push_back({Name, std::move(Fn)}); }
	};

	template <typename A, typename B>
	std::string Describe(const A& a, const B& b)
	{
		std::ostringstream S;
		S << "expected [" << b << "] got [" << a << "]";
		return S.str();
	}

	inline const char* CategoryOf(const char* Name)
	{
		if (std::strstr(Name, "Chaos")) return "chaos";
		if (std::strstr(Name, "Benchmark") || std::strstr(Name, "Perf_")) return "benchmark";
		if (std::strstr(Name, "HostElection") || std::strstr(Name, "Scenario_C")) return "election";
		if (std::strstr(Name, "Migration") || std::strstr(Name, "Scenario_D") || std::strstr(Name, "Scenario_E") || std::strstr(Name, "Scenario_F")) return "migration";
		if (std::strstr(Name, "Lease") || std::strstr(Name, "Scenario_B") || std::strstr(Name, "Scenario_A")) return "lease";
		if (std::strstr(Name, "Sync") || std::strstr(Name, "Scenario_H") || std::strstr(Name, "Scenario_J") || std::strstr(Name, "Scenario_K")) return "sync";
		if (std::strstr(Name, "GitHub") || std::strstr(Name, "Scenario_I")) return "git";
		if (std::strstr(Name, "World") || std::strstr(Name, "Scenario_")) return "session";
		return "other";
	}
}

#define SW_TEST(name)                                                         \
	static void name();                                                       \
	static swtest::Registrar name##_registrar(#name, name);                   \
	static void name()

#define EXPECT_TRUE(cond)                                                     \
	do { if (!(cond)) swtest::ReportFailure(__FILE__, __LINE__, "EXPECT_TRUE(" #cond ")"); } while (0)
#define ASSERT_TRUE(cond)                                                     \
	do { if (!(cond)) { swtest::ReportFailure(__FILE__, __LINE__, "ASSERT_TRUE(" #cond ")"); return; } } while (0)
// Operands are copied: binding references could dangle into temporaries
// (e.g. View().Error->Code).
#define EXPECT_EQ(a, b)                                                       \
	do { auto va_ = (a); auto vb_ = (b); if (!(va_ == vb_)) swtest::ReportFailure(__FILE__, __LINE__, std::string(#a " == " #b ": ") + swtest::Describe(va_, vb_)); } while (0)
#define ASSERT_EQ(a, b)                                                       \
	do { auto va_ = (a); auto vb_ = (b); if (!(va_ == vb_)) { swtest::ReportFailure(__FILE__, __LINE__, std::string(#a " == " #b ": ") + swtest::Describe(va_, vb_)); return; } } while (0)
// Asserts a Result/Status succeeded, printing its error otherwise.
#define ASSERT_OK(expr)                                                       \
	do { auto&& r_ = (expr); if (!r_.Ok()) { swtest::ReportFailure(__FILE__, __LINE__, std::string(#expr " failed: ") + r_.Err().Describe()); return; } } while (0)
#define EXPECT_ERR(expr, code)                                                \
	do { auto&& r_ = (expr); if (r_.Ok()) swtest::ReportFailure(__FILE__, __LINE__, std::string(#expr " unexpectedly succeeded")); \
		else if (r_.Err().Code != (code)) swtest::ReportFailure(__FILE__, __LINE__, std::string(#expr " wrong error: ") + r_.Err().Describe()); } while (0)
