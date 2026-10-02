#pragma once
// IObjectStore that sends a world's save files to wherever world.json says they live.
//
// Most worlds keep their saves in the repository's own object store (GitHub releases / folder). A world created on an
// rclone provider records that in world.json (WorldInfo::SaveStorage); each PC links its own connection to it
// (WorldEntry::SaveRemote). The router reads world.json once, then:
//   - no saveStorage            -> the default store (unchanged behaviour)
//   - saveStorage + linked here -> the linked rclone store, <SaveRemote>/<worldId>
//   - saveStorage, not linked   -> a clear "link it to host" error. Joining a hosted session never touches the object
//                                  store, so an unlinked player can still play; only hosting needs the saves.
// Only the object store changes. The repository, leases and revision fencing stay exactly where they were.

#include <functional>
#include <memory>
#include <mutex>
#include <string>

#include "SharedWorldCore/Storage/Storage.h"

namespace sw
{
	/** Every "not linked" error contains this text (the UI uses it to offer linking). */
	inline constexpr const char* SaveStorageNotLinkedPhrase = "on this PC to host it";

	struct SaveStorageRouterConfig
	{
		std::string WorldId;
		std::shared_ptr<IWorldRepository> Repository;
		/** The repository's own store (what every world used before rclone). */
		std::shared_ptr<IObjectStore> Default;
		/** This PC's link (remote:folder) or empty. */
		std::string SaveRemote;
		/** Cached marker from settings, used only until world.json has been read (e.g. while a world is being created). */
		std::string KnownBackend;
		std::string KnownLabel;
		/** Opens an (already encoded) store on an rclone path. */
		std::function<Result<std::shared_ptr<IObjectStore>>(const std::string& Fs)> OpenRemote;
		std::function<void(const std::string& Backend, const std::string& Label)> OnSeen;
	};

	class SaveStorageRouter final : public IObjectStore
	{
	public:
		explicit SaveStorageRouter(SaveStorageRouterConfig Config);

		Result<bool> Has(const std::string& Sha256) override;
		Status Put(const std::string& Sha256, const std::string& LocalPath) override;
		Status Get(const std::string& Sha256, const std::string& DestPath) override;
		Status Remove(const std::string& Sha256) override;
		Result<std::vector<std::string>> List() override;
		std::string Describe() const override;

	private:
		/** The store to use now, or why there is none. Successful answers are cached; failures are retried. */
		Result<std::shared_ptr<IObjectStore>> Resolve();

		SaveStorageRouterConfig Cfg;
		mutable std::mutex Mutex;
		std::shared_ptr<IObjectStore> Resolved;
		std::string ResolvedName;
	};
}
