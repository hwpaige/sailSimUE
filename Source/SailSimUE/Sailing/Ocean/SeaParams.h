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
	 * Wave height scale (cm). Reserved for procedural blend; primary displacement is
	 * Water Body Gerstner (EnsureGerstnerWaterWaves).
	 */
	float AmplitudeCm = 0.f;
	/** 0 = long swell, 1 = short chop (material chop path). */
	float Choppiness = 0.f;
	/** Dominant wave direction (met FROM); wind angle fed into Gerstner when retuned. */
	float WaveDirDeg = 45.f;
	/** Seconds; reserved for future spectra. */
	float TimeSec = 0.f;

	static FSeaParams DefaultOpenOcean()
	{
		FSeaParams P;
		P.WindSpeedKn = 16.f;
		P.WindDirDeg = 225.f;
		P.WaveDirDeg = 45.f;
		P.AmplitudeCm = 0.f; // visual Gerstner from WaterWaves asset
		P.Choppiness = 0.f;
		return P;
	}
};
