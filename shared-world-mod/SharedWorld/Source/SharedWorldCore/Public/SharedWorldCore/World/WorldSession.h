#pragma once
// The Shared World session engine: everything behind the PLAY button.
// Native successor of shared-world-helper/internal/world (same decisions,
// same invariants), plus migration, host-loss recovery and restore.
//
// Threading: public methods are called from the game thread and never
// block on I/O. Work runs on two serial worker queues (operations, and
// heartbeats so a long upload never starves the lease). Tick(), called
// from the game thread about once a second, drives heartbeats and polling.
// The UI reads View(), an immutable snapshot.
//
// The session only *decides*; the game layer carries out game actions
// (load save, join session, save game) when the view asks for them and
// reports back through the On...() events.

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "SharedWorldCore/HostHandshake/HostHandshake.h"
#include "SharedWorldCore/Lease/Lease.h"
#include "SharedWorldCore/Migration/Migration.h"
#include "SharedWorldCore/Sync/Sync.h"
#include "SharedWorldCore/Util/TaskQueue.h"
#include "SharedWorldCore/World/Compatibility.h"

namespace sw
{
	enum class SessionState
	{
		Idle,
		Checking,        // reading the shared world
		WaitingForHost,  // a host holds the lease but has not published its session yet
		WaitingForSession, // host lease live; session / HostReady not yet available
		CheckingHost,    // verifying the lease holder is reachable
		HostVerified,    // handshake ok; transitioning to join
		HostUnreachable, // probe failed; may JoinRetry while lease still live
		JoinReady,       // decision JOIN: the game should join View().Join
		Joining,         // game join sequence in flight
		JoinRetry,       // join/verify failed; short retry while lease live
		Joined,          // playing as a client; following the host
		Reconnecting,    // host connection lost: waiting for a new host or taking over
		RecoveringHost,  // UX: recovering after unreachable host / crash signals
		ElectingHost,    // selecting / acquiring a new host after lease expiry
		Acquiring,       // claiming the host lease
		Recovering,      // publishing unsynced / recovered progress
		Downloading,     // fetching and verifying the save
		ReadyToHost,     // save installed: the game should load View().SavePath
		StartingSession, // game world loading / session definition starting
		PublishingSession, // waiting for online session id
		Hosting,
		Uploading,       // checkpoint / final / migration upload
		Migrating,       // handing the world to View().Successor
		Releasing,
		Restoring,
		LeaseLost,       // another host took over: stop, progress kept as backup
		Error,
	};
	const char* ToString(SessionState S);

	enum class Decision { None, Host, Join };

	/** Player-facing error. Message is written for players; Detail is for diagnostics. */
	struct ErrorInfo
	{
		std::string Code;
		std::string Message;
		std::string Detail;
		bool bLocalSaveUnchanged = true;
		std::string BackupPath;
		int64_t CloudRevision = 0;
		int64_t LocalRevision = 0;
		bool bRetryable = false;
	};

	struct SessionView
	{
		std::string WorldId;
		SessionState State = SessionState::Idle;
		Decision TheDecision = Decision::None;
		std::string Message;
		std::vector<std::string> Steps; // recent notifications, oldest first
		std::optional<ErrorInfo> Error;
		/** One-shot popup for the player; clear with AcknowledgeNotice() after showing. */
		std::optional<std::string> PlayerNotice;
		std::string HostName;
		std::optional<JoinInfo> Join;
		std::string SavePath;           // for ReadyToHost
		int64_t Generation = 0;
		int64_t Revision = 0;
		std::optional<Identity> Successor; // for Migrating
		uint64_t Sequence = 0;          // increments on every change
	};

	enum class SaveKind { Checkpoint, Final, Migration };

	struct SessionConfig
	{
		Identity Me;
		/** From UFGSaveSystem::GetSaveDirectoryPath() - never guessed. */
		std::string SaveDirectory;
		std::string SaveName; // e.g. SharedWorld_<worldId>
		LocalVersions Versions;
		TimeMs HeartbeatInterval = Seconds(20);
		TimeMs PollInterval = Seconds(3);
		TimeMs JoinWaitTimeout = Minutes(3);
		/** How long to keep probing an unreachable host before waiting on lease expiry. */
		TimeMs HostVerifyTimeout = Seconds(45);
		TimeMs HostVerifyProbeTimeout = Seconds(5);
		TimeMs JoinRetryInterval = Seconds(3);
		/** Delay per takeover rank after a host crash (rank 0 goes first). */
		TimeMs TakeoverStagger = Seconds(5);
		UploadOptions Upload;
		/**
		 * Optional reachability probe. When null, clients still require
		 * Lease.bHostReady / IsJoinable before JoinReady, but skip the live handshake.
		 */
		std::shared_ptr<IHostVerifier> HostVerifier;
	};

	class WorldSession
	{
	public:
		WorldSession(std::shared_ptr<LeaseManager> InLeases, std::shared_ptr<SyncEngine> InSync, SessionConfig InConfig);
		~WorldSession();
		WorldSession(const WorldSession&) = delete;
		WorldSession& operator=(const WorldSession&) = delete;

		SessionView View() const;
		const SessionConfig& Config() const { return Cfg; }

