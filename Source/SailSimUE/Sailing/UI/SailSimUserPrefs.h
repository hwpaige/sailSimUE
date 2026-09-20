#pragma once

#include "CoreMinimal.h"

class ASailBoatPawn;
class UWorld;

/**
 * Persistent user preferences (sail trim, wind, sky, map size).
 * Stored under Saved/Config/SailSimUserPrefs.ini so values survive editor/game restarts.
 */
struct FSailSimUserPrefs
{
	/** True wind in DISPLAY knots (calibrated feel scale; see FBoatDynamics::WindDisplayFromPhysics). */
	float TwsKn = 12.f;
	float TwdDeg = 225.f;
	// --- Sail / helm (persist across runs) ---
	float Outhaul01 = 0.94f;
	float Vang01 = 0.40f;
	float SheetEase01 = 0.25f; // main sheet 0 hard … 1 eased
	float SpinSheet01 = 0.18f;
	float JibCar01 = 0.45f;
	/** Jib default set (up) — douse with helm button if needed. */
	bool bJibSet = true;
	/** Kite default doused — arm with helm button when wanted. */
	bool bKiteSet = false;
	float Cloud01 = 1.f;
	float Fog01 = 1.f;
	/** Local solar hours 0..24; 12 = Fair Day (captured map look). */
	float TimeOfDayHours = 12.f;
	/** Calendar season 0..1 (0 winter, 0.25 spring, 0.5 summer, 0.75 autumn). */
	float Season01 = 0.5f;
	float MapPanelW = 424.f;
	float MapPanelH = 241.f;
	int32 EnvPreset = 0;
	bool bLightsPanelOpen = true;

	static FString ConfigPath();
	void Load();
	void Save() const;

	/** Apply wind / trim / sky to the live boat + ocean subsystem. */
	void Apply(ASailBoatPawn* Boat, UWorld* World) const;
};
