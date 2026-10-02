#include "SharedWorldCore/Storage/LogRepository.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <set>
#include <thread>

#include "SharedWorldCore/Util/Json.h"
#include "SharedWorldCore/Util/Random.h"

namespace sw
{
	namespace
	{
		constexpr const char* LogDir = "log";
		constexpr size_t MaxEntryBytes = 4 * 1024 * 1024;

		std::string SeqName(int64_t Seq)
		{
			char Buf[32];
			std::snprintf(Buf, sizeof(Buf), "%012lld", static_cast<long long>(Seq));
			return Buf;
		}

		/** "000000000084" -> 84, anything else -> -1 (foreign files in the folder are ignored). */
		int64_t ParseSeqName(const std::string& Name)
		{
			if (Name.size() != 12) return -1;
			int64_t V = 0;
			for (char C : Name)
			{
				if (C < '0' || C > '9') return -1;
				V = V * 10 + (C - '0');
			}
			return V;
		}

		/** "84.ab12..." -> 84 */
		int64_t SeqOfCommitId(const std::string& Id)
		{
			const size_t Dot = Id.find('.');
			if (Dot == std::string::npos || Dot == 0 || Dot > 18) return -1;
			int64_t V = 0;
			for (size_t i = 0; i < Dot; ++i)
			{
				if (Id[i] < '0' || Id[i] > '9') return -1;
				V = V * 10 + (Id[i] - '0');
			}
			return V;
		}

		/** Deterministic winner among candidates for one sequence number: earliest server time, then store id. */
		bool Earlier(const LogRepository::Entry& A, const LogRepository::Entry& B)
		{
			if (A.Stored.ServerTime != B.Stored.ServerTime) return A.Stored.ServerTime < B.Stored.ServerTime;
			return A.Id < B.Id;
		}
	}

	LogRepository::LogRepository(std::shared_ptr<ILogStore> InStore, LogRepositoryConfig InConfig)
		: Store(std::move(InStore)), Cfg(std::move(InConfig))
	{
		if (!Cfg.Sleep)
		{
			Cfg.Sleep = [](TimeMs Ms) { std::this_thread::sleep_for(std::chrono::milliseconds(Ms)); };
		}
		if (!Cfg.NewWriterTag)
		{
			Cfg.NewWriterTag = []() { SystemRandom R; return R.Hex(8); };
		}
	}

	Result<std::shared_ptr<const LogRepository::Entry>> LogRepository::Load(const LogEntryInfo& Info)
	{
		{
			std::lock_guard<std::mutex> Lock(CacheMutex);
			auto It = CacheByStoredId.find(Info.Id);
			if (It != CacheByStoredId.end()) return It->second;
		}
		std::string Text;
		SW_ASSIGN(Text, Store->Read(LogDir, Info));
		if (Text.size() > MaxEntryBytes) return MakeError(ErrorCode::Corrupt, "log entry too large");
		json::ParseLimits Limits;
		Limits.MaxBytes = MaxEntryBytes;
		json::Value V;
		SW_ASSIGN(V, json::Parse(Text, Limits));
		auto E = std::make_shared<Entry>();
		SW_ASSIGN(E->Seq, json::GetInt(V, "seq"));
		SW_ASSIGN(E->Id, json::GetString(V, "id", 64));
		SW_ASSIGN(E->Parent, json::GetString(V, "parent", 64));
		SW_ASSIGN(E->Message, json::GetString(V, "message", 4096));
		SW_ASSIGN(E->Time, json::GetInt(V, "time"));
		if (E->Seq != ParseSeqName(Info.Name) || SeqOfCommitId(E->Id) != E->Seq)
		{
			return MakeError(ErrorCode::Corrupt, "log entry does not match its name");
		}
		const json::Value* Tree = V.Find("tree");
		if (!Tree || !Tree->IsObject()) return MakeError(ErrorCode::Corrupt, "log entry has no tree");
		for (const auto& [Path, Content] : Tree->AsObject())
		{
			if (!Content.IsString()) return MakeError(ErrorCode::Corrupt, "log entry tree value is not text");
			E->Tree.emplace(Path, Content.AsString());
		}
		E->Stored = Info;
		std::lock_guard<std::mutex> Lock(CacheMutex);
		CacheByStoredId[Info.Id] = E;
		return std::shared_ptr<const Entry>(E);
	}

