#include <atomic>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <random>

#include "TestFramework.h"

namespace swtest
{
	namespace
	{
		bool GFailed = false;
		std::vector<std::filesystem::path> GTempDirs;
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

int main(int argc, char** argv)
{
	const char* Filter = argc > 1 ? argv[1] : nullptr;
	int Run = 0, Failed = 0;
	for (auto& T : swtest::Registry())
	{
		if (Filter && !std::strstr(T.Name, Filter))
		{
			continue;
		}
		++Run;
		swtest::GFailed = false;
		const auto Start = std::chrono::steady_clock::now();
		T.Fn();
		const auto Ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - Start).count();
		if (swtest::GFailed)
		{
			++Failed;
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
	std::printf("%d tests, %d failed\n", Run, Failed);
	return Failed ? 1 : 0;
}
