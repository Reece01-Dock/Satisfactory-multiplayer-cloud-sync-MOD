#include "SharedWorldCore/Util/FileUtil.h"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <random>
#include <system_error>

#include "SharedWorldCore/Util/Sha256.h"

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace sw::file
{
	namespace
	{
		fs::path ToPath(const std::string& Utf8)
		{
			return fs::path(std::u8string(reinterpret_cast<const char8_t*>(Utf8.data()), Utf8.size()));
		}

		std::string FromPath(const fs::path& P)
		{
			const std::u8string U = P.u8string();
			return std::string(reinterpret_cast<const char*>(U.data()), U.size());
		}

		FILE* Open(const std::string& Path, const char* Mode)
		{
#if defined(_WIN32)
			std::wstring WMode(Mode, Mode + std::char_traits<char>::length(Mode));
			FILE* F = nullptr;
			const errno_t Err = _wfopen_s(&F, ToPath(Path).c_str(), WMode.c_str());
			if (Err != 0)
			{
				errno = Err; // callers inspect errno (EEXIST for "x" mode)
				return nullptr;
			}
			return F;
#else
			return std::fopen(Path.c_str(), Mode);
#endif
		}

		bool FlushToDisk(FILE* F)
		{
			if (std::fflush(F) != 0)
			{
				return false;
			}
#if defined(_WIN32)
			return _commit(_fileno(F)) == 0;
#else
			return fsync(fileno(F)) == 0;
#endif
		}

		Error IoError(const std::string& What, const std::string& Path)
		{
			return MakeError(ErrorCode::Io, What + " '" + Path + "'");
		}

		Error IoError(const std::string& What, const std::string& Path, const std::error_code& Ec)
		{
			return MakeError(ErrorCode::Io, What + " '" + Path + "': " + Ec.message());
		}

		Status WriteAndClose(FILE* F, const std::string& Path, const std::string& Data)
		{
			const bool bWrote = Data.empty() || std::fwrite(Data.data(), 1, Data.size(), F) == Data.size();
			const bool bFlushed = bWrote && FlushToDisk(F);
			const bool bClosed = std::fclose(F) == 0;
			if (!bWrote || !bFlushed || !bClosed)
			{
				return IoError("could not write", Path);
			}
			return {};
		}
	}

	std::string TempSibling(const std::string& Path, const std::string& Tag)
	{
		static std::atomic<uint64_t> Counter{0};
		std::random_device Rd;
		const uint64_t N = (uint64_t(Rd()) << 32) ^ Counter.fetch_add(1);
		char Buf[32];
		std::snprintf(Buf, sizeof(Buf), "%016llx", static_cast<unsigned long long>(N));
		return Path + "." + Tag + "-" + Buf;
	}

	Result<std::string> ReadAll(const std::string& Path, int64_t MaxBytes)
	{
		FILE* F = Open(Path, "rb");
		if (!F)
		{
			std::error_code Ec;
			if (!fs::exists(ToPath(Path), Ec))
			{
				return MakeError(ErrorCode::NotFound, "file not found '" + Path + "'");
			}
			return IoError("could not open", Path);
		}
		std::string Out;
		char Buf[65536];
		size_t N;
		while ((N = std::fread(Buf, 1, sizeof(Buf), F)) > 0)
		{
			Out.append(Buf, N);
			if (static_cast<int64_t>(Out.size()) > MaxBytes)
			{
				std::fclose(F);
				return MakeError(ErrorCode::Invalid, "file too large '" + Path + "'");
			}
		}
		const bool bErr = std::ferror(F) != 0;
		std::fclose(F);
		if (bErr)
		{
			return IoError("could not read", Path);
		}
		return Out;
	}

	Status WriteAtomic(const std::string& Path, const std::string& Data)
	{
		const std::string Tmp = TempSibling(Path, "tmp");
		FILE* F = Open(Tmp, "wbx");
		if (!F)
		{
			return IoError("could not create", Tmp);
		}
		Status S = WriteAndClose(F, Tmp, Data);
		if (!S)
		{
			(void)Remove(Tmp);
			return S;
		}
		return ReplaceWith(Tmp, Path);
	}

	Status CreateExclusive(const std::string& Path, const std::string& Data)
	{
		errno = 0;
		FILE* F = Open(Path, "wbx");
		if (!F)
		{
			// Decide from errno, not a later Exists() check: the file may be
			// removed in between (e.g. a lock released by another process).
			if (errno == EEXIST)
			{
				return MakeError(ErrorCode::AlreadyExists, "already exists '" + Path + "'");
			}
			return IoError("could not create", Path);
		}
		return WriteAndClose(F, Path, Data);
	}

	Status CopyExclusive(const std::string& Src, const std::string& Dst)
	{
		FILE* In = Open(Src, "rb");
		if (!In)
		{
			return Exists(Src) ? Status(IoError("could not open", Src)) : Status(MakeError(ErrorCode::NotFound, "file not found '" + Src + "'"));
		}
		errno = 0;
		FILE* Out = Open(Dst, "wbx");
		if (!Out)
		{
			std::fclose(In);
			return errno == EEXIST ? Status(MakeError(ErrorCode::AlreadyExists, "already exists '" + Dst + "'")) : Status(IoError("could not create", Dst));
		}
		char Buf[1 << 16];
		size_t N;
		bool bOk = true;
		while ((N = std::fread(Buf, 1, sizeof(Buf), In)) > 0)
		{
			if (std::fwrite(Buf, 1, N, Out) != N)
			{
				bOk = false;
				break;
			}
		}
		bOk = bOk && std::ferror(In) == 0 && FlushToDisk(Out);
		std::fclose(In);
		bOk = (std::fclose(Out) == 0) && bOk;
		if (!bOk)
		{
			(void)Remove(Dst);
			return IoError("could not copy to", Dst);
		}
		return {};
	}

	Result<HashResult> Hash(const std::string& Path, int64_t MaxBytes)
	{
		FILE* F = Open(Path, "rb");
		if (!F)
		{
			return Exists(Path) ? IoError("could not open", Path) : MakeError(ErrorCode::NotFound, "file not found '" + Path + "'");
		}
		Sha256 H;
		HashResult R;
		std::vector<char> Buf(1 << 20);
		size_t N;
		while ((N = std::fread(Buf.data(), 1, Buf.size(), F)) > 0)
		{
			H.Update(Buf.data(), N);
			R.Size += static_cast<int64_t>(N);
			if (R.Size > MaxBytes)
			{
				std::fclose(F);
				return MakeError(ErrorCode::Invalid, "file too large '" + Path + "'");
			}
		}
		const bool bErr = std::ferror(F) != 0;
		std::fclose(F);
		if (bErr)
		{
			return IoError("could not read", Path);
		}
		R.Sha256 = Sha256::ToHex(H.Finish());
		return R;
	}

	Status FlushExisting(const std::string& Path)
	{
		FILE* F = Open(Path, "r+b"); // writable handle, see header comment
		if (!F)
		{
			return IoError("could not open for flush", Path);
		}
		const bool bOk = FlushToDisk(F);
		std::fclose(F);
		return bOk ? Status() : Status(IoError("could not flush", Path));
	}

	Status ReplaceWith(const std::string& Temp, const std::string& Target)
	{
		std::error_code Ec;
		fs::rename(ToPath(Temp), ToPath(Target), Ec);
		if (Ec)
		{
			(void)Remove(Temp);
			return IoError("could not replace", Target, Ec);
		}
		return {};
	}

	Status LinkExclusive(const std::string& Existing, const std::string& NewPath)
	{
		std::error_code Ec;
		fs::create_hard_link(ToPath(Existing), ToPath(NewPath), Ec);
		if (Ec)
		{
			if (Exists(NewPath))
			{
				return MakeError(ErrorCode::AlreadyExists, "already exists '" + NewPath + "'");
			}
			return IoError("could not link", NewPath, Ec);
		}
		return {};
	}

	bool Exists(const std::string& Path)
	{
		std::error_code Ec;
		return fs::exists(ToPath(Path), Ec);
	}

	Result<int64_t> Size(const std::string& Path)
	{
		std::error_code Ec;
		const auto S = fs::file_size(ToPath(Path), Ec);
		if (Ec)
		{
			return Exists(Path) ? IoError("could not stat", Path, Ec) : MakeError(ErrorCode::NotFound, "file not found '" + Path + "'");
		}
		return static_cast<int64_t>(S);
	}

	Result<double> AgeSeconds(const std::string& Path)
	{
		std::error_code Ec;
		const auto T = fs::last_write_time(ToPath(Path), Ec);
		if (Ec)
		{
			return MakeError(ErrorCode::NotFound, "file not found '" + Path + "'");
		}
		return std::chrono::duration<double>(fs::file_time_type::clock::now() - T).count();
	}

	Status Remove(const std::string& Path)
	{
		std::error_code Ec;
		fs::remove(ToPath(Path), Ec);
		return Ec ? Status(IoError("could not remove", Path, Ec)) : Status();
	}

	Status RemoveAll(const std::string& Path)
	{
		std::error_code Ec;
		fs::remove_all(ToPath(Path), Ec);
		return Ec ? Status(IoError("could not remove", Path, Ec)) : Status();
	}

	Status CreateDirectories(const std::string& Path)
	{
		std::error_code Ec;
		fs::create_directories(ToPath(Path), Ec);
		return Ec ? Status(IoError("could not create directory", Path, Ec)) : Status();
	}

	Result<std::vector<std::string>> ListNames(const std::string& Dir)
	{
		std::vector<std::string> Out;
		std::error_code Ec;
		if (!fs::exists(ToPath(Dir), Ec))
		{
			return Out;
		}
		for (fs::directory_iterator It(ToPath(Dir), Ec), End; !Ec && It != End; It.increment(Ec))
		{
			Out.push_back(FromPath(It->path().filename()));
		}
		if (Ec)
		{
			return IoError("could not list", Dir, Ec);
		}
		return Out;
	}

	std::string Join(const std::string& A, const std::string& B) { return FromPath(ToPath(A) / ToPath(B)); }
	std::string Parent(const std::string& Path) { return FromPath(ToPath(Path).parent_path()); }
	std::string FileName(const std::string& Path) { return FromPath(ToPath(Path).filename()); }
}
