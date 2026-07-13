#pragma once

#include "CoreMinimal.h"
#include "Sailing/Ocean/IOceanHeightSampler.h"

/**
 * Compatibility facade — prefer USailOceanSubsystem.
 * Returns world-space samples (boat moves through the field).
 */
struct FOceanHeightSample
{
	static FOceanSample SampleAt(
		const UWorld* World,
		const FVector& WorldXY,
		float PreferZ = 0.f,
		bool bUsePreferZ = false);
};
