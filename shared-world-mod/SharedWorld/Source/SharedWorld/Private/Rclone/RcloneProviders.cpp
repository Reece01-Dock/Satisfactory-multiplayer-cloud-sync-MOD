#include "Rclone/RcloneProviders.h"

#include "Dom/JsonObject.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformProcess.h"
#include "Math/RandomStream.h"
#include "Misc/FileHelper.h"
#include "Misc/Guid.h"
#include "Misc/Paths.h"
#include "Rclone/RcloneObjectStore.h"
#include "Serialization/JsonReader.h"
#include "Serialization/JsonSerializer.h"
#include "Serialization/JsonWriter.h"
#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldTypes.h"

namespace
{
	FCriticalSection GProvidersLock;
	TSharedPtr<const TArray<FRcloneBackend>> GProviders;

	FString ToJson(const TSharedRef<FJsonObject>& Obj)
	{
		FString Out;
		const TSharedRef<TJsonWriter<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>> Writer =
			TJsonWriterFactory<TCHAR, TCondensedJsonPrintPolicy<TCHAR>>::Create(&Out);
		FJsonSerializer::Serialize(Obj, Writer);
		return Out;
	}

	FString StringField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Name)
	{
		FString V;
		if (Obj.IsValid()) Obj->TryGetStringField(Name, V);
		return V;
	}

	bool BoolField(const TSharedPtr<FJsonObject>& Obj, const TCHAR* Name)
	{
		bool V = false;
		if (Obj.IsValid()) Obj->TryGetBoolField(Name, V);
		return V;
	}

	FString ConnectionsPath()
	{
		return FPaths::Combine(FPlatformProcess::UserSettingsDir(), TEXT("SatisfactorySharedWorld"), TEXT("rclone-connections.json"));
	}
}

// ---------------------------------------------------------------------------------------------- schema

bool FRcloneOption::AppliesTo(const FString& ProviderValue) const
{
	if (ProviderFilter.IsEmpty()) return true;
	FString Filter = ProviderFilter;
	const bool bNegated = Filter.StartsWith(TEXT("!"));
	if (bNegated) Filter.RightChopInline(1);
	TArray<FString> Items;
	Filter.ParseIntoArray(Items, TEXT(","), /*CullEmpty=*/true);
	const bool bListed = Items.Contains(ProviderValue);
	return bNegated ? !bListed : bListed;
}

const FRcloneOption* FRcloneBackend::FindOption(const FString& OptionName) const
{
	for (const FRcloneOption& O : Options)
	{
		if (O.Name == OptionName) return &O;
	}
	return nullptr;
}

FString FRcloneBackend::DisplayName() const
{
	static const TMap<FString, FString> Overrides = {
		{ TEXT("drive"), TEXT("Google Drive") },
		{ TEXT("onedrive"), TEXT("Microsoft OneDrive") },
		{ TEXT("dropbox"), TEXT("Dropbox") },
		{ TEXT("s3"), TEXT("S3-compatible storage") },
		{ TEXT("sftp"), TEXT("SFTP (SSH)") },
		{ TEXT("webdav"), TEXT("WebDAV") },
		{ TEXT("azureblob"), TEXT("Azure Blob Storage") },
		{ TEXT("google cloud storage"), TEXT("Google Cloud Storage") },
		{ TEXT("b2"), TEXT("Backblaze B2") },
		{ TEXT("smb"), TEXT("SMB / Windows share") },
		{ TEXT("ftp"), TEXT("FTP") },
	};
	if (const FString* O = Overrides.Find(Name)) return *O;
	FString D = Description;
	int32 Cut = INDEX_NONE;
	if (D.FindChar(TEXT(','), Cut)) D.LeftInline(Cut);
	if (D.Len() > 40) D = D.Left(40).TrimEnd() + TEXT("...");
	return D.IsEmpty() ? Name : D;
}

TSharedPtr<const TArray<FRcloneBackend>> FRcloneProviders::Get()
{
	FScopeLock L(&GProvidersLock);
	return GProviders;
}

