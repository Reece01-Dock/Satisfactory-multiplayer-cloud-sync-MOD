#pragma once

// Isolated access to Satisfactory internals that have no documented / exported
// public API. Everything here is a version-sensitive workaround: it validates
// what it touches, refuses (with a diagnostic) instead of guessing, and is the
// only place that needs revisiting after a game update. See
// docs/ficsit-compatibility.md for the list and the audited game build.

#include "CoreMinimal.h"

class UWorld;

namespace SharedWorldShim
{
	/**
	 * Makes the game's selected session type one that creates an online session,
	 * so UFGSaveSystem::LoadSaveFile hosts a joinable listen game instead of
	 * SessionDef_SinglePlayer.
	 *
	 * Why a shim: UFGSessionSettings::SetSessionDefinition / ApplySettingsModel go
	 * through UFGSessionSettingsModel, which is not FACTORYGAME_API, so a mod cannot
	 * link it. No documented replacement found (docs.ficsit.app was unreachable
	 * during the audit; checked against the headers vendored with SML 3.12).
	 *
	 * Writes UFGSessionSettings::mCurrentSessionDefinition (+ mSessionDefinitionName
	 * when present) by reflection after checking property types; the write is verified
	 * through the public getter and rolled back if it did not take.
	 * OutError explains a failure in one line.
	 */
	bool EnsureHostingSessionDefinition(UWorld* MenuWorld, FString* OutError = nullptr);

	/** Logs (once per session) when the running game build differs from the audited one. */
	void WarnIfUnauditedGameBuild();
}
