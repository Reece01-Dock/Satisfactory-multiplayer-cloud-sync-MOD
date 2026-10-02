#include "UI/SharedWorldStorageModel.h"

#include "SharedWorldSubsystem.h"

FString FSharedWorldStorageProvider::FileOperationsText() const
{
	TArray<FString> Ops;
	if (bSupportsRead) Ops.Add(TEXT("Read"));
	if (bSupportsWrite) Ops.Add(TEXT("Write"));
	if (bSupportsDelete) Ops.Add(TEXT("Delete"));
	if (bSupportsMove) Ops.Add(TEXT("Move"));
	if (bSupportsList) Ops.Add(TEXT("List"));
	return Ops.Num() > 0 ? FString::Join(Ops, TEXT(", ")) : FString(TEXT("None"));
}

FText FSharedWorldStorageProvider::ConnectVerb() const
{
	return Difficulty == ESharedWorldStorageDifficulty::Easy
		? NSLOCTEXT("SharedWorld", "VerbConnect", "Connect")
		: NSLOCTEXT("SharedWorld", "VerbConfigure", "Configure");
}

const FSharedWorldStorageProvider* FSharedWorldStorageCatalog::Find(const FString& ProviderId) const
{
	for (const FSharedWorldStorageProvider& P : Providers)
	{
		if (P.ProviderId == ProviderId) return &P;
	}
	return nullptr;
}

bool FSharedWorldStorageCatalog::Matches(const FSharedWorldStorageProvider& P, const FString& Query)
{
	const FString Q = Query.TrimStartAndEnd();
	if (Q.IsEmpty()) return true;
	if (P.DisplayName.Contains(Q, ESearchCase::IgnoreCase) || P.Description.Contains(Q, ESearchCase::IgnoreCase)
		|| P.Category.Contains(Q, ESearchCase::IgnoreCase))
	{
		return true;
	}
	for (const FString& Tag : P.Tags)
	{
		if (Tag.Contains(Q, ESearchCase::IgnoreCase)) return true; // e.g. "s3" also finds S3-compatible providers
	}
	return false;
}

namespace
{
	FSharedWorldStorageProvider Placeholder(const TCHAR* Id, const TCHAR* Name, const TCHAR* Desc, const TCHAR* Mono,
		const FLinearColor& Color, ESharedWorldStorageDifficulty Difficulty, bool bRecommended,
		const TCHAR* Category, std::initializer_list<const TCHAR*> Tags)
	{
		FSharedWorldStorageProvider P;
		P.ProviderId = Id;
		P.DisplayName = Name;
		P.Description = Desc;
		P.IconMonogram = Mono;
		P.IconColor = Color;
		P.Difficulty = Difficulty;
		P.bRecommended = bRecommended;
		P.Category = Category;
		for (const TCHAR* T : Tags) P.Tags.Add(T);
		return P; // bAvailable / bConnected stay false: display-only ("coming soon") until a real backend exists
	}
}