	Result<std::shared_ptr<const LogRepository::Entry>> LogRepository::ResolveHead()
	{
		std::vector<LogEntryInfo> Infos;
		SW_ASSIGN(Infos, Store->List(LogDir));
		std::map<int64_t, std::vector<LogEntryInfo>> BySeq;
		for (LogEntryInfo& I : Infos)
		{
			const int64_t Seq = ParseSeqName(I.Name);
			if (Seq > 0) BySeq[Seq].push_back(std::move(I));
		}

		// Walk the chain from the oldest kept entry. At each sequence number the valid candidates are the entries
		// built on the previous winner; the earliest of them wins. A gap or no valid candidate ends the chain.
		std::shared_ptr<const Entry> Head;
		for (const auto& [Seq, Candidates] : BySeq)
		{
			if (Head && Seq != Head->Seq + 1) break;
			std::shared_ptr<const Entry> Winner;
			for (const LogEntryInfo& C : Candidates)
			{
				auto E = Load(C);
				if (!E)
				{
					if (E.Is(ErrorCode::NotFound)) continue; // removed by compaction meanwhile
					if (E.Is(ErrorCode::Corrupt) || E.Is(ErrorCode::Invalid)) continue; // never let one bad file block the world
					return E.Err();
				}
				const bool bLinks = Head ? ((*E)->Parent == Head->Id) : true;
				if (!bLinks) continue;
				if (!Winner || Earlier(**E, *Winner)) Winner = *E;
			}
			if (!Winner) break;
			Head = Winner;
		}
		if (!Head) return MakeError(ErrorCode::NotFound, "no Shared World here yet");
		return Head;
	}

	Result<std::shared_ptr<const LogRepository::Entry>> LogRepository::FindCommit(const std::string& CommitId)
	{
		const int64_t Seq = SeqOfCommitId(CommitId);
		if (Seq <= 0) return MakeError(ErrorCode::NotFound, "unknown commit");
		{
			std::lock_guard<std::mutex> Lock(CacheMutex);
			for (const auto& [StoredId, E] : CacheByStoredId)
			{
				if (E->Id == CommitId) return E;
			}
		}
		std::vector<LogEntryInfo> Infos;
		SW_ASSIGN(Infos, Store->List(LogDir));
		const std::string Name = SeqName(Seq);
		for (const LogEntryInfo& I : Infos)
		{
			if (I.Name != Name) continue;
			auto E = Load(I);
			if (E && (*E)->Id == CommitId) return *E;
		}
		return MakeError(ErrorCode::NotFound, "commit not found (it may have been compacted)");
	}

	Result<std::string> LogRepository::Head()
	{
		auto H = ResolveHead();
		if (!H) return H.Err();
		return (*H)->Id;
	}

	Result<std::string> LogRepository::ReadFile(const std::string& CommitId, const std::string& Path)
	{
		SW_TRY(ValidateRepoPath(Path));
		std::shared_ptr<const Entry> E;
		SW_ASSIGN(E, FindCommit(CommitId));
		auto It = E->Tree.find(Path);
		if (It == E->Tree.end()) return MakeError(ErrorCode::NotFound, "no such file: " + Path);
		return It->second;
	}

	Result<std::vector<std::string>> LogRepository::ListDirectory(const std::string& CommitId, const std::string& Dir)
	{
		if (!Dir.empty()) SW_TRY(ValidateRepoPath(Dir));
		std::shared_ptr<const Entry> E;
		SW_ASSIGN(E, FindCommit(CommitId));
		const std::string Prefix = Dir.empty() ? std::string() : Dir + "/";
		std::set<std::string> Names;
		for (const auto& [Path, Content] : E->Tree)
		{
			if (Path.compare(0, Prefix.size(), Prefix) != 0) continue;
			const std::string Rest = Path.substr(Prefix.size());
			const size_t Slash = Rest.find('/');
			Names.insert(Slash == std::string::npos ? Rest : Rest.substr(0, Slash));
		}
		return std::vector<std::string>(Names.begin(), Names.end());
	}

	Result<std::string> LogRepository::Commit(const std::string& ExpectedHead, const std::vector<FileChange>& Changes, const std::string& Message)
	{
		for (const FileChange& C : Changes) SW_TRY(ValidateRepoPath(C.Path));

		std::map<std::string, std::string> Tree;
		int64_t Seq = 1;
		std::string Parent;
		auto H = ResolveHead();
		if (H)
		{
			if ((*H)->Id != ExpectedHead) return MakeError(ErrorCode::Conflict, "the world changed");
			Tree = (*H)->Tree;
			Seq = (*H)->Seq + 1;
			Parent = (*H)->Id;
		}
		else if (H.Is(ErrorCode::NotFound))
		{
			if (!ExpectedHead.empty()) return MakeError(ErrorCode::Conflict, "the world changed");
		}
		else
		{
			return H.Err();
		}
		for (const FileChange& C : Changes)
		{
			if (C.Content) Tree[C.Path] = *C.Content;
			else Tree.erase(C.Path);
		}

		const std::string Id = std::to_string(Seq) + "." + Cfg.NewWriterTag();
		json::Value V;
		V.Set("v", 1);
		V.Set("seq", Seq);
		V.Set("id", Id);
		V.Set("parent", Parent);
		V.Set("message", Message.substr(0, 4000));
		V.Set("time", static_cast<int64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count()));
		json::Object TreeObj;
		for (const auto& [Path, Content] : Tree) TreeObj.emplace(Path, json::Value(Content));
		V.Set("tree", json::Value(std::move(TreeObj)));
		const std::string Text = json::Serialize(V);
		if (Text.size() > MaxEntryBytes) return MakeError(ErrorCode::Invalid, "world record too large");