bool FRcloneProviders::IsStorageBackend(const FString& N)
{
	// Wrappers, aliases, read-only and in-memory backends are not places to keep a world.
	static const TSet<FString> Excluded = {
		TEXT("alias"), TEXT("combine"), TEXT("union"), TEXT("crypt"), TEXT("cache"), TEXT("chunker"), TEXT("compress"),
		TEXT("hasher"), TEXT("local"), TEXT("memory"), TEXT("http"), TEXT("doi"), TEXT("archive"), TEXT("google photos"),
		TEXT("tardigrade"), TEXT("netstorage"),
	};
	return !Excluded.Contains(N);
}

bool FRcloneProviders::Load(FString& OutError)
{
	if (Get().IsValid()) return true;
	FRcloneRuntime& Rc = FRcloneRuntime::Get();
	const FRcloneResult R = Rc.Rpc(TEXT("config/providers"), TEXT("{}"));
	if (!R.bOk)
	{
		OutError = R.ErrorText();
		return false;
	}
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(R.Output), Root) || !Root.IsValid())
	{
		OutError = TEXT("rclone returned an unreadable provider list.");
		return false;
	}
	const TArray<TSharedPtr<FJsonValue>>* Providers = nullptr;
	if (!Root->TryGetArrayField(TEXT("providers"), Providers) || !Providers)
	{
		OutError = TEXT("rclone returned no providers.");
		return false;
	}

	TSharedRef<TArray<FRcloneBackend>> Out = MakeShared<TArray<FRcloneBackend>>();
	for (const TSharedPtr<FJsonValue>& PV : *Providers)
	{
		const TSharedPtr<FJsonObject>* PO = nullptr;
		if (!PV.IsValid() || !PV->TryGetObject(PO) || !PO) continue;
		FRcloneBackend B;
		B.Name = StringField(*PO, TEXT("Name"));
		B.Description = StringField(*PO, TEXT("Description"));
		if (B.Name.IsEmpty()) continue;
		const TArray<TSharedPtr<FJsonValue>>* Options = nullptr;
		if ((*PO)->TryGetArrayField(TEXT("Options"), Options) && Options)
		{
			for (const TSharedPtr<FJsonValue>& OV : *Options)
			{
				const TSharedPtr<FJsonObject>* OO = nullptr;
				if (!OV.IsValid() || !OV->TryGetObject(OO) || !OO) continue;
				FRcloneOption O;
				O.Name = StringField(*OO, TEXT("Name"));
				O.Help = StringField(*OO, TEXT("Help"));
				O.Default = StringField(*OO, TEXT("DefaultStr"));
				O.Type = StringField(*OO, TEXT("Type"));
				O.ProviderFilter = StringField(*OO, TEXT("Provider"));
				O.bRequired = BoolField(*OO, TEXT("Required"));
				O.bPassword = BoolField(*OO, TEXT("IsPassword"));
				O.bAdvanced = BoolField(*OO, TEXT("Advanced"));
				O.bExclusive = BoolField(*OO, TEXT("Exclusive"));
				double Hide = 0;
				(*OO)->TryGetNumberField(TEXT("Hide"), Hide);
				O.bHidden = (static_cast<int32>(Hide) & 2) != 0; // rclone: hidden from the interactive configurator
				const TArray<TSharedPtr<FJsonValue>>* Examples = nullptr;
				if ((*OO)->TryGetArrayField(TEXT("Examples"), Examples) && Examples)
				{
					for (const TSharedPtr<FJsonValue>& EV : *Examples)
					{
						const TSharedPtr<FJsonObject>* EO = nullptr;
						if (!EV.IsValid() || !EV->TryGetObject(EO) || !EO) continue;
						FRcloneOptionExample E;
						E.Value = StringField(*EO, TEXT("Value"));
						E.Help = StringField(*EO, TEXT("Help"));
						O.Examples.Add(MoveTemp(E));
					}
				}
				if (!O.Name.IsEmpty()) B.Options.Add(MoveTemp(O));
			}
		}
		// Browser sign-in backends carry the full OAuth set; the player never types tokens or endpoints by hand.
		B.bOAuth = B.FindOption(TEXT("client_id")) && B.FindOption(TEXT("client_secret")) && B.FindOption(TEXT("token"))
			&& B.FindOption(TEXT("auth_url")) && B.FindOption(TEXT("token_url"));
		if (B.bOAuth)
		{
			for (FRcloneOption& O : B.Options)
			{
				if (O.Name == TEXT("token") || O.Name == TEXT("auth_url") || O.Name == TEXT("token_url") || O.Name == TEXT("client_credentials"))
				{
					O.bHidden = true;
				}
			}
		}
		Out->Add(MoveTemp(B));
	}
	Out->Sort([](const FRcloneBackend& A, const FRcloneBackend& B) { return A.DisplayName() < B.DisplayName(); });
	UE_LOG(LogSharedWorld, Log, TEXT("[SharedWorld/rclone] event=providers_loaded count=%d"), Out->Num());
	FScopeLock L(&GProvidersLock);
	GProviders = Out;
	return true;
}

