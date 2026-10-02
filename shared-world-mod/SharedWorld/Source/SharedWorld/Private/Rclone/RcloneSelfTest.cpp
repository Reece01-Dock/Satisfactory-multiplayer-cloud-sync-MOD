#include "Rclone/RcloneSelfTest.h"

#include "HAL/FileManager.h"
#include "Math/RandomStream.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Rclone/RcloneObjectStore.h"
#include "Rclone/RcloneRuntime.h"
#include "SharedWorldCore/Util/FileUtil.h"

FString FRcloneSelfTestResult::Summary() const
{
	return FString::Join(Lines, TEXT("\n"));
}

FRcloneSelfTestResult RunRcloneSelfTest()
{
	FRcloneSelfTestResult Result;
	auto Step = [&Result](bool bPass, const FString& Text)
	{
		Result.Lines.Add(FString::Printf(TEXT("%s %s"), bPass ? TEXT("OK    ") : TEXT("FAILED"), *Text));
		return bPass;
	};

	FRcloneRuntime& Rc = FRcloneRuntime::Get();
	const bool bLoaded = Rc.EnsureLoaded();
	if (!Step(bLoaded, bLoaded ? FString(TEXT("engine loaded")) : FString::Printf(TEXT("engine not loaded: %s"), *Rc.UnavailableReason())))
	{
		return Result;
	}
	const FString Version = Rc.Version();
	if (!Step(!Version.IsEmpty(), FString::Printf(TEXT("engine version %s"), Version.IsEmpty() ? TEXT("unknown") : *Version))) return Result;

	const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SharedWorldRcloneSelfTest"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	const FString RemoteDir = FPaths::Combine(Root, TEXT("remote"));
	const FString SrcFile = FPaths::Combine(Root, TEXT("source.bin"));
	const FString OutFile = FPaths::Combine(Root, TEXT("download"), TEXT("restored.bin"));
	IFileManager::Get().MakeDirectory(*RemoteDir, true);
	auto Cleanup = [&Root]() { IFileManager::Get().DeleteDirectory(*Root, false, true); };

	// ~3 MB of pseudo-random bytes, so it is a real multi-chunk transfer and not an empty file.
	TArray<uint8> Payload;
	Payload.SetNumUninitialized(3 * 1024 * 1024 + 123);
	FRandomStream Rand(0x5A17);
	for (uint8& B : Payload) B = static_cast<uint8>(Rand.RandRange(0, 255));
	if (!Step(FFileHelper::SaveArrayToFile(Payload, *SrcFile), TEXT("wrote test file"))) { Cleanup(); return Result; }

	const std::string SrcUtf8(TCHAR_TO_UTF8(*SrcFile));
	auto Hash = sw::file::Hash(SrcUtf8);
	if (!Step(Hash.Ok(), TEXT("hashed test file"))) { Cleanup(); return Result; }
	const std::string Id = Hash->Sha256;

	FRcloneObjectStore Store(RemoteDir);
	bool bAllOk = true;
	{
		auto R = Store.Has(Id);
		bAllOk &= Step(R.Ok() && !R.Value(), TEXT("object absent before upload"));
	}
	{
		sw::Status S = Store.Put(Id, SrcUtf8);
		bAllOk &= Step(S.Ok(), S.Ok() ? FString(TEXT("uploaded")) : FString::Printf(TEXT("upload: %s"), UTF8_TO_TCHAR(S.Err().Describe().c_str())));
	}
	{
		auto R = Store.Has(Id);
		bAllOk &= Step(R.Ok() && R.Value(), TEXT("object present after upload"));
	}
	{
		sw::Status S = Store.Put(Id, SrcUtf8);
		bAllOk &= Step(S.Ok(), TEXT("second upload is idempotent"));
	}
	{
		auto L = Store.List();
		bool bFound = false;
		if (L.Ok()) for (const std::string& N : L.Value()) bFound |= (N == Id);
		bAllOk &= Step(L.Ok() && bFound, TEXT("listed the object"));
	}
	{
		sw::Status S = Store.Get(Id, std::string(TCHAR_TO_UTF8(*OutFile)));
		bAllOk &= Step(S.Ok(), S.Ok() ? FString(TEXT("downloaded")) : FString::Printf(TEXT("download: %s"), UTF8_TO_TCHAR(S.Err().Describe().c_str())));
		auto H2 = sw::file::Hash(std::string(TCHAR_TO_UTF8(*OutFile)));
		bAllOk &= Step(H2.Ok() && H2->Sha256 == Id, TEXT("downloaded bytes match the original hash"));
	}
	{
		// A hostile id must never become a path.
		sw::Status S = Store.PutBlob("../escape", SrcUtf8);
		bAllOk &= Step(!S.Ok(), TEXT("unsafe object id rejected"));
	}
	{
		sw::Status S = Store.Remove(Id);
		auto R = Store.Has(Id);
		bAllOk &= Step(S.Ok() && R.Ok() && !R.Value(), TEXT("deleted"));
	}
	Cleanup();
	Result.bOk = bAllOk;
	Result.Lines.Add(bAllOk ? TEXT("PASS: rclone engine and object store work end to end.") : TEXT("FAIL: see the FAILED lines above."));
	return Result;
}
