#pragma once
// Minimal test framework (no exceptions, no dependencies).

#include <cstdio>
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
}

#define SW_TEST(name)                                                         \
	static void name();                                                       \
	static swtest::Registrar name##_registrar(#name, name);                   \
	static void name()

#define EXPECT_TRUE(cond)                                                     \
	do { if (!(cond)) swtest::ReportFailure(__FILE__, __LINE__, "EXPECT_TRUE(" #cond ")"); } while (0)
#define ASSERT_TRUE(cond)                                                     \
	do { if (!(cond)) { swtest::ReportFailure(__FILE__, __LINE__, "ASSERT_TRUE(" #cond ")"); return; } } while (0)
#define EXPECT_EQ(a, b)                                                       \
	do { auto&& va_ = (a); auto&& vb_ = (b); if (!(va_ == vb_)) swtest::ReportFailure(__FILE__, __LINE__, std::string(#a " == " #b ": ") + swtest::Describe(va_, vb_)); } while (0)
#define ASSERT_EQ(a, b)                                                       \
	do { auto&& va_ = (a); auto&& vb_ = (b); if (!(va_ == vb_)) { swtest::ReportFailure(__FILE__, __LINE__, std::string(#a " == " #b ": ") + swtest::Describe(va_, vb_)); return; } } while (0)
// Asserts a Result/Status succeeded, printing its error otherwise.
#define ASSERT_OK(expr)                                                       \
	do { auto&& r_ = (expr); if (!r_.Ok()) { swtest::ReportFailure(__FILE__, __LINE__, std::string(#expr " failed: ") + r_.Err().Describe()); return; } } while (0)
#define EXPECT_ERR(expr, code)                                                \
	do { auto&& r_ = (expr); if (r_.Ok()) swtest::ReportFailure(__FILE__, __LINE__, std::string(#expr " unexpectedly succeeded")); \
		else if (r_.Err().Code != (code)) swtest::ReportFailure(__FILE__, __LINE__, std::string(#expr " wrong error: ") + r_.Err().Describe()); } while (0)
