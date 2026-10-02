#include "Rclone/RcloneObjectStore.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "Misc/Paths.h"
#include "Rclone/RcloneRuntime.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Sha256.h"
#include "SharedWorldTypes.h"

namespace
{
	FString Utf8ToFString(const std::string& S) { return FString(UTF8_TO_TCHAR(S.c_str())); }
	std::string FStringToUtf8(const FString& S) { return std::string(TCHAR_TO_UTF8(*S)); }

	/** Object ids become path segments: only a conservative character set is accepted (no traversal, no separators). */
	bool IsSafeId(const std::string& Id)
	{
		if (Id.empty() || Id.size() > 128) return false;
		for (char C : Id)
		{
			const bool bOk = (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '.' || C == '_' || C == '-';
			if (!bOk) return false;
		}
		return Id != "." && Id != "..";
	}

	FString ObjectRemote(const std::string& Id)
	{
		const FString IdText = Utf8ToFString(Id);
		return FString::Printf(TEXT("objects/%s/%s"), *IdText.Left(2), *IdText);
	}

	FString ToJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	TSharedPtr<FJsonObject> ParseJson(const FString& Text)
	{
		TSharedPtr<FJsonObject> Obj;
		const TSharedRef<TJsonReader<>> Reader = TJsonReaderFactory<>::Create(Text);
		if (!FJsonSerializer::Deserialize(Reader, Obj)) return nullptr;
		return Obj;
	}

	sw::Error MapError(const FRcloneResult& R, const char* What)
	{
		const FString Text = R.ErrorText();
		const FString L = Text.ToLower();
		sw::ErrorCode Code = sw::ErrorCode::Io;
		if (R.Status == 0) Code = sw::ErrorCode::Unsupported; // engine missing / not loaded
		else if (L.Contains(TEXT("not found")) || L.Contains(TEXT("doesn't exist")) || L.Contains(TEXT("does not exist"))) Code = sw::ErrorCode::NotFound;
		else if (L.Contains(TEXT("401")) || L.Contains(TEXT("403")) || L.Contains(TEXT("unauthorized")) || L.Contains(TEXT("invalid_grant")) || L.Contains(TEXT("token"))) Code = sw::ErrorCode::Unauthorized;
		else if (L.Contains(TEXT("429")) || L.Contains(TEXT("rate limit")) || L.Contains(TEXT("too many requests"))) Code = sw::ErrorCode::RateLimited;
		else if (L.Contains(TEXT("timeout")) || L.Contains(TEXT("connection")) || L.Contains(TEXT("dial")) || L.Contains(TEXT("no such host"))) Code = sw::ErrorCode::Network;
		UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/rclone] event=op_failed what=%s status=%d detail=%s"), UTF8_TO_TCHAR(What), R.Status, *Text);
		return sw::MakeError(Code, std::string(What) + ": " + FStringToUtf8(Text));
	}

	/** operations/stat. nullopt on error; bExists reports whether the item exists, Size its length. */
	bool Stat(const FString& Fs, const FString& Remote, bool& bExists, int64& Size, FRcloneResult& OutResult)
	{
		const TSharedRef<FJsonObject> In = MakeShared<FJsonObject>();
		In->SetStringField(TEXT("fs"), Fs);
		In->SetStringField(TEXT("remote"), Remote);
		OutResult = FRcloneRuntime::Get().Rpc(TEXT("operations/stat"), ToJson(In));
		if (!OutResult.bOk) return false;
		bExists = false;
		Size = -1;
		if (const TSharedPtr<FJsonObject> Body = ParseJson(OutResult.Output))
		{
			const TSharedPtr<FJsonObject>* Item = nullptr;
			if (Body->TryGetObjectField(TEXT("item"), Item) && Item && Item->IsValid()) // "item": null when absent
			{
				bExists = true;
				double D = 0;
				if ((*Item)->TryGetNumberField(TEXT("Size"), D)) Size = static_cast<int64>(D);
			}
		}
		return true;
	}

	bool CopyFile(const FString& SrcFs, const FString& SrcRemote, const FString& DstFs, const FString& DstRemote, FRcloneResult& OutResult)
	{
		const TSharedRef<FJsonObject> In = MakeShared<FJsonObject>();
		In->SetStringField(TEXT("srcFs"), SrcFs);
		In->SetStringField(TEXT("srcRemote"), SrcRemote);
		In->SetStringField(TEXT("dstFs"), DstFs);
		In->SetStringField(TEXT("dstRemote"), DstRemote);
		OutResult = FRcloneRuntime::Get().Rpc(TEXT("operations/copyfile"), ToJson(In));
		return OutResult.bOk;
	}
}

FRcloneObjectStore::FRcloneObjectStore(FString InFs) : Fs(MoveTemp(InFs)) {}

sw::Result<bool> FRcloneObjectStore::Has(const std::string& Id)
{
	if (!IsSafeId(Id)) return sw::MakeError(sw::ErrorCode::Invalid, "invalid object id");
	bool bExists = false;
	int64 Size = -1;
	FRcloneResult R;
	if (!Stat(Fs, ObjectRemote(Id), bExists, Size, R))
	{
		if (MapError(R, "stat").Code == sw::ErrorCode::NotFound) return false;
		return MapError(R, "stat");
	}
	return bExists;
}