		auto Created = Store->Create(LogDir, SeqName(Seq), Text);
		if (!Created)
		{
			if (Created.Is(ErrorCode::AlreadyExists)) return MakeError(ErrorCode::Conflict, "someone else changed the world first");
			if (Created.Is(ErrorCode::Network) || Created.Is(ErrorCode::Ambiguous))
			{
				return MakeError(ErrorCode::Ambiguous, "unsure whether the change was saved: " + Created.Err().Message);
			}
			return Created.Err();
		}

		if (!Store->ExclusiveCreate())
		{
			// Duplicate mode: give a competitor's earlier entry time to show up, then let the shared rule decide.
			Cfg.Sleep(Cfg.SettleMs);
			auto After = ResolveHead();
			if (!After) return MakeError(ErrorCode::Ambiguous, "could not confirm the change: " + After.Err().Message);
			// Walk back from the head to our sequence number: we won only if our entry is on the winning chain.
			std::shared_ptr<const Entry> Cur = *After;
			while (Cur && Cur->Seq > Seq)
			{
				auto P = FindCommit(Cur->Parent);
				if (!P) break;
				Cur = *P;
			}
			if (!Cur || Cur->Id != Id)
			{
				(void)Store->Delete(LogDir, *Created); // a losing entry is ignored by everyone anyway; tidy up
				return MakeError(ErrorCode::Conflict, "someone else changed the world first");
			}
		}
		Compact(Seq);
		return Id;
	}

	void LogRepository::Compact(int64_t HeadSeq)
	{
		if (Cfg.KeepEntries <= 0 || HeadSeq <= Cfg.KeepEntries || HeadSeq % 16 != 0) return;
		auto Infos = Store->List(LogDir);
		if (!Infos) return;
		const int64_t Cutoff = HeadSeq - Cfg.KeepEntries;
		for (const LogEntryInfo& I : *Infos)
		{
			const int64_t Seq = ParseSeqName(I.Name);
			if (Seq > 0 && Seq < Cutoff)
			{
				(void)Store->Delete(LogDir, I);
				std::lock_guard<std::mutex> Lock(CacheMutex);
				CacheByStoredId.erase(I.Id);
			}
		}
	}

	Result<std::vector<CommitInfo>> LogRepository::Log(const std::string& FromCommit, int MaxCount)
	{
		std::vector<CommitInfo> Out;
		std::string Cur = FromCommit;
		while (!Cur.empty() && static_cast<int>(Out.size()) < MaxCount)
		{
			auto E = FindCommit(Cur);
			if (!E)
			{
				if (E.Is(ErrorCode::NotFound) && !Out.empty()) break; // older history was compacted
				return E.Err();
			}
			Out.push_back(CommitInfo{(*E)->Id, (*E)->Parent, (*E)->Message, (*E)->Time});
			Cur = (*E)->Parent;
		}
		return Out;
	}

	std::string LogRepository::Describe() const
	{
		return "log repository on " + Store->Describe();
	}

	// ------------------------------------------------------------------ MemoryLogStore

	namespace
	{
		TimeMs SteadyNowMs()
		{
			return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
		}
	}

	Result<std::vector<LogEntryInfo>> MemoryLogStore::List(const std::string& Dir)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		std::vector<LogEntryInfo> Out;
		auto It = Dirs.find(Dir);
		if (It == Dirs.end()) return Out;
		const TimeMs Now = SteadyNowMs();
		for (const Stored& S : It->second)
		{
			if (S.Info.ServerTime + DelayMs <= Now) Out.push_back(S.Info); // simulated listing delay
		}
		return Out;
	}

	Result<std::string> MemoryLogStore::Read(const std::string& Dir, const LogEntryInfo& Entry)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		auto It = Dirs.find(Dir);
		if (It != Dirs.end())
		{
			for (const Stored& S : It->second)
			{
				if (S.Info.Id == Entry.Id) return S.Data;
			}
		}
		return MakeError(ErrorCode::NotFound, "no such entry");
	}

	Result<LogEntryInfo> MemoryLogStore::Create(const std::string& Dir, const std::string& Name, const std::string& Data)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		std::vector<Stored>& Entries = Dirs[Dir];
		if (bExclusiveMode)
		{
			for (const Stored& S : Entries)
			{
				if (S.Info.Name == Name) return MakeError(ErrorCode::AlreadyExists, "exists");
			}
		}
		Stored S;
		S.Info.Name = Name;
		S.Info.Id = bExclusiveMode ? Name : Name + "#" + std::to_string(NextId++);
		S.Info.ServerTime = SteadyNowMs();
		S.Data = Data;
		Entries.push_back(S);
		return S.Info;
	}

	Status MemoryLogStore::Delete(const std::string& Dir, const LogEntryInfo& Entry)
	{
		std::lock_guard<std::mutex> Lock(Mutex);
		auto It = Dirs.find(Dir);
		if (It == Dirs.end()) return {};
		auto& V = It->second;
		V.erase(std::remove_if(V.begin(), V.end(), [&](const Stored& S) { return S.Info.Id == Entry.Id; }), V.end());
		return {};
	}
}