// ---------------------------------------------------------------------------------------------- connections

FString FRcloneConnection::Fs() const
{
	FString F = Folder;
	F.TrimStartAndEndInline();
	while (F.StartsWith(TEXT("/"))) F.RightChopInline(1);
	return RemoteName + TEXT(":") + F;
}

TArray<FRcloneConnection> FRcloneConnections::Load()
{
	TArray<FRcloneConnection> Out;
	FString Text;
	if (!FFileHelper::LoadFileToString(Text, *ConnectionsPath())) return Out;
	TSharedPtr<FJsonObject> Root;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(Text), Root) || !Root.IsValid()) return Out;
	const TArray<TSharedPtr<FJsonValue>>* Items = nullptr;
	if (!Root->TryGetArrayField(TEXT("connections"), Items) || !Items) return Out;
	for (const TSharedPtr<FJsonValue>& V : *Items)
	{
		const TSharedPtr<FJsonObject>* O = nullptr;
		if (!V.IsValid() || !V->TryGetObject(O) || !O) continue;
		FRcloneConnection C;
		C.RemoteName = StringField(*O, TEXT("remote"));
		C.CatalogId = StringField(*O, TEXT("catalogId"));
		C.BackendType = StringField(*O, TEXT("type"));
		C.Label = StringField(*O, TEXT("label"));
		C.Folder = StringField(*O, TEXT("folder"));
		FDateTime::ParseIso8601(*StringField(*O, TEXT("created")), C.CreatedUtc);
		C.bVerified = BoolField(*O, TEXT("verified"));
		FDateTime::ParseIso8601(*StringField(*O, TEXT("verifiedAt")), C.VerifiedUtc);
		if (!C.RemoteName.IsEmpty()) Out.Add(MoveTemp(C));
	}
	return Out;
}

bool FRcloneConnections::Save(const TArray<FRcloneConnection>& Connections)
{
	TArray<TSharedPtr<FJsonValue>> Items;
	for (const FRcloneConnection& C : Connections)
	{
		const TSharedRef<FJsonObject> O = MakeShared<FJsonObject>();
		O->SetStringField(TEXT("remote"), C.RemoteName);
		O->SetStringField(TEXT("catalogId"), C.CatalogId);
		O->SetStringField(TEXT("type"), C.BackendType);
		O->SetStringField(TEXT("label"), C.Label);
		O->SetStringField(TEXT("folder"), C.Folder);
		O->SetStringField(TEXT("created"), C.CreatedUtc.ToIso8601());
		O->SetBoolField(TEXT("verified"), C.bVerified);
		O->SetStringField(TEXT("verifiedAt"), C.VerifiedUtc.ToIso8601());
		Items.Add(MakeShared<FJsonValueObject>(O));
	}
	const TSharedRef<FJsonObject> Root = MakeShared<FJsonObject>();
	Root->SetArrayField(TEXT("connections"), Items);
	IFileManager::Get().MakeDirectory(*FPaths::GetPath(ConnectionsPath()), true);
	return FFileHelper::SaveStringToFile(ToJson(Root), *ConnectionsPath());
}

FString FRcloneConnections::MakeRemoteName(const FString& BackendType, const TArray<FRcloneConnection>& Existing)
{
	FString Base = TEXT("sw-");
	for (TCHAR Ch : BackendType)
	{
		Base.AppendChar(FChar::IsAlnum(Ch) ? FChar::ToLower(Ch) : TEXT('-'));
	}
	for (int32 N = 1; N < 1000; ++N)
	{
		const FString Candidate = FString::Printf(TEXT("%s-%d"), *Base, N);
		bool bTaken = false;
		for (const FRcloneConnection& C : Existing) bTaken |= C.RemoteName.Equals(Candidate, ESearchCase::IgnoreCase);
		if (!bTaken) return Candidate;
	}
	return Base + TEXT("-") + FGuid::NewGuid().ToString(EGuidFormats::Digits).Left(8);
}

