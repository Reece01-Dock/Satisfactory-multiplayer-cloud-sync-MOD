#include "SharedWorldCore/Save/SaveFile.h"
#include "SharedWorldCore/Storage/Compress.h"
#include "SharedWorldCore/Storage/EncodingObjectStore.h"
#include "SharedWorldCore/Storage/MemoryStorage.h"
#include "SharedWorldCore/Storage/SaveObject.h"
#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Sha256.h"
#include "TestFramework.h"
#include "TestHelpers.h"

using namespace sw;
using namespace swtest;

namespace
{
	std::string WriteSynth(const std::string& Dir, const std::string& Name, size_t BodyBytes)
	{
		std::string Body(BodyBytes, '\0');
		for (size_t i = 0; i < Body.size(); ++i) Body[i] = static_cast<char>(i * 17 + 3);
		const std::string P = file::Join(Dir, Name + ".sav");
		(void)file::Remove(P);
		(void)file::CreateExclusive(P, save::BuildSynthetic("S", Body));
		return P;
	}
}

SW_TEST(Compression_ZstdRoundTripSynthetic)
{
	const std::string Dir = TempDir();
	const std::string Sav = WriteSynth(Dir, "a", 400 * 1024);
	auto H0 = file::Hash(Sav);
	ASSERT_OK(H0);

	const std::string Pkg = file::Join(Dir, "a.swob");
	CompressOptions Opt;
	Opt.Kind = CompressionKind::Zstd;
	Opt.Level = 3;
	Opt.MinRatioGain = -1.0;
	auto Packed = PackSaveObject(Sav, Pkg, Opt);
	ASSERT_OK(Packed);
	EXPECT_TRUE(Packed->Kind == CompressionKind::Zstd || Packed->Kind == CompressionKind::None);

	const std::string Out = file::Join(Dir, "out.sav");
	ASSERT_OK(UnpackSaveObject(Pkg, Out, H0->Sha256, H0->Size, nullptr));
	auto H1 = file::Hash(Out);
	ASSERT_OK(H1);
	EXPECT_EQ(H0->Sha256, H1->Sha256);
}

SW_TEST(Compression_LegacyRawStillLoads)
{
	const std::string Dir = TempDir();
	const std::string Sav = WriteSynth(Dir, "legacy", 8192);
	auto H0 = file::Hash(Sav);
	ASSERT_OK(H0);
	const std::string Out = file::Join(Dir, "legacy-out.sav");
	ASSERT_OK(UnpackSaveObject(Sav, Out, H0->Sha256, H0->Size, nullptr));
	auto H1 = file::Hash(Out);
	ASSERT_OK(H1);
	EXPECT_EQ(H0->Sha256, H1->Sha256);
}

SW_TEST(Compression_EncodingObjectStoreDedupAndGet)
{
	const std::string Dir = TempDir();
	const std::string Sav = WriteSynth(Dir, "enc", 200 * 1024);
	auto H0 = file::Hash(Sav);
	ASSERT_OK(H0);

	auto Mem = std::make_shared<MemoryObjectStore>();
	EncodingObjectStoreConfig Cfg;
	Cfg.Compress.Kind = CompressionKind::Zstd;
	Cfg.Compress.Level = 1;
	Cfg.Compress.MinRatioGain = -1.0;
	EncodingObjectStore Store(Mem, Cfg);

	ASSERT_OK(Store.Put(H0->Sha256, Sav));
	EXPECT_EQ(Store.LastPutEncoding().UncompressedSize, H0->Size);

	auto Has = Store.Has(H0->Sha256);
	ASSERT_OK(Has);
	EXPECT_TRUE(*Has);

	ASSERT_OK(Store.Put(H0->Sha256, Sav));

	const std::string Dest = file::Join(Dir, "got.sav");
	ASSERT_OK(Store.Get(H0->Sha256, Dest));
	auto H1 = file::Hash(Dest);
	ASSERT_OK(H1);
	EXPECT_EQ(H0->Sha256, H1->Sha256);
	ASSERT_OK(save::ValidateFile(Dest));
}

SW_TEST(Compression_CorruptedPackageRejected)
{
	const std::string Dir = TempDir();
	const std::string Sav = WriteSynth(Dir, "c", 4096);
	auto H0 = file::Hash(Sav);
	ASSERT_OK(H0);
	const std::string Pkg = file::Join(Dir, "c.swob");
	CompressOptions Opt;
	Opt.Kind = CompressionKind::Zstd;
	Opt.Level = 1;
	Opt.MinRatioGain = -1.0;
	ASSERT_OK(PackSaveObject(Sav, Pkg, Opt));

	auto Bytes = file::ReadAll(Pkg, 1 << 30);
	ASSERT_OK(Bytes);
	if (Bytes->size() > 80) (*Bytes)[80] ^= 0x5a;
	(void)file::Remove(Pkg);
	ASSERT_OK(file::CreateExclusive(Pkg, *Bytes));

	const std::string Out = file::Join(Dir, "bad.sav");
	EXPECT_TRUE(!UnpackSaveObject(Pkg, Out, H0->Sha256, H0->Size, nullptr).Ok());
}
