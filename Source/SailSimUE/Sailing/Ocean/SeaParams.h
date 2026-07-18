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
	/**
	 * Wave height scale (cm). Kept for API compatibility; flat-ocean MVP ignores this
	 * (Gerstner / procedural swell disabled).
	 */
	float AmplitudeCm = 0.f;
	/** 0 = long swell, 1 = short chop (unused while flat). */
	float Choppiness = 0.f;
	/** Dominant wave direction (unused while flat). */
	float WaveDirDeg = 45.f;
	/** Seconds; reserved for future spectra. */
	float TimeSec = 0.f;

	static FSeaParams DefaultOpenOcean()
	{
		FSeaParams P;
		P.WindSpeedKn = 16.f;
		P.WindDirDeg = 225.f;
		P.WaveDirDeg = 45.f;
		P.AmplitudeCm = 0.f; // flat plane
		P.Choppiness = 0.f;
		return P;
	}
};