// ---------------------------------------------------------------------------------------------- actions

FRcloneResult RcloneActions::CreateRemote(const FString& RemoteName, const FString& BackendType, const TMap<FString, FString>& Parameters)
{
	// rclone's non-interactive configuration is a small state machine: each call either finishes (State empty) or returns
	// the next question. Nothing here knows a provider; questions are answered from rclone's own defaults, except the
	// browser sign-in (config_is_local = true), which is what makes rclone open the player's browser and wait.
	FRcloneRuntime& Rc = FRcloneRuntime::Get();

	const TSharedRef<FJsonObject> Params = MakeShared<FJsonObject>();
	for (const TPair<FString, FString>& P : Parameters)
	{
		Params->SetStringField(P.Key, P.Value);
	}
	const TSharedRef<FJsonObject> Opt = MakeShared<FJsonObject>();
	Opt->SetBoolField(TEXT("nonInteractive"), true);
	Opt->SetBoolField(TEXT("obscure"), true); // plain-text passwords from the form are obscured by rclone before they hit disk
	const TSharedRef<FJsonObject> In = MakeShared<FJsonObject>();
	In->SetStringField(TEXT("name"), RemoteName);
	In->SetStringField(TEXT("type"), BackendType);
	In->SetObjectField(TEXT("parameters"), Params);
	In->SetObjectField(TEXT("opt"), Opt);

	FRcloneResult R = Rc.Rpc(TEXT("config/create"), ToJson(In));
	for (int32 Step = 0; Step < 16 && R.bOk; ++Step)
	{
		TSharedPtr<FJsonObject> Body;
		if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(R.Output), Body) || !Body.IsValid()) break;
		const FString Error = StringField(Body, TEXT("Error"));
		if (!Error.IsEmpty())
		{
			R.bOk = false;
			R.Output = ToJson([&] { const TSharedRef<FJsonObject> E = MakeShared<FJsonObject>(); E->SetStringField(TEXT("error"), Error); return E; }());
			break;
		}
		const FString State = StringField(Body, TEXT("State"));
		if (State.IsEmpty()) break; // finished

		const TSharedPtr<FJsonObject>* Option = nullptr;
		FString Answer;
		FString OptionName;
		if (Body->TryGetObjectField(TEXT("Option"), Option) && Option && Option->IsValid())
		{
			OptionName = StringField(*Option, TEXT("Name"));
			Answer = StringField(*Option, TEXT("DefaultStr"));
			const TArray<TSharedPtr<FJsonValue>>* Examples = nullptr;
			if ((*Option)->TryGetArrayField(TEXT("Examples"), Examples) && Examples && Examples->Num() > 0)
			{
				// A choice question whose default is not one of the offered values: take the first offered value.
				bool bListed = false;
				FString First;
				for (const TSharedPtr<FJsonValue>& EV : *Examples)
				{
					const TSharedPtr<FJsonObject>* EO = nullptr;
					if (EV.IsValid() && EV->TryGetObject(EO) && EO)
					{
						const FString V = StringField(*EO, TEXT("Value"));
						if (First.IsEmpty()) First = V;
						bListed |= (V == Answer);
					}
				}
				bool bExclusive = false;
				(*Option)->TryGetBoolField(TEXT("Exclusive"), bExclusive);
				if (bExclusive && !bListed) Answer = First;
			}
		}
		if (OptionName == TEXT("config_is_local") || OptionName == TEXT("config_shared_client_id")) Answer = TEXT("true");

		const TSharedRef<FJsonObject> NextOpt = MakeShared<FJsonObject>();
		NextOpt->SetBoolField(TEXT("nonInteractive"), true);
		NextOpt->SetBoolField(TEXT("obscure"), true);
		NextOpt->SetBoolField(TEXT("continue"), true);
		NextOpt->SetStringField(TEXT("state"), State);
		NextOpt->SetStringField(TEXT("result"), Answer);
		const TSharedRef<FJsonObject> Next = MakeShared<FJsonObject>();
		// rclone requires name, type and parameters on every step, not only the first.
		Next->SetStringField(TEXT("name"), RemoteName);
		Next->SetStringField(TEXT("type"), BackendType);
		Next->SetObjectField(TEXT("parameters"), MakeShared<FJsonObject>());
		Next->SetObjectField(TEXT("opt"), NextOpt);
		R = Rc.Rpc(TEXT("config/create"), ToJson(Next));
	}
	return R;
}

