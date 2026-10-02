// LogRepository: compare-and-swap on plain file storage (any rclone provider).

#include <chrono>
#include <memory>
#include <thread>
#include <string>

#include "SharedWorldCore/Storage/LogRepository.h"
#include "TestFramework.h"

using namespace sw;

namespace
{
	std::vector<FileChange> Put(const std::string& Path, const std::string& Content)
	{
		return {FileChange{Path, Content}};
	}
}

SW_TEST(LogRepo_ExclusiveStoreRejectsSecondWriterOnSameHead)
{
	auto Store = std::make_shared<MemoryLogStore>(true);
	LogRepository A(Store), B(Store);
	auto First = A.Commit("", Put("state/current.json", "{\"g\":1}"), "create");
	ASSERT_OK(First);
	auto WinA = A.Commit(*First, Put("state/current.json", "{\"g\":2}"), "a");
	auto LoseB = B.Commit(*First, Put("state/current.json", "{\"g\":2,\"b\":1}"), "b");
	ASSERT_OK(WinA);
	EXPECT_ERR(LoseB, ErrorCode::Conflict);
	auto Head = B.Head();
	ASSERT_OK(Head);
	EXPECT_EQ(*Head, *WinA);
	auto Text = B.ReadFile(*Head, "state/current.json");
	ASSERT_OK(Text);
	EXPECT_EQ(*Text, std::string("{\"g\":2}"));
}

// The hard case: a store that allows duplicate names and shows new files late. Writer B commits while A's entry is
// still invisible to it. Both entries exist; the shared rule must give the slot to exactly one of them.
SW_TEST(LogRepo_DuplicateStoreInvisibleRaceHasExactlyOneWinner)
{
	auto Store = std::make_shared<MemoryLogStore>(false, 15); // listings lag 15 ms
	LogRepositoryConfig BCfg;
	BCfg.SettleMs = 60;
	LogRepository Setup(Store, BCfg);
	auto Base = Setup.Commit("", Put("state/current.json", "base"), "create");
	ASSERT_OK(Base);
	std::this_thread::sleep_for(std::chrono::milliseconds(30)); // base visible to everyone

	auto B = std::make_shared<LogRepository>(Store, BCfg);
	Result<std::string> BResult = MakeError(ErrorCode::Invalid, "not run");
	bool bRanB = false;
	LogRepositoryConfig ACfg;
	ACfg.SettleMs = 60;
	// A's settle wait is where B runs its whole commit: A's entry exists but is not yet listed.
	ACfg.Sleep = [&](TimeMs Ms)
	{
		if (!bRanB)
		{
			bRanB = true;
			BResult = B->Commit(*Base, Put("state/current.json", "from-b"), "b");
		}
		std::this_thread::sleep_for(std::chrono::milliseconds(Ms));
	};
	LogRepository A(Store, ACfg);
	auto AResult = A.Commit(*Base, Put("state/current.json", "from-a"), "a");

	ASSERT_TRUE(bRanB);
	const int Wins = (AResult.Ok() ? 1 : 0) + (BResult.Ok() ? 1 : 0);
	ASSERT_EQ(Wins, 1);
	// Usually A wins (earlier server time). If both entries share a timestamp the id tie-break decides, which is
	// equally fine: what matters is exactly one winner, the loser is told Conflict, and every reader agrees.
	const bool bAWon = AResult.Ok();
	EXPECT_ERR(bAWon ? BResult : AResult, ErrorCode::Conflict);
	const std::string WinnerId = bAWon ? *AResult : *BResult;
	auto Head = Setup.Head();
	ASSERT_OK(Head);
	EXPECT_EQ(*Head, WinnerId);
	EXPECT_EQ(*Setup.ReadFile(*Head, "state/current.json"), std::string(bAWon ? "from-a" : "from-b"));
	LogRepository Fresh(Store); // an independent reader reaches the same verdict
	EXPECT_EQ(*Fresh.Head(), WinnerId);
}

