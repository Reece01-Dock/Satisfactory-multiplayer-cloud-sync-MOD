// Moving a whole world (record + locks + saves) to plain file storage, e.g. GitHub -> Dropbox.

#include <memory>
#include <string>

#include "SharedWorldCore/Lease/Lease.h"
#include "SharedWorldCore/Storage/LogRepository.h"
#include "SharedWorldCore/Storage/MemoryStorage.h"
#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Sha256.h"
#include "SharedWorldCore/World/MoveSaves.h"
#include "TestFramework.h"
#include "TestHelpers.h"

using namespace sw;
using namespace swtest;

namespace
{
	std::string PutObject(IObjectStore& Store, const std::string& Content)
	{
		const std::string Path = file::Join(swtest::TempDir(), "obj.bin");
		if (!file::WriteAtomic(Path, Content)) return {};
		const std::string Id = Sha256::HexOf(Content);
		return Store.Put(Id, Path) ? Id : std::string();
	}
}

SW_TEST(MoveWorld_WholeWorldMovesAndOldCopyIsFrozen)
{
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto OldRepo = std::make_shared<MemoryRepository>();
	auto OldObjects = std::make_shared<MemoryObjectStore>();
	ASSERT_OK(CreateTestWorld(OldRepo, Clock));
	auto OldLeases = MakeLeases(OldRepo, Clock);

	// A host publishes revision 1 on the old storage.
	auto Acq = OldLeases->Acquire(Player(1), "n1");
	ASSERT_OK(Acq);
	ASSERT_TRUE(Acq->Token.has_value());
	LeaseToken Tok = *Acq->Token;
	const std::string Obj = PutObject(*OldObjects, "the factory save");
	ASSERT_TRUE(!Obj.empty());
	RevisionMeta Rev;
	Rev.Number = 1;
	Rev.Generation = Tok.Generation;
	Rev.PreviousRevision = 0;
	Rev.ObjectSha256 = Obj;
	Rev.Size = 16;
	Rev.CreatedAt = StartTime;
	Rev.Uploader = Player(1);
	Rev.Reason = Reason::Checkpoint;
	ASSERT_OK(OldLeases->CommitRevision(Tok, Rev));
	ASSERT_OK(OldLeases->Release(Tok));
	const int64_t GenBefore = OldLeases->Store().Load()->State.Generation;

	// Move to an rclone-like log store.
	auto NewStore = std::make_shared<MemoryLogStore>(false, 0, false);
	LogRepositoryConfig Cfg;
	Cfg.SettleMs = 1;
	auto NewRepo = std::make_shared<LogRepository>(NewStore, Cfg);
	auto NewObjects = std::make_shared<MemoryObjectStore>();
	auto Moved = MoveWholeWorld(*OldLeases, *OldObjects, *NewRepo, *NewObjects, {"dropbox", "Dropbox"}, Player(1), swtest::TempDir());
	ASSERT_OK(Moved);
	EXPECT_EQ(Moved->Copied, 1);

	// New copy: same world, unhosted, generation continues, save present.
	auto NewLeases = MakeLeases(NewRepo, Clock);
	auto NewSnap = NewLeases->Store().Load();
	ASSERT_OK(NewSnap);
	EXPECT_TRUE(!NewSnap->State.CurrentLease.has_value());
	EXPECT_TRUE(NewSnap->State.Generation >= GenBefore);
	ASSERT_TRUE(NewSnap->State.Head.has_value());
	EXPECT_EQ(NewSnap->State.Head->ObjectSha256, Obj);
	EXPECT_TRUE(*NewObjects->Has(Obj));
	auto NewInfo = NewLeases->Store().LoadInfo(NewSnap->CommitId);
	ASSERT_OK(NewInfo);
	EXPECT_TRUE(NewInfo->MovedTo.IsDefault());

	// The lease works on the new copy, with a higher generation than anything before.
	auto NewAcq = NewLeases->Acquire(Player(2), "n2");
	ASSERT_OK(NewAcq);
	ASSERT_TRUE(NewAcq->Token.has_value());
	EXPECT_TRUE(NewAcq->Token->Generation > GenBefore);

	// Old copy: frozen with a movedTo marker, unhosted.
	auto OldSnap = OldLeases->Store().Load();
	ASSERT_OK(OldSnap);
	auto OldInfo = OldLeases->Store().LoadInfo(OldSnap->CommitId);
	ASSERT_OK(OldInfo);
	EXPECT_EQ(OldInfo->MovedTo.Backend, std::string("dropbox"));
	EXPECT_TRUE(!OldSnap->State.CurrentLease.has_value());

	// Moving again onto a folder that already holds the world is refused.
	auto Again = MoveWholeWorld(*OldLeases, *OldObjects, *NewRepo, *NewObjects, {"dropbox", "Dropbox"}, Player(1), swtest::TempDir());
	EXPECT_ERR(Again, ErrorCode::AlreadyExists);
}

SW_TEST(MoveWorld_RefusedWhileSomeoneHosts)
{
	auto Clock = std::make_shared<FakeClock>(StartTime);
	auto OldRepo = std::make_shared<MemoryRepository>();
	auto OldObjects = std::make_shared<MemoryObjectStore>();
	ASSERT_OK(CreateTestWorld(OldRepo, Clock));
	auto OldLeases = MakeLeases(OldRepo, Clock);
	auto Host = OldLeases->Acquire(Player(1), "host");
	ASSERT_OK(Host);
	ASSERT_TRUE(Host->Token.has_value());

	auto NewRepo = std::make_shared<LogRepository>(std::make_shared<MemoryLogStore>(true));
	auto NewObjects = std::make_shared<MemoryObjectStore>();
	auto Moved = MoveWholeWorld(*OldLeases, *OldObjects, *NewRepo, *NewObjects, {"dropbox", "Dropbox"}, Player(2), swtest::TempDir());
	EXPECT_ERR(Moved, ErrorCode::Conflict);
	EXPECT_ERR(NewRepo->Head(), ErrorCode::NotFound); // nothing was written
}