sw::Status FRcloneObjectStore::Put(const std::string& Id, const std::string& LocalPath)
{
	// Same contract as FileObjectStore::Put: the bytes must hash to the id.
	auto H = sw::file::Hash(LocalPath);
	if (!H.Ok() || H->Sha256 != Id)
	{
		return sw::MakeError(sw::ErrorCode::Corrupt, "object content does not match its id");
	}
	return PutBlob(Id, LocalPath);
}

sw::Status FRcloneObjectStore::PutBlob(const std::string& Id, const std::string& LocalPath)
{
	if (!IsSafeId(Id)) return sw::MakeError(sw::ErrorCode::Invalid, "invalid object id");
	auto Has_ = Has(Id);
	if (!Has_.Ok()) return Has_.Err();
	if (Has_.Value()) return {}; // content-addressed: same id means same bytes

	const FString Local = Utf8ToFString(LocalPath);
	const int64 LocalSize = IFileManager::Get().FileSize(*Local);
	if (LocalSize < 0) return sw::MakeError(sw::ErrorCode::NotFound, "local file not found");

	FRcloneResult R;
	if (!CopyFile(FPaths::GetPath(Local), FPaths::GetCleanFilename(Local), Fs, ObjectRemote(Id), R))
	{
		return MapError(R, "upload");
	}
	// Providers publish an upload atomically (or rclone uploads under a temporary name and renames), but confirm the
	// size so a truncated transfer can never be mistaken for the object. Readers also re-verify the save hash.
	bool bExists = false;
	int64 RemoteSize = -1;
	if (!Stat(Fs, ObjectRemote(Id), bExists, RemoteSize, R)) return MapError(R, "verify upload");
	if (!bExists || (RemoteSize >= 0 && RemoteSize != LocalSize))
	{
		(void)Remove(Id); // do not leave a truncated object behind
		return sw::MakeError(sw::ErrorCode::Corrupt, "uploaded object size does not match");
	}
	return {};
}

sw::Status FRcloneObjectStore::Get(const std::string& Id, const std::string& DestPath)
{
	if (!IsSafeId(Id)) return sw::MakeError(sw::ErrorCode::Invalid, "invalid object id");
	const FString Dest = Utf8ToFString(DestPath);
	const FString DestDir = FPaths::GetPath(Dest);
	const FString Part = FPaths::GetCleanFilename(Dest) + TEXT(".part");
	IFileManager::Get().MakeDirectory(*DestDir, true);
	IFileManager::Get().Delete(*FPaths::Combine(DestDir, Part), false, true, true);

	FRcloneResult R;
	if (!CopyFile(Fs, ObjectRemote(Id), DestDir, Part, R))
	{
		return MapError(R, "download");
	}
	// Never expose a half-written file at the destination: move into place only after the transfer finished.
	if (!IFileManager::Get().Move(*Dest, *FPaths::Combine(DestDir, Part), /*Replace=*/true, /*EvenIfReadOnly=*/true))
	{
		return sw::MakeError(sw::ErrorCode::Io, "could not move the downloaded object into place");
	}
	return {};
}

sw::Status FRcloneObjectStore::Remove(const std::string& Id)
{
	if (!IsSafeId(Id)) return sw::MakeError(sw::ErrorCode::Invalid, "invalid object id");
	const TSharedRef<FJsonObject> In = MakeShared<FJsonObject>();
	In->SetStringField(TEXT("fs"), Fs);
	In->SetStringField(TEXT("remote"), ObjectRemote(Id));
	const FRcloneResult R = FRcloneRuntime::Get().Rpc(TEXT("operations/deletefile"), ToJson(In));
	if (!R.bOk)
	{
		const sw::Error E = MapError(R, "delete");
		if (E.Code == sw::ErrorCode::NotFound) return {}; // already gone
		return E;
	}
	return {};
}

sw::Result<std::vector<std::string>> FRcloneObjectStore::List()
{
	const TSharedRef<FJsonObject> Opt = MakeShared<FJsonObject>();
	Opt->SetBoolField(TEXT("recurse"), true);
	Opt->SetBoolField(TEXT("filesOnly"), true);
	Opt->SetBoolField(TEXT("noModTime"), true);
	Opt->SetBoolField(TEXT("noMimeType"), true);
	const TSharedRef<FJsonObject> In = MakeShared<FJsonObject>();
	In->SetStringField(TEXT("fs"), Fs);
	In->SetStringField(TEXT("remote"), TEXT("objects"));
	In->SetObjectField(TEXT("opt"), Opt);
	const FRcloneResult R = FRcloneRuntime::Get().Rpc(TEXT("operations/list"), ToJson(In));

	std::vector<std::string> Out;
	if (!R.bOk)
	{
		const sw::Error E = MapError(R, "list");
		if (E.Code == sw::ErrorCode::NotFound) return Out; // nothing stored yet
		return E;
	}
	if (const TSharedPtr<FJsonObject> Body = ParseJson(R.Output))
	{
		const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
		if (Body->TryGetArrayField(TEXT("list"), Items) && Items)
		{
			for (const TSharedPtr<FJsonValue>& V : *Items)
			{
				const TSharedPtr<FJsonObject>* Item = nullptr;
				FString Name;
				if (V.IsValid() && V->TryGetObject(Item) && Item && (*Item)->TryGetStringField(TEXT("Name"), Name) && IsSafeId(FStringToUtf8(Name)))
				{
					Out.push_back(FStringToUtf8(Name));
				}
			}
		}
	}
	return Out;
}

std::string FRcloneObjectStore::Describe() const
{
	return "rclone:" + FStringToUtf8(Fs); // never contains credentials: those live in rclone.conf
}
