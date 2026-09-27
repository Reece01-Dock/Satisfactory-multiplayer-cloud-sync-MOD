#include "SharedWorldCore/Util/Time.h"

#include <chrono>
#include <cstdio>

namespace sw
{
	TimeMs SystemClock::Now() const
	{
		using namespace std::chrono;
		return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
	}

	namespace
	{
		// Howard Hinnant's civil-date algorithms (public domain).
		int64_t DaysFromCivil(int64_t Y, unsigned M, unsigned D)
		{
			Y -= M <= 2;
			const int64_t Era = (Y >= 0 ? Y : Y - 399) / 400;
			const unsigned Yoe = static_cast<unsigned>(Y - Era * 400);
			const unsigned Doy = (153 * (M + (M > 2 ? -3 : 9)) + 2) / 5 + D - 1;
			const unsigned Doe = Yoe * 365 + Yoe / 4 - Yoe / 100 + Doy;
			return Era * 146097 + static_cast<int64_t>(Doe) - 719468;
		}

		void CivilFromDays(int64_t Z, int64_t& Y, unsigned& M, unsigned& D)
		{
			Z += 719468;
			const int64_t Era = (Z >= 0 ? Z : Z - 146096) / 146097;
			const unsigned Doe = static_cast<unsigned>(Z - Era * 146097);
			const unsigned Yoe = (Doe - Doe / 1460 + Doe / 36524 - Doe / 146096) / 365;
			Y = static_cast<int64_t>(Yoe) + Era * 400;
			const unsigned Doy = Doe - (365 * Yoe + Yoe / 4 - Yoe / 100);
			const unsigned Mp = (5 * Doy + 2) / 153;
			D = Doy - (153 * Mp + 2) / 5 + 1;
			M = Mp < 10 ? Mp + 3 : Mp - 9;
			Y += M <= 2;
		}

		bool Digits(std::string_view S, size_t Pos, size_t N, int& Out)
		{
			if (Pos + N > S.size()) return false;
			Out = 0;
			for (size_t i = 0; i < N; ++i)
			{
				const char C = S[Pos + i];
				if (C < '0' || C > '9') return false;
				Out = Out * 10 + (C - '0');
			}
			return true;
		}
	}

	std::string FormatTime(TimeMs T)
	{
		int64_t Days = T / 86400000;
		int64_t Rem = T % 86400000;
		if (Rem < 0)
		{
			Rem += 86400000;
			--Days;
		}
		int64_t Y;
		unsigned M, D;
		CivilFromDays(Days, Y, M, D);
		char Buf[40];
		std::snprintf(Buf, sizeof(Buf), "%04lld-%02u-%02uT%02lld:%02lld:%02lld.%03lldZ", static_cast<long long>(Y), M, D,
			static_cast<long long>(Rem / 3600000), static_cast<long long>((Rem / 60000) % 60),
			static_cast<long long>((Rem / 1000) % 60), static_cast<long long>(Rem % 1000));
		return Buf;
	}

	Result<TimeMs> ParseTime(std::string_view S)
	{
		auto Bad = [&]() { return MakeError(ErrorCode::Invalid, "invalid timestamp '" + std::string(S.substr(0, 40)) + "'"); };
		int Y, Mo, D, H, Mi, Se;
		if (!Digits(S, 0, 4, Y) || S.size() < 20 || S[4] != '-' || !Digits(S, 5, 2, Mo) || S[7] != '-' || !Digits(S, 8, 2, D)
			|| (S[10] != 'T' && S[10] != 't') || !Digits(S, 11, 2, H) || S[13] != ':' || !Digits(S, 14, 2, Mi) || S[16] != ':'
			|| !Digits(S, 17, 2, Se))
		{
			return Bad();
		}
		if (Mo < 1 || Mo > 12 || D < 1 || D > 31 || H > 23 || Mi > 59 || Se > 60)
		{
			return Bad();
		}
		size_t Pos = 19;
		int64_t Ms = 0;
		if (Pos < S.size() && S[Pos] == '.')
		{
			++Pos;
			int Count = 0;
			while (Pos < S.size() && S[Pos] >= '0' && S[Pos] <= '9')
			{
				if (Count < 3)
				{
					Ms = Ms * 10 + (S[Pos] - '0');
				}
				++Count;
				++Pos;
			}
			if (Count == 0) return Bad();
			for (int i = Count; i < 3; ++i) Ms *= 10;
		}
		int64_t OffsetMin = 0;
		if (Pos < S.size() && (S[Pos] == 'Z' || S[Pos] == 'z'))
		{
			++Pos;
		}
		else if (Pos < S.size() && (S[Pos] == '+' || S[Pos] == '-'))
		{
			int Oh, Om;
			if (!Digits(S, Pos + 1, 2, Oh) || Pos + 3 >= S.size() || S[Pos + 3] != ':' || !Digits(S, Pos + 4, 2, Om)) return Bad();
			OffsetMin = (S[Pos] == '-' ? -1 : 1) * (Oh * 60 + Om);
			Pos += 6;
		}
		else
		{
			return Bad();
		}
		if (Pos != S.size()) return Bad();
		const int64_t Days = DaysFromCivil(Y, static_cast<unsigned>(Mo), static_cast<unsigned>(D));
		return Days * 86400000 + (int64_t(H) * 3600 + Mi * 60 + Se) * 1000 + Ms - OffsetMin * 60000;
	}
}
