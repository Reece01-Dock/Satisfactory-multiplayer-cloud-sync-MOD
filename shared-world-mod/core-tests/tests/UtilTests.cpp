#include "SharedWorldCore/Util/RefreshCache.h"
#include "SharedWorldCore/Util/Json.h"
#include "SharedWorldCore/Util/Log.h"
#include "SharedWorldCore/Util/Random.h"
#include "SharedWorldCore/Util/Sha256.h"
#include "SharedWorldCore/Util/Time.h"
#include "TestFramework.h"

using namespace sw;

SW_TEST(Sha256_KnownVectors)
{
	// FIPS 180-4 / NIST examples.
	EXPECT_EQ(Sha256::HexOf(""), std::string("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"));
	EXPECT_EQ(Sha256::HexOf("abc"), std::string("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
	EXPECT_EQ(Sha256::HexOf("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
		std::string("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"));
	Sha256 H;
	const std::string A(1000, 'a');
	for (int i = 0; i < 1000; ++i) H.Update(A); // one million 'a'
	EXPECT_EQ(Sha256::ToHex(H.Finish()), std::string("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"));
}

SW_TEST(Sha256_StreamingMatchesOneShot)
{
	std::string Data;
	for (int i = 0; i < 5000; ++i) Data += static_cast<char>(i * 7 + 3);
	for (size_t Step : {1u, 3u, 63u, 64u, 65u, 1000u})
	{
		Sha256 H;
		for (size_t Pos = 0; Pos < Data.size(); Pos += Step) H.Update(std::string_view(Data).substr(Pos, Step));
		EXPECT_EQ(Sha256::ToHex(H.Finish()), Sha256::HexOf(Data));
	}
	EXPECT_TRUE(Sha256::IsValidHex(Sha256::HexOf("x")));
	EXPECT_TRUE(!Sha256::IsValidHex("ABCD"));
	EXPECT_TRUE(!Sha256::IsValidHex(std::string(64, 'G')));
}

SW_TEST(Json_RoundTripAndExactIntegers)
{
	auto V = json::Parse(R"({"b":[1,2.5,true,null,"x\u00e9\ud83d\ude00"],"a":9007199254740993,"neg":-42,"s":"q\"\\\n"})");
	ASSERT_OK(V);
	EXPECT_EQ(V->Find("a")->AsInt(), int64_t(9007199254740993)); // not representable as double
	EXPECT_EQ(V->Find("neg")->AsInt(), int64_t(-42));
	EXPECT_EQ(V->Find("b")->AsArray()[4].AsString(), std::string("x\xC3\xA9\xF0\x9F\x98\x80"));
	const std::string Out = json::Serialize(*V);
	EXPECT_EQ(Out, std::string(R"({"a":9007199254740993,"b":[1,2.5,true,null,"xé😀"],"neg":-42,"s":"q\"\\\n"})"));
	auto Again = json::Parse(Out);
	ASSERT_OK(Again);
	EXPECT_TRUE(*Again == *V);
	auto Pretty = json::Parse(json::Serialize(*V, 2));
	ASSERT_OK(Pretty);
	EXPECT_TRUE(*Pretty == *V);
}

SW_TEST(Json_RejectsMalformedAndHostileInput)
{
	for (const char* Bad : {"", "{", "[1,]", "{\"a\":1,}", "{\"a\" 1}", "01", "1.", ".5", "-", "tru", "\"\\x\"",
			 "\"\x01\"", "{\"a\":1,\"a\":2}", "[1] 2", "\"\\ud800\"", "\"\xC0\x80\"", "\"\xED\xA0\x80\"", "1e", "NaN", "{a:1}"})
	{
		auto R = json::Parse(Bad);
		if (R.Ok()) swtest::ReportFailure(__FILE__, __LINE__, std::string("accepted: ") + Bad);
	}
	std::string Deep(1000, '[');
	EXPECT_ERR(json::Parse(Deep), ErrorCode::Invalid);
	json::ParseLimits Small;
	Small.MaxBytes = 10;
	EXPECT_ERR(json::Parse("[1,2,3,4,5,6,7]", Small), ErrorCode::Invalid);
}

SW_TEST(Json_CheckedAccessors)
{
	auto V = json::Parse(R"({"s":"x","i":5,"b":true,"n":null,"d":1.5})");
	ASSERT_OK(V);
	EXPECT_EQ(json::GetString(*V, "s").Value(), std::string("x"));
	EXPECT_ERR(json::GetString(*V, "i"), ErrorCode::Invalid);
	EXPECT_ERR(json::GetInt(*V, "d"), ErrorCode::Invalid); // fractions are not integers
	EXPECT_EQ(json::GetInt(*V, "i").Value(), int64_t(5));
	EXPECT_TRUE(!json::GetOptionalString(*V, "n").Value().has_value());
	EXPECT_TRUE(!json::GetOptionalString(*V, "missing").Value().has_value());
	EXPECT_ERR(json::GetOptionalString(*V, "i"), ErrorCode::Invalid);
	EXPECT_ERR(json::GetString(*V, "s", 0), ErrorCode::Invalid);
}

SW_TEST(Time_FormatParseRoundTrip)
{
	auto T = ParseTime("2026-09-27T12:34:56.789Z");
	ASSERT_OK(T);
	EXPECT_EQ(FormatTime(*T), std::string("2026-09-27T12:34:56.789Z"));
	EXPECT_EQ(ParseTime("2026-09-27T14:34:56.789+02:00").Value(), *T);
	EXPECT_EQ(ParseTime("2026-09-27T12:34:56Z").Value(), *T - 789);
	EXPECT_EQ(ParseTime("1970-01-01T00:00:00Z").Value(), int64_t(0));
	EXPECT_EQ(FormatTime(-1), std::string("1969-12-31T23:59:59.999Z"));
	EXPECT_EQ(ParseTime("2024-02-29T00:00:00.123456Z").Value() % 1000, int64_t(123));
	for (const char* Bad : {"2026-13-01T00:00:00Z", "2026-09-27 12:00:00Z", "2026-09-27T12:00:00", "garbage", "2026-09-27T12:00:00Zjunk"})
	{
		if (ParseTime(Bad).Ok()) swtest::ReportFailure(__FILE__, __LINE__, std::string("accepted: ") + Bad);
	}
}

SW_TEST(Log_RedactsSecrets)
{
	auto Sink = std::make_shared<MemoryLogSink>();
	Logger L(Sink);
	L.Info("TokenStored", {{"token", "ghp_abc123"}, {"refresh_token", "r1"}, {"Authorization", "Bearer z"}, {"generation", "7"}, {"msg", "has space"}});
	const std::string Line = Sink->Lines().at(0);
	EXPECT_TRUE(Line.find("ghp_abc123") == std::string::npos);
	EXPECT_TRUE(Line.find("Bearer z") == std::string::npos);
	EXPECT_TRUE(Line.find("r1") == std::string::npos);
	EXPECT_TRUE(Line.find("generation=7") != std::string::npos);
	EXPECT_TRUE(Line.find("msg=\"has space\"") != std::string::npos);
}

SW_TEST(Random_UuidShape)
{
	SeededRandom R(42);
	const std::string U = NewUuid(R);
	EXPECT_EQ(U.size(), size_t(36));
	EXPECT_EQ(U[14], '4');
	EXPECT_TRUE(U[19] == '8' || U[19] == '9' || U[19] == 'a' || U[19] == 'b');
	SystemRandom S;
	EXPECT_TRUE(S.Hex(16) != S.Hex(16));
	EXPECT_EQ(S.Hex(8).size(), size_t(16));
}

SW_TEST(Util_MemoryLogSinkKeepsNewestLines)
{
	sw::MemoryLogSink Sink(4);
	for (int i = 0; i < 23; ++i) Sink.Write(sw::LogLevel::Info, "E", {{"n", std::to_string(i)}});
	const auto Lines = Sink.Lines();
	EXPECT_TRUE(Lines.size() >= 4 && Lines.size() <= 5);
	EXPECT_TRUE(Lines.back().find("n=22") != std::string::npos);
	EXPECT_TRUE(!Sink.Contains("n=3 ") && Sink.Contains("n=22"));
	sw::MemoryLogSink Unbounded;
	for (int i = 0; i < 50; ++i) Unbounded.Write(sw::LogLevel::Info, "E", {});
	EXPECT_EQ(Unbounded.Lines().size(), size_t(50));
}

SW_TEST(RefreshCache_OneRefreshInFlightAndRateLimited)
{
	RefreshCache<int> Cache;
	EXPECT_TRUE(!Cache.Get().has_value());
	EXPECT_TRUE(Cache.TryBeginRefresh(100.0, 2.0)); // first refresh always starts, whatever the clock base
	EXPECT_TRUE(!Cache.TryBeginRefresh(105.0, 2.0)); // one in flight
	Cache.Set(7);
	Cache.EndRefresh();
	EXPECT_EQ(*Cache.Get(), 7);
	EXPECT_TRUE(!Cache.TryBeginRefresh(101.0, 2.0)); // too soon
	EXPECT_TRUE(Cache.TryBeginRefresh(102.5, 2.0));
	Cache.Set(std::nullopt); // a failed read clears the value rather than serving stale state
	Cache.EndRefresh();
	EXPECT_TRUE(!Cache.Get().has_value());
}
