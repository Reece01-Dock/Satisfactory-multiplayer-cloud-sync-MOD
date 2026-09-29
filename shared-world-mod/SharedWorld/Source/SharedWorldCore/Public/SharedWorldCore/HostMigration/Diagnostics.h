#pragma once
// Development diagnostics snapshot for host ranking / migration UI.

#include <algorithm>
#include <string>
#include <vector>

#include "SharedWorldCore/HostMigration/HostMigration.h"

namespace sw
{
	/** Fill from a HostMigrationEngine for the in-game debug overlay. */
	inline MigrationDiagnostics MakeDiagnosticsSnapshot(const HostMigrationEngine& Engine)
	{
		return Engine.Diagnostics();
	}

	inline std::string FormatDiagnosticsText(const MigrationDiagnostics& D)
	{
		std::string Out;
		Out += "CURRENT HOST\n";
		Out += (D.CurrentHost.DisplayName.empty() ? D.CurrentHost.PlayerId : D.CurrentHost.DisplayName) + "\n\n";
		Out += "GENERATION\n" + std::to_string(D.Generation) + "\n\n";
		Out += "REVISION\n" + std::to_string(D.Revision) + "\n\n";
		Out += "Migration state:\n";
		Out += ToString(D.Phase);
		Out += "\n\nPreferred successor:\n";
		if (D.PreferredSuccessor) Out += D.PreferredSuccessor->DisplayName.empty() ? D.PreferredSuccessor->PlayerId : D.PreferredSuccessor->DisplayName;
		else Out += "(none)";
		Out += "\n\nHOST CANDIDATES\n";
		for (size_t i = 0; i < D.Ranking.size(); ++i)
		{
			const RankedHost& R = D.Ranking[i];
			Out += std::to_string(i + 1) + ". " + (R.Who.DisplayName.empty() ? R.Who.PlayerId : R.Who.DisplayName) + "\n";
			Out += "   Score: " + std::to_string(R.Score.Total / 1000) + "\n";
			Out += "   Avg RTT: " + std::to_string(R.Network.AvgRttMs) + "ms\n";
			Out += "   Worst RTT: " + std::to_string(R.Network.WorstRttMs) + "ms\n";
			Out += "   Loss: " + std::to_string(R.Network.AvgLossBp / 100) + "." + std::to_string((R.Network.AvgLossBp / 10) % 10) + "%\n";
			Out += "   Jitter: " + std::to_string(R.Network.AvgJitterMs) + "ms\n";
			Out += "   Save Ready: ";
			Out += (R.Score.SaveReady > 0 ? "YES" : "NO");
			Out += "\n   Storage Ready: ";
			Out += (R.Score.StorageReady > 0 ? "YES" : "NO");
			Out += "\n   Session Ready: ";
			Out += (R.Score.SessionReady > 0 ? "YES" : "NO");
			Out += "\n";
		}
		if (!D.PlayerIds.empty() && !D.RttGridMs.empty())
		{
			Out += "\nPEER LATENCY MATRIX (ms)\n";
			Out += "         ";
			for (const std::string& Id : D.PlayerIds) Out += Id.substr(0, std::min<size_t>(Id.size(), 6)) + " ";
			Out += "\n";
			for (size_t R = 0; R < D.PlayerIds.size(); ++R)
			{
				Out += D.PlayerIds[R].substr(0, std::min<size_t>(D.PlayerIds[R].size(), 8));
				Out += " ";
				for (size_t C = 0; C < D.PlayerIds.size(); ++C)
				{
					if (R == C) Out += "   -  ";
					else Out += std::to_string(D.RttGridMs[R][C]) + " ";
				}
				Out += "\n";
			}
		}
		if (!D.OverlayMessage.empty())
		{
			Out += "\nOverlay:\n";
			Out += D.OverlayMessage;
			Out += "\n";
		}
		return Out;
	}
}