		// ---- menu actions
		/** "Take me into this Shared World": decides HOST or JOIN. */
		void Play();
		/** Stop waiting for a host, or give the world back before it loaded. */
		void Cancel();
		/** Acknowledge an error, a finished join, or a lost lease. */
		void Dismiss();
		/** Clear View().PlayerNotice after the game has shown it. */
		void AcknowledgeNotice();
		/** Restore revision From as a new revision (needs RestoreRevision permission). */
		void Restore(int64_t FromRevision);

		// ---- game events: host
		/** The world is loaded as host; Join is the published session (nullopt: friends list only). */
		void OnHostingStarted(const std::optional<JoinInfo>& Join);
		/** Game has begun loading the SharedWorld save (after ReadyToHost). */
		void OnHostLoadStarted();
		/** Online session id is being sought / publish in progress. */
		void OnHostPublishingSession();
		/** UFGSaveSystem::SaveGame completed for SaveName. */
		void OnSaveCompleted(SaveKind Kind);
		/** The host's world ended without a final save (exit to menu / quit). */
		void OnWorldEnded();
		/**
		 * Process is quitting: do not start long HTTP uploads (Curl/HTTP die mid-exit and hang).
		 * Clears local lease tracking; cloud lease expires. Prefer a proper leave-to-menu so a final upload runs.
		 */
		void AbandonOnProcessExit();
		/** Asks the game to save for a planned migration to Successor (game then calls OnSaveCompleted(Migration)). */
		void RequestMigration(const Identity& Successor);
		void SetPlayers(std::vector<SessionPlayer> Players);

		// ---- game events: client
		void OnJoinedAsClient();
		/** The game layer started the join sequence for View().Join. */
		void OnJoinStarted();
		/** The game could not join View().Join. */
		void OnJoinFailed(const std::string& Reason);
		void OnHostConnectionLost();
		/**
		 * The player left the host's game on purpose (quit to menu). Stops
		 * following the world, so this PC never takes over hosting in the
		 * background after the host leaves.
		 */
		void OnLeftAsClient();

		/** Drives heartbeats and polling. Call ~1/s from the game thread. */
		void Tick(TimeMs Now);

		/** Blocks until both worker queues are idle (tests / shutdown). */
		void WaitIdle();

	private:
		// All Do* functions run on a worker queue.
		void DoCheckAndDecide();
		void DoVerifyHost(StateSnapshot Snap);
		void DoHost(const StateSnapshot& Snap);
		void DoFollowHost();
		void DoReconnect();
		void DoUpload(SaveKind Kind, std::optional<Identity> Successor);
		void DoRelease(const std::string& DoneMessage, std::optional<Identity> Successor);
		void DoHeartbeat();
		void DoRestore(int64_t FromRevision);

		void Set(SessionState S, const std::string& Message);
		void Note(const std::string& Message);
		void Fail(ErrorInfo E);
		void FailFrom(const Error& E, const std::string& Code, const std::string& Message, bool bRetryable);
		void LoseLease();
		void AbortHosting(ErrorInfo E);
		void PersistActiveLease(const std::optional<LeaseToken>& Token);
		std::optional<std::string> PermissionProblem(const std::string& CommitId, Permission P);
		bool IsLeaseState(SessionState S) const;
		void EnterJoinPath(const Lease& L, int64_t HeadRevision, const std::string& Message);
		void ScheduleHostVerify(const StateSnapshot& Snap);

		std::shared_ptr<LeaseManager> Leases;
		std::shared_ptr<SyncEngine> Sync;
		SessionConfig Cfg;

		mutable std::mutex Mutex;
		SessionView Current;
		std::optional<LeaseToken> Token;
		std::optional<JoinInfo> PublishedJoin;
		std::vector<SessionPlayer> Players;
		std::optional<Identity> PendingSuccessor;
		int64_t FollowGeneration = 0;     // host generation we joined
		std::vector<SessionPlayer> LastSeenPlayers;
		TimeMs LastHeartbeat = 0;
		TimeMs LastPoll = 0;
		TimeMs WaitDeadline = 0;
		TimeMs TakeoverNotBefore = 0;
		TimeMs ConnectionLostAt = 0;
		TimeMs HostVerifyDeadline = 0;
		int HostVerifyAttempts = 0;
		std::atomic<bool> bPollInFlight{false};
		std::atomic<bool> bHeartbeatInFlight{false};
		std::atomic<bool> bVerifyInFlight{false};

		SerialQueue Ops;
		SerialQueue Heartbeats;
	};

	/** Main-menu status for one world, derived from the shared state. */
	enum class WorldStatus { Available, Starting, Online, Saving, Stopping, Migrating, Recoverable, NoSave, Unreachable, NotCreated };
	const char* ToString(WorldStatus S);

	struct WorldSummary
	{
		std::string WorldId;
		std::string Name;
		WorldStatus Status = WorldStatus::Unreachable;
		std::string StatusText;
		std::string HostName;
		std::vector<SessionPlayer> Players;
		int64_t Revision = 0;
		int64_t Generation = 0;
		TimeMs LastPlayedAt = 0;
		std::string LastHostName;
		TimeMs UpdatedAt = 0;
		std::string Problem; // diagnostics when Unreachable
	};

	/** Lightweight metadata read (no save transfer): safe to poll from the menu. */
	WorldSummary Summarize(LeaseManager& Leases);
}
