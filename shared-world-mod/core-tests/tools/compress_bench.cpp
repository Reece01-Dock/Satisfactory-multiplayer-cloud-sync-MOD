// Compression benchmark for Satisfactory .sav files (and synthetic multi-chunk saves).
// Usage: sw_compress_bench [path.sav ...]
// Without args: uses SW_TESTDATA_DIR conformance + optional SW_BENCH_SAVES env (`;`-separated).

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "SharedWorldCore/Save/SaveFile.h"
#include "SharedWorldCore/Storage/Compress.h"
#include "SharedWorldCore/Util/FileUtil.h"

using namespace sw;

namespace
{
	std::string FormatBytes(int64_t N)
	{
		char Buf[64];
		if (N >= 1 << 20) std::snprintf(Buf, sizeof(Buf), "%.2f MB", N / (1024.0 * 1024.0));
		else if (N >= 1024) std::snprintf(Buf, sizeof(Buf), "%.1f KB", N / 1024.0);
		else std::snprintf(Buf, sizeof(Buf), "%lld B", static_cast<long long>(N));
		return Buf;
	}

	void BenchOne(const std::string& Path)
	{
		auto Sz = file::Size(Path);
		if (!Sz)
		{
			std::printf("Skip (missing): %s\n", Path.c_str());
			return;
		}
		std::printf("\nSave: %s\nPath: %s\n\nRaw:\n%s\n", FormatBytes(*Sz).c_str(), Path.c_str(), FormatBytes(*Sz).c_str());

		struct Case { CompressionKind Kind; int Level; const char* Label; };
		const Case Cases[] = {
			{CompressionKind::Zstd, 1, "zstd-1"},
			{CompressionKind::Zstd, 3, "zstd-3"},
			{CompressionKind::Zstd, 5, "zstd-5"},
			{CompressionKind::Zstd, 10, "zstd-10"},
			{CompressionKind::Zlib, 1, "zlib-1"},
			{CompressionKind::Zlib, 6, "zlib-6"},
			{CompressionKind::Zlib, 9, "zlib-9"},
		};

		const std::string TmpDir = file::Join(file::Parent(Path).empty() ? "." : file::Parent(Path), "_sw_bench_tmp");
		(void)file::CreateDirectories(TmpDir);

		for (const Case& C : Cases)
		{
			CompressOptions Opt;
			Opt.Kind = C.Kind;
			Opt.Level = C.Level;
			Opt.MinRatioGain = -1.0;
			const std::string Out = file::Join(TmpDir, std::string(C.Label) + ".bin");
			const std::string Round = file::Join(TmpDir, std::string(C.Label) + ".sav");
			(void)file::Remove(Out);
			(void)file::Remove(Round);

			auto Comp = CompressFile(Path, Out, Opt);
			if (!Comp)
			{
				std::printf("\n%s:\nFAILED compress: %s\n", C.Label, Comp.Err().Describe().c_str());
				continue;
			}
			const auto T0 = std::chrono::steady_clock::now();
			auto Dec = DecompressFile(Out, Round, Comp->Kind, Comp->UncompressedSize);
			const auto DecMs = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - T0).count();
			if (!Dec)
			{
				std::printf("\n%s:\nFAILED decompress: %s\n", C.Label, Dec.Err().Describe().c_str());
				continue;
			}
			const double Pct = Comp->UncompressedSize > 0
				? (100.0 * static_cast<double>(Comp->CompressedSize) / static_cast<double>(Comp->UncompressedSize))
				: 100.0;
			std::printf("\n%s:\n%s\n%.1f%% of raw\ncompress %lldms\ndecompress %lldms\n",
				C.Label,
				FormatBytes(Comp->CompressedSize).c_str(),
				Pct,
				static_cast<long long>(Comp->CompressMs),
				static_cast<long long>(DecMs > 0 ? DecMs : Dec->DecompressMs));
			(void)file::Remove(Out);
			(void)file::Remove(Round);
		}
	}

	std::string MakeSynthetic(const std::string& Dir, size_t BodyBytes, const char* Name)
	{
		std::string Body(BodyBytes, '\0');
		for (size_t i = 0; i < Body.size(); ++i) Body[i] = static_cast<char>((i * 131) ^ (i >> 3));
		const std::string P = file::Join(Dir, Name);
		(void)file::Remove(P);
		(void)file::CreateExclusive(P, save::BuildSynthetic("Bench", Body));
		return P;
	}
}

int main(int argc, char** argv)
{
	std::vector<std::string> Paths;
	for (int i = 1; i < argc; ++i) Paths.push_back(argv[i]);

	if (const char* Env = std::getenv("SW_BENCH_SAVES"))
	{
		std::string S = Env;
		size_t Start = 0;
		while (Start < S.size())
		{
			size_t End = S.find(';', Start);
			if (End == std::string::npos) End = S.size();
			if (End > Start) Paths.push_back(S.substr(Start, End - Start));
			Start = End + 1;
		}
	}

	if (Paths.empty())
	{
#ifdef SW_TESTDATA_DIR
		Paths.push_back(std::string(SW_TESTDATA_DIR) + "/conformance/valid_random_multichunk.sav");
#endif
		const char* Local = std::getenv("LOCALAPPDATA");
		if (Local)
		{
			Paths.push_back(std::string(Local) + "/FactoryGame/Saved/SaveGames/76561198137203279/SharedWorld_test-test-autosave-0-ddbb7f.sav");
			Paths.push_back(std::string(Local) + "/FactoryGame/Saved/SaveGames/76561198137203279/test_autosave_1.sav");
			Paths.push_back(std::string(Local) + "/SatisfactorySharedWorld/worlds/cheatogame-cheatogame-autosave-0-bb64d5/backups/cheatogame-cheatogame-autosave-0-bb64d5/20260929T161310.825Z_pre-download-r1.sav");
		}
	}

	std::printf("=== Shared Worlds save compression benchmark ===\n");
	std::printf("Note: Satisfactory .sav bodies are already zlib-chunked; outer compression may gain little.\n");

	for (const std::string& P : Paths)
	{
		if (file::Exists(P)) BenchOne(P);
	}

	// Synthetic larger saves (valid multi-chunk format) for scale.
	const std::string SynthDir = file::Join(
#ifdef SW_TESTDATA_DIR
		std::string(SW_TESTDATA_DIR)
#else
		"."
#endif
		, "_bench_synth");
	(void)file::CreateDirectories(SynthDir);
	for (size_t MB : {2ull, 10ull, 48ull})
	{
		const std::string Name = "synth_" + std::to_string(MB) + "mb.sav";
		const std::string P = MakeSynthetic(SynthDir, MB * 1024 * 1024, Name.c_str());
		BenchOne(P);
		(void)file::Remove(P);
	}

	std::printf("\nDone.\n");
	return 0;
}