FRcloneResult RcloneActions::DeleteRemote(const FString& RemoteName)
{
	const TSharedRef<FJsonObject> In = MakeShared<FJsonObject>();
	In->SetStringField(TEXT("name"), RemoteName);
	return FRcloneRuntime::Get().Rpc(TEXT("config/delete"), ToJson(In));
}

FRcloneAbout RcloneActions::About(const FString& Fs)
{
	FRcloneAbout Out;
	const TSharedRef<FJsonObject> In = MakeShared<FJsonObject>();
	In->SetStringField(TEXT("fs"), Fs);
	const FRcloneResult R = FRcloneRuntime::Get().Rpc(TEXT("operations/about"), ToJson(In));
	if (!R.bOk) return Out;
	TSharedPtr<FJsonObject> Body;
	if (!FJsonSerializer::Deserialize(TJsonReaderFactory<>::Create(R.Output), Body) || !Body.IsValid()) return Out;
	double V = 0;
	Out.bOk = true;
	if (Body->TryGetNumberField(TEXT("free"), V)) Out.Free = static_cast<int64>(V);
	if (Body->TryGetNumberField(TEXT("total"), V)) Out.Total = static_cast<int64>(V);
	if (Body->TryGetNumberField(TEXT("used"), V)) Out.Used = static_cast<int64>(V);
	return Out;
}

bool RcloneActions::ProbeStorage(const FString& Fs, FString& OutError)
{
	const FString Root = FPaths::Combine(FPaths::ProjectSavedDir(), TEXT("SharedWorldRcloneProbe"), FGuid::NewGuid().ToString(EGuidFormats::Digits));
	IFileManager::Get().MakeDirectory(*Root, true);
	auto Cleanup = [&Root]() { IFileManager::Get().DeleteDirectory(*Root, false, true); };
	const FString Src = FPaths::Combine(Root, TEXT("probe.bin"));
	const FString Dst = FPaths::Combine(Root, TEXT("back.bin"));

	TArray<uint8> Bytes;
	Bytes.SetNumUninitialized(4096);
	FRandomStream Rand(static_cast<int32>(FGuid::NewGuid().A));
	for (uint8& B : Bytes) B = static_cast<uint8>(Rand.RandRange(0, 255));
	if (!FFileHelper::SaveArrayToFile(Bytes, *Src)) { OutError = TEXT("Could not create a test file on this PC."); Cleanup(); return false; }

	const std::string SrcUtf8(TCHAR_TO_UTF8(*Src));
	auto H = sw::file::Hash(SrcUtf8);
	if (!H.Ok()) { OutError = TEXT("Could not hash the test file."); Cleanup(); return false; }
	const std::string Id = H->Sha256;

	FRcloneObjectStore Store(Fs);
	auto Fail = [&](const TCHAR* Step, const sw::Error& E)
	{
		OutError = FString::Printf(TEXT("%s failed: %s"), Step, UTF8_TO_TCHAR(E.Message.c_str()));
		(void)Store.Remove(Id);
		Cleanup();
		return false;
	};
	sw::Status P = Store.Put(Id, SrcUtf8);
	if (!P.Ok()) return Fail(TEXT("Writing a test file"), P.Err());
	sw::Status G = Store.Get(Id, std::string(TCHAR_TO_UTF8(*Dst)));
	if (!G.Ok()) return Fail(TEXT("Reading it back"), G.Err());
	auto H2 = sw::file::Hash(std::string(TCHAR_TO_UTF8(*Dst)));
	if (!H2.Ok() || H2->Sha256 != Id) return Fail(TEXT("Verifying the file"), sw::MakeError(sw::ErrorCode::Corrupt, "the stored file differs from what was written"));
	sw::Status R = Store.Remove(Id);
	if (!R.Ok()) return Fail(TEXT("Cleaning up"), R.Err());
	Cleanup();
	return true;
}
