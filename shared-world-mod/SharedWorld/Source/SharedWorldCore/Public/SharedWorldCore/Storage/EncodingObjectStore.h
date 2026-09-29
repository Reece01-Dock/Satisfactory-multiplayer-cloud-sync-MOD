#pragma once
// IObjectStore decorator: stores SWOB packages (optional zstd/zlib) under the
// logical uncompressed SHA-256 object id. Get always restores a raw .sav.

#include <memory>

#include "SharedWorldCore/Storage/Compress.h"
#include "SharedWorldCore/Storage/SaveObject.h"
#include "SharedWorldCore/Storage/Storage.h"

namespace sw
{
	struct EncodingObjectStoreConfig
	{
		CompressOptions Compress;
		bool bEnabled = true;
	};

	class EncodingObjectStore final : public IObjectStore
	{
	public:
		EncodingObjectStore(std::shared_ptr<IObjectStore> Inner, EncodingObjectStoreConfig Cfg = {});

		Result<bool> Has(const std::string& Sha256) override;
		Status Put(const std::string& Sha256, const std::string& LocalPath) override;
		Status Get(const std::string& Sha256, const std::string& DestPath) override;
		Status Remove(const std::string& Sha256) override;
		Result<std::vector<std::string>> List() override;
		std::string Describe() const override;

		IObjectStore& Inner() { return *Inner_; }
		const EncodingObjectStoreConfig& Config() const { return Cfg_; }
		const SaveObjectEncoding& LastPutEncoding() const { return LastPut_; }
		const CompressStats& LastPutStats() const { return LastStats_; }
		const SaveObjectEncoding* PeekLastPutEncoding() const override { return &LastPut_; }
		const CompressStats* PeekLastPutStats() const override { return &LastStats_; }

	private:
		std::shared_ptr<IObjectStore> Inner_;
		EncodingObjectStoreConfig Cfg_;
		SaveObjectEncoding LastPut_;
		CompressStats LastStats_;
	};
}
