#pragma once

#include "CoreMinimal.h"
#include "Sailing/BoatSpec.h"

/**
 * Phase 3.3: call Python sail_geom oracle to re-loft a boat into Content/Data/live_boat3d.json.
 * Development-time only (requires python3 + sail-sim backend next to SailSimUE).
 */
struct FBoatLoftOracle
{
	/** Relative Content path for the live loft JSON. */
	static FString LiveBoatJsonRelative() { return TEXT("Data/live_boat3d.json"); }

	/** Write live_spec.json + run Scripts/export_boat3d_live.py. Returns true on success. */
	static bool RebuildLiveLoft(const FBoatSpec& Spec, FString* OutError = nullptr);

	/** Absolute path to Content/Data/live_boat3d.json if it exists. */
	static bool LiveLoftExists();
};
