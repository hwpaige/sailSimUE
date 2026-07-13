#pragma once

#include "CoreMinimal.h"

/**
 * Sea state controls (Phase 5).
 * Units: wind kn, directions deg met convention (from), amplitude in cm.
 * Maps to water-pro / Angular wave sliders conceptually — not their shaders.
 */
struct FSeaParams
{
	float WindSpeedKn = 18.f;
	/** Meteorological: direction wind blows FROM (deg). */
	float WindDirDeg = 225.f;
	/** Overall wave height scale (cm peak amplitude ballpark for Gerstner). */
	float AmplitudeCm = 40.f;
	/** 0 = long swell, 1 = short chop. */
	float Choppiness = 0.45f;
	/** Dominant wave direction (propagation, usually wind + 180). */
	float WaveDirDeg = 45.f;
	/** Seconds; used by procedural spectra. */
	float TimeSec = 0.f;

	static FSeaParams DefaultOpenOcean()
	{
		FSeaParams P;
		P.WindSpeedKn = 18.f;
		P.WindDirDeg = 225.f;
		P.WaveDirDeg = 45.f;
		P.AmplitudeCm = 45.f;
		P.Choppiness = 0.5f;
		return P;
	}
};
