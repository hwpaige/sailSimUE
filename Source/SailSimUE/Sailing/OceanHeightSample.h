#pragma once

#include "CoreMinimal.h"

/** Single ocean surface query result (Phase 5 will back this with FFT). */
struct FOceanSample
{
	FVector Surface = FVector::ZeroVector;
	FVector Normal = FVector::UpVector;
	float Depth = 0.f;
	bool bValid = false;
};

/**
 * Abstract height sample so buoyancy / float never hardcode WaterBody vs FFT.
 * Current backend: UE Water plugin Gerstner ocean bodies.
 */
struct FOceanHeightSample
{
	/** Best surface near WorldXY among all water bodies in World. */
	static FOceanSample SampleAt(
		const UWorld* World,
		const FVector& WorldXY,
		float PreferZ = 0.f,
		bool bUsePreferZ = false);
};