// Same race on an rclone-like store: unique names per writer, no server time. Settle (60) >= 2 x listing delay (15).
SW_TEST(LogRepo_RcloneLikeStoreInvisibleRaceHasExactlyOneWinner)
{
	for (int Round = 0; Round < 5; ++Round)
	{
		auto Store = std::make_shared<MemoryLogStore>(false, 15, false);
		LogRepositoryConfig Cfg;
		Cfg.SettleMs = 60;
		LogRepository Setup(Store, Cfg);
		auto Base = Setup.Commit("", Put("state/current.json", "base"), "create");
		ASSERT_OK(Base);
		std::this_thread::sleep_for(std::chrono::milliseconds(30));

		auto B = std::make_shared<LogRepository>(Store, Cfg);
		Result<std::string> BResult = MakeError(ErrorCode::Invalid, "not run");
		bool bRanB = false;
		LogRepositoryConfig ACfg = Cfg;
		ACfg.Sleep = [&](TimeMs Ms)
		{
			if (!bRanB)
			{
				bRanB = true;
				BResult = B->Commit(*Base, Put("state/current.json", "from-b"), "b");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(Ms));
		};
		LogRepository A(Store, ACfg);
		auto AResult = A.Commit(*Base, Put("state/current.json", "from-a"), "a");
		ASSERT_TRUE(bRanB);
		ASSERT_EQ((AResult.Ok() ? 1 : 0) + (BResult.Ok() ? 1 : 0), 1);
		const std::string WinnerId = AResult.Ok() ? *AResult : *BResult;
		LogRepository Fresh(Store);
		EXPECT_EQ(*Fresh.Head(), WinnerId);
		// The loser's entry was removed or is ignored: a further commit on the winner works for everyone.
		auto Next = Fresh.Commit(WinnerId, Put("state/current.json", "next"), "next");
		ASSERT_OK(Next);
	}
}

SW_TEST(LogRepo_HistoryAndDirectoriesFollowTheWinningChain)
{
	auto Store = std::make_shared<MemoryLogStore>(true);
	LogRepository R(Store);
	auto C1 = R.Commit("", Put("world.json", "w"), "one");
	ASSERT_OK(C1);
	auto C2 = R.Commit(*C1, {FileChange{"revisions/a.json", std::string("a")}, FileChange{"revisions/b.json", std::string("b")}}, "two");
	ASSERT_OK(C2);
	auto C3 = R.Commit(*C2, {FileChange{"revisions/a.json", std::nullopt}}, "three");
	ASSERT_OK(C3);
	auto Listed = R.ListDirectory(*C3, "revisions");
	ASSERT_OK(Listed);
	ASSERT_EQ(Listed->size(), static_cast<size_t>(1));
	EXPECT_EQ((*Listed)[0], std::string("b.json"));
	auto Root = R.ListDirectory(*C3, "");
	ASSERT_OK(Root);
	EXPECT_EQ(Root->size(), static_cast<size_t>(2)); // "revisions", "world.json"
	auto History = R.Log(*C3, 10);
	ASSERT_OK(History);
	ASSERT_EQ(History->size(), static_cast<size_t>(3));
	EXPECT_EQ((*History)[0].Message, std::string("three"));
	EXPECT_EQ((*History)[2].Message, std::string("one"));
	EXPECT_ERR(R.ReadFile(*C3, "revisions/a.json"), ErrorCode::NotFound);
	EXPECT_EQ(*R.ReadFile(*C2, "revisions/a.json"), std::string("a")); // old commits are immutable
}

SW_TEST(LogRepo_CompactionKeepsTheWorldReadable)
{
	auto Store = std::make_shared<MemoryLogStore>(true);
	LogRepositoryConfig C;
	C.KeepEntries = 8;
	LogRepository R(Store, C);
	std::string Head;
	for (int i = 0; i < 40; ++i)
	{
		auto Next = R.Commit(Head, Put("state/current.json", std::to_string(i)), "n");
		ASSERT_OK(Next);
		Head = *Next;
	}
	LogRepository Fresh(Store); // a new reader with an empty cache
	auto H = Fresh.Head();
	ASSERT_OK(H);
	EXPECT_EQ(*H, Head);
	EXPECT_EQ(*Fresh.ReadFile(*H, "state/current.json"), std::string("39"));
	auto Listed = Store->List("log");
	ASSERT_OK(Listed);
	EXPECT_TRUE(Listed->size() < 40); // old entries were removed
}