FSharedWorldStorageCatalog FSharedWorldStorageCatalog::Build(USharedWorldSubsystem& SW)
{
	FSharedWorldStorageCatalog C;

	FString Note;
	const sw::ProviderConfig Default = SW.ResolveDefaultStorage(Note);
	const FString Login = SW.GetGitHubLogin();
	const bool bGitConnected = !Login.IsEmpty();

	// ---- GitHub: the one live cloud provider. Every identity field comes from real state.
	{
		FSharedWorldStorageProvider P;
		P.ProviderId = TEXT("github");
		P.DisplayName = TEXT("GitHub");
		P.Description = TEXT("Use a GitHub repository to store Shared World data.");
		P.IconMonogram = TEXT("GH");
		P.IconColor = FLinearColor(0.10f, 0.11f, 0.13f, 1.f);
		P.Difficulty = ESharedWorldStorageDifficulty::Easy;
		P.bRecommended = true;
		P.Category = TEXT("Version control");
		P.Tags = { TEXT("git"), TEXT("repository"), TEXT("repo") };
		P.bAvailable = true;
		P.bRuntimeBacked = true;
		P.bConnected = bGitConnected;
		if (bGitConnected)
		{
			P.AccountName = Login;
			if (Default.Kind == sw::ProviderKind::GitHub && !Default.Repo.empty())
			{
				P.RepositoryName = FString::Printf(TEXT("%s/%s"), UTF8_TO_TCHAR(Default.Owner.c_str()), UTF8_TO_TCHAR(Default.Repo.c_str()));
			}
			// One branch per world inside the repository (see ProviderConfig).
			P.Location = TEXT("shared-world/* branches");
		}
		// What the GitHub backend in this mod supports (provider-type metadata, not a live probe).
		P.bHasCapabilityInfo = true;
		P.bSupportsRead = P.bSupportsWrite = P.bSupportsDelete = P.bSupportsList = true;
		P.bSupportsHash = true;
		P.bSupportsAtomicWrites = true;
		P.ProviderTier = TEXT("Stable");
		C.Providers.Add(P);
	}

	// ---- Local shared folder: real, used when GitHub is not linked.
	{
		FSharedWorldStorageProvider P;
		P.ProviderId = TEXT("local-folder");
		P.DisplayName = TEXT("Local folder");
		P.Description = TEXT("Keep Shared World data in a folder on this PC or a network share.");
		P.IconMonogram = TEXT("DIR");
		P.IconColor = FLinearColor(0.38f, 0.40f, 0.45f, 1.f);
		P.Difficulty = ESharedWorldStorageDifficulty::Easy;
		P.Category = TEXT("Local");
		P.Tags = { TEXT("folder"), TEXT("network share"), TEXT("disk") };
		P.bAvailable = true;
		P.bRuntimeBacked = true;
		P.bConnected = !bGitConnected && Default.Kind == sw::ProviderKind::Folder;
		if (P.bConnected)
		{
			P.Location = UTF8_TO_TCHAR(Default.FolderPath.c_str());
		}
		P.bHasCapabilityInfo = true;
		P.bSupportsRead = P.bSupportsWrite = P.bSupportsDelete = P.bSupportsList = true;
		P.ProviderTier = TEXT("Local");
		C.Providers.Add(P);
	}

	// ---- display-only entries (no backend yet; rclone discovery will replace these)
	using D = ESharedWorldStorageDifficulty;
	C.Providers.Add(Placeholder(TEXT("google-drive"), TEXT("Google Drive"), TEXT("Reliable cloud storage with plenty of space."), TEXT("GD"),
		FLinearColor(0.16f, 0.50f, 0.32f, 1.f), D::Easy, true, TEXT("Cloud storage"), { TEXT("google"), TEXT("drive") }));
	C.Providers.Add(Placeholder(TEXT("onedrive"), TEXT("OneDrive"), TEXT("Microsoft cloud storage with seamless integration."), TEXT("OD"),
		FLinearColor(0.10f, 0.42f, 0.80f, 1.f), D::Easy, true, TEXT("Cloud storage"), { TEXT("microsoft"), TEXT("office") }));
	C.Providers.Add(Placeholder(TEXT("dropbox"), TEXT("Dropbox"), TEXT("Simple and reliable cloud storage."), TEXT("DB"),
		FLinearColor(0.05f, 0.35f, 0.92f, 1.f), D::Easy, true, TEXT("Cloud storage"), {}));
	C.Providers.Add(Placeholder(TEXT("cloudflare-r2"), TEXT("Cloudflare R2"), TEXT("High performance object storage."), TEXT("R2"),
		FLinearColor(0.88f, 0.45f, 0.12f, 1.f), D::Advanced, false, TEXT("Object storage"), { TEXT("s3"), TEXT("s3-compatible"), TEXT("cloudflare"), TEXT("bucket") }));
	C.Providers.Add(Placeholder(TEXT("amazon-s3"), TEXT("Amazon S3"), TEXT("Scalable cloud storage with global availability."), TEXT("S3"),
		FLinearColor(0.72f, 0.18f, 0.18f, 1.f), D::Advanced, false, TEXT("Object storage"), { TEXT("s3"), TEXT("aws"), TEXT("amazon"), TEXT("bucket") }));
	C.Providers.Add(Placeholder(TEXT("webdav"), TEXT("WebDAV"), TEXT("Connect to any WebDAV server."), TEXT("DAV"),
		FLinearColor(0.42f, 0.34f, 0.80f, 1.f), D::Advanced, false, TEXT("Self-hosted"), { TEXT("nextcloud"), TEXT("owncloud"), TEXT("server") }));
	C.Providers.Add(Placeholder(TEXT("sftp"), TEXT("SFTP"), TEXT("Use your own SFTP server or NAS."), TEXT(">_"),
		FLinearColor(0.10f, 0.55f, 0.40f, 1.f), D::Advanced, false, TEXT("Self-hosted"), { TEXT("ssh"), TEXT("nas"), TEXT("server") }));

	// Active = what new Shared Worlds will actually use. Connected is tracked separately on each provider.
	if (bGitConnected) C.ActiveProviderId = TEXT("github");
	else if (Default.Kind == sw::ProviderKind::Folder) C.ActiveProviderId = TEXT("local-folder");
	else C.ActiveProviderId = TEXT("github");
	for (FSharedWorldStorageProvider& P : C.Providers)
	{
		// A provider can only be active if it is usable: an unlinked GitHub is the default choice but not "active".
		P.bActive = (P.ProviderId == C.ActiveProviderId) && P.bConnected;
	}
	return C;
}
