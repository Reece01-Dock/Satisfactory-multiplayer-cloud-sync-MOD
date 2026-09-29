#include "SharedWorldCore/Storage/EncodingObjectStore.h"

#include "SharedWorldCore/Util/FileUtil.h"
#include "SharedWorldCore/Util/Sha256.h"

namespace sw
{
	EncodingObjectStore::EncodingObjectStore(std::shared_ptr<IObjectStore> Inner, EncodingObjectStoreConfig Cfg)
		: Inner_(std::move(Inner)), Cfg_(std::move(Cfg))
	{
	}

	Result<bool> EncodingObjectStore::Has(const std::string& Sha256)
	{
		return Inner_->Has(Sha256);
	}

	Status EncodingObjectStore::Put(const std::string& Sha256, const std::string& LocalPath)
	{
		auto H = file::Hash(LocalPath);
		if (!H) return H.Err();
		if (H->Sha256 != Sha256)
			return MakeError(ErrorCode::Invalid, "Put logical hash mismatch");

		LastPut_ = {};
		LastStats_ = {};

		if (!Cfg_.bEnabled)
		{
			LastPut_.Compression = CompressionKind::None;
			LastPut_.UncompressedSize = H->Size;
			LastPut_.CompressedSize = H->Size;
			return Inner_->Put(Sha256, LocalPath);
		}

		// Idempotent: existing object (raw or package) is fine.
		if (auto Exists = Inner_->Has(Sha256); Exists.Ok() && *Exists)
		{
			LastPut_.Compression = CompressionKind::None; // unknown; SyncEngine may omit encoding
			LastPut_.UncompressedSize = H->Size;
			LastPut_.CompressedSize = H->Size;
			return {};
		}

		const std::string StagingParent = file::Parent(LocalPath);
		const std::string Pkg = file::TempSibling(file::Join(StagingParent.empty() ? "." : StagingParent, Sha256), "swob");
		struct Clean { std::string P; ~Clean() { if (!P.empty()) (void)file::Remove(P); } } Guard{Pkg};

		auto Packed = PackSaveObject(LocalPath, Pkg, Cfg_.Compress);
		if (!Packed)
		{
			// Fall back to raw Put so hosting still works.
			LastPut_.Compression = CompressionKind::None;
			LastPut_.UncompressedSize = H->Size;
			LastPut_.CompressedSize = H->Size;
			return Inner_->Put(Sha256, LocalPath);
		}
		LastStats_ = *Packed;
		LastPut_.Compression = Packed->Kind;
		LastPut_.CompressionVersion = 1;
		LastPut_.UncompressedSize = Packed->UncompressedSize;
		LastPut_.CompressedSize = Packed->CompressedSize;
		LastPut_.CompressedSha256 = Packed->CompressedSha256;

		// Real Satisfactory .sav files are already zlib-chunked; outer compression
		// often saves <5%. Keep those as legacy raw objects for max compatibility.
		if (Packed->Kind == CompressionKind::None || Packed->bSkippedPoorRatio)
		{
			LastPut_.Compression = CompressionKind::None;
			LastPut_.CompressedSize = H->Size;
			LastPut_.CompressedSha256.clear();
			return Inner_->Put(Sha256, LocalPath);
		}

		// Inner stores must accept package bytes under the logical id (no content-hash check).
		SW_TRY(Inner_->PutBlob(Sha256, Pkg));
		return {};
	}

	Status EncodingObjectStore::Get(const std::string& Sha256, const std::string& DestPath)
	{
		const std::string StagingParent = file::Parent(DestPath);
		const std::string Tmp = file::TempSibling(file::Join(StagingParent.empty() ? "." : StagingParent, Sha256), "obj");
		struct Clean { std::string P; ~Clean() { if (!P.empty()) (void)file::Remove(P); } } Guard{Tmp};
		SW_TRY(Inner_->Get(Sha256, Tmp));
		SaveObjectEncoding Enc;
		SW_TRY(UnpackSaveObject(Tmp, DestPath, Sha256, 0, &Enc));
		return {};
	}

	Status EncodingObjectStore::Remove(const std::string& Sha256)
	{
		return Inner_->Remove(Sha256);
	}

	Result<std::vector<std::string>> EncodingObjectStore::List()
	{
		return Inner_->List();
	}

	std::string EncodingObjectStore::Describe() const
	{
		return "encoding(" + Inner_->Describe() + ")";
	}
}
